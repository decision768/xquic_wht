#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xquic/netins_udp.h>
#include "netins_udp_v2_internal.h"
#include "netins_udp_v2_transport.h"

struct netins_udp_client_s {
    int state;
    int error_code;
    char error_message[NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN];
    uint32_t close_timeout_ms;
    uint32_t events;
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;
};

struct netins_udp_server_s {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int state;
    int accepting;
    int error_code;
    char error_message[NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN];
    uint32_t events;
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;
};

struct netins_udp_server_connection_s {
    int state;
    int error_code;
    char error_message[NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN];
    struct sockaddr_storage peer_address;
    socklen_t peer_address_len;
    uint64_t shutdown_error;
    uint32_t events;
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;
    netins_udp_v2_transport_snapshot_t snapshot;
};

struct netins_udp_h3_request_s {
    int server_side;
    int lane_index;
    int error_code;
    int send_finished;
    netins_udp_h3_request_config_t config;
    netins_udp_h3_response_config_t response;
    netins_udp_v2_notifier_t *parent_notifier;
    uint32_t events;
    int notify_fds[2];
    unsigned char tx_wire[1024];
    size_t tx_wire_length;
    unsigned char rx_wire[1024];
    size_t rx_wire_length;
    size_t rx_wire_offset;
    int rx_wire_built;
};

static netins_udp_v2_transport_config_t g_last_config;
static netins_udp_v2_transport_server_config_t g_last_server_config;
static netins_udp_v2_transport_client_t *g_last_client;
static netins_udp_v2_transport_server_t *g_last_server;
static netins_udp_v2_transport_server_connection_t *g_last_connection;
static int g_create_count;
static int g_close_count;
static int g_server_create_count;
static int g_server_close_count;
static int g_connection_close_count;
static int g_server_accept_ready;
static int g_server_accept_entered;
static int g_start_result;
static int g_wait_result;
static pthread_mutex_t g_wait_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wait_cond = PTHREAD_COND_INITIALIZER;
static int g_block_wait;
static int g_wait_entered;
static int g_close_requested;
static int g_block_start;
static int g_start_entered;
static int g_start_continue;
static int g_block_get_state;
static int g_get_state_entered;
static int g_get_state_continue;
static int g_pending_resolvers;
static int g_log_count[4];
static char g_last_log_message[NETINS_UDP_ERROR_MSG_LEN + 32];
static netins_udp_h3_request_t *g_last_data_request;
static int g_data_request_open_count;
static int g_data_request_accept_count;
static int g_data_request_close_count;
static unsigned char g_data_sent[1024];
static size_t g_data_sent_length;
static unsigned char g_data_received[256];
static size_t g_data_received_length;
static size_t g_data_received_offset;

static uint32_t
test_get_u32(const unsigned char *input)
{
    return ((uint32_t)input[0] << 24)
        | ((uint32_t)input[1] << 16)
        | ((uint32_t)input[2] << 8)
        | (uint32_t)input[3];
}

static uint64_t
test_get_u64(const unsigned char *input)
{
    uint64_t value = 0;
    for (unsigned int i = 0; i < 8; ++i) {
        value = (value << 8) | input[i];
    }
    return value;
}

static void
test_put_u16(unsigned char *output, uint16_t value)
{
    output[0] = (unsigned char)(value >> 8);
    output[1] = (unsigned char)value;
}

static void
test_put_u32(unsigned char *output, uint32_t value)
{
    output[0] = (unsigned char)(value >> 24);
    output[1] = (unsigned char)(value >> 16);
    output[2] = (unsigned char)(value >> 8);
    output[3] = (unsigned char)value;
}

static void
test_put_u64(unsigned char *output, uint64_t value)
{
    for (unsigned int i = 0; i < 8; ++i) {
        output[i] = (unsigned char)(value >> (56U - 8U * i));
    }
}

static void
test_build_frame(unsigned char *output, int lane, int fin,
    uint64_t offset, const unsigned char *data, uint32_t length)
{
    test_put_u32(output, 0x4e554432U);
    output[4] = 1;
    output[5] = fin ? 1 : 0;
    test_put_u16(output + 6, (uint16_t)lane);
    test_put_u64(output + 8, offset);
    test_put_u32(output + 16, length);
    test_put_u32(output + 20, 0);
    if (length != 0) {
        memcpy(output + 24, data, length);
    }
}

static void
test_request_init_notify(netins_udp_h3_request_t *request)
{
    request->notify_fds[0] = -1;
    request->notify_fds[1] = -1;
    assert(pipe(request->notify_fds) == 0);
    int flags = fcntl(request->notify_fds[0], F_GETFL, 0);
    assert(flags >= 0);
    assert(fcntl(request->notify_fds[0], F_SETFL,
        flags | O_NONBLOCK) == 0);
    unsigned char byte = 1;
    assert(write(request->notify_fds[1], &byte, 1) == 1);
}

static void
test_request_parse_sent(netins_udp_h3_request_t *request)
{
    while (request->tx_wire_length >= 24) {
        uint32_t payload_length = test_get_u32(request->tx_wire + 16);
        size_t frame_length = 24U + payload_length;
        if (request->tx_wire_length < frame_length) {
            return;
        }
        assert(test_get_u32(request->tx_wire) == 0x4e554432U);
        uint64_t offset = test_get_u64(request->tx_wire + 8);
        if ((request->tx_wire[5] & 1U) == 0) {
            assert(offset + payload_length <= sizeof(g_data_sent));
            memcpy(g_data_sent + (size_t)offset,
                request->tx_wire + 24, payload_length);
            if (g_data_sent_length < offset + payload_length) {
                g_data_sent_length = (size_t)(offset + payload_length);
            }
        }
        memmove(request->tx_wire,
            request->tx_wire + frame_length,
            request->tx_wire_length - frame_length);
        request->tx_wire_length -= frame_length;
    }
}

static void
test_request_build_received(netins_udp_h3_request_t *request)
{
    if (request->rx_wire_built) {
        return;
    }
    request->rx_wire_built = 1;
    if (request->lane_index == 0 && g_data_received_length != 0) {
        test_build_frame(request->rx_wire, request->lane_index, 0, 0,
            g_data_received, (uint32_t)g_data_received_length);
        request->rx_wire_length = 24 + g_data_received_length;
    }
    test_build_frame(request->rx_wire + request->rx_wire_length,
        request->lane_index, 1, g_data_received_length, NULL, 0);
    request->rx_wire_length += 24;
}

void
netins_udp_h3_request_config_init(
    netins_udp_h3_request_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->api_version = NETINS_UDP_API_VERSION;
    strcpy(config->method, "GET");
    strcpy(config->path, "/");
    strcpy(config->content_type, "application/octet-stream");
}

void
netins_udp_h3_response_config_init(
    netins_udp_h3_response_config_t *config)
{
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->api_version = NETINS_UDP_API_VERSION;
    config->status = 200;
    strcpy(config->content_type, "application/octet-stream");
}

int
netins_udp_h3_request_open(netins_udp_t *netins,
    const netins_udp_h3_request_config_t *config,
    netins_udp_h3_request_t **out_request)
{
    (void)netins;
    netins_udp_h3_request_t *request = calloc(1, sizeof(*request));
    assert(request != NULL);
    request->config = *config;
    const char *lane = strrchr(config->path, '/');
    assert(lane != NULL && lane[1] >= '0' && lane[1] <= '7');
    request->lane_index = lane[1] - '0';
    test_request_init_notify(request);
    request->events =
        NETINS_UDP_EVENT_READABLE | NETINS_UDP_EVENT_WRITABLE;
    *out_request = request;
    g_last_data_request = request;
    g_data_request_open_count++;
    return NETINS_UDP_OK;
}

int
netins_udp_h3_request_accept(netins_udp_t *netins,
    netins_udp_h3_request_t **out_request)
{
    (void)netins;
    netins_udp_h3_request_t *request = calloc(1, sizeof(*request));
    assert(request != NULL);
    request->server_side = 1;
    request->lane_index = g_data_request_accept_count % 8;
    test_request_init_notify(request);
    netins_udp_h3_request_config_init(&request->config);
    strcpy(request->config.method, "POST");
    snprintf(request->config.path, sizeof(request->config.path),
        "/netins/udp/stream/v2/%d", request->lane_index);
    strcpy(request->config.content_type,
        "application/x-netins-udp-stream-v2");
    request->config.content_length =
        NETINS_UDP_CONTENT_LENGTH_UNKNOWN;
    request->events =
        NETINS_UDP_EVENT_READABLE | NETINS_UDP_EVENT_WRITABLE;
    *out_request = request;
    g_last_data_request = request;
    g_data_request_accept_count++;
    return NETINS_UDP_OK;
}

int
netins_udp_v2_h3_request_open_internal(netins_udp_t *netins,
    const netins_udp_h3_request_config_t *config,
    netins_udp_h3_request_t **out_request)
{
    return netins_udp_h3_request_open(netins, config, out_request);
}

int
netins_udp_v2_h3_request_accept_internal(netins_udp_t *netins,
    netins_udp_h3_request_t **out_request)
{
    return netins_udp_h3_request_accept(netins, out_request);
}

int
netins_udp_v2_h3_request_accept_internal_dontwait(
    netins_udp_t *netins, netins_udp_h3_request_t **out_request)
{
    return netins_udp_h3_request_accept(netins, out_request);
}

int
netins_udp_h3_request_set_response(
    netins_udp_h3_request_t *request,
    const netins_udp_h3_response_config_t *config)
{
    request->response = *config;
    return NETINS_UDP_OK;
}

ssize_t
netins_udp_h3_request_send(netins_udp_h3_request_t *request,
    const void *data, size_t length, int flags)
{
    if ((flags & ~(NETINS_UDP_MSG_DONTWAIT | NETINS_UDP_MSG_FIN)) != 0
        || (length > 0 && data == NULL))
    {
        request->error_code = EINVAL;
        errno = EINVAL;
        return -1;
    }
    size_t available = sizeof(request->tx_wire)
        - request->tx_wire_length;
    size_t accepted = length < available ? length : available;
    if (accepted != 0) {
        memcpy(request->tx_wire + request->tx_wire_length,
            data, accepted);
        request->tx_wire_length += accepted;
        test_request_parse_sent(request);
    }
    if ((flags & NETINS_UDP_MSG_FIN) != 0 && accepted == length) {
        request->send_finished = 1;
    }
    request->error_code = 0;
    return (ssize_t)accepted;
}

ssize_t
netins_udp_h3_request_recv(netins_udp_h3_request_t *request,
    void *data, size_t length, int flags)
{
    if ((flags & ~(NETINS_UDP_MSG_DONTWAIT | NETINS_UDP_MSG_PEEK)) != 0
        || (length > 0 && data == NULL))
    {
        request->error_code = EINVAL;
        errno = EINVAL;
        return -1;
    }
    test_request_build_received(request);
    size_t remaining = request->rx_wire_length
        - request->rx_wire_offset;
    size_t delivered = length < remaining ? length : remaining;
    if (delivered > 0) {
        memcpy(data, request->rx_wire + request->rx_wire_offset,
            delivered);
        if ((flags & NETINS_UDP_MSG_PEEK) == 0) {
            request->rx_wire_offset += delivered;
        }
    }
    request->error_code = 0;
    return (ssize_t)delivered;
}

int
netins_udp_h3_request_get_info(netins_udp_h3_request_t *request,
    netins_udp_h3_request_info_t *out)
{
    *out = (netins_udp_h3_request_info_t)
        NETINS_UDP_H3_REQUEST_INFO_INITIALIZER;
    strcpy(out->method, request->config.method);
    strcpy(out->path, request->config.path);
    strcpy(out->request_content_type, request->config.content_type);
    out->request_content_length = request->config.content_length;
    out->request_headers_ready = 1;
    if (!request->server_side) {
        out->response_headers_ready = 1;
        out->response_status = 200;
        out->response_content_length =
            NETINS_UDP_CONTENT_LENGTH_UNKNOWN;
        strcpy(out->response_content_type,
            "application/x-netins-udp-stream-v2");
    }
    return NETINS_UDP_OK;
}

int
netins_udp_h3_request_get_last_error_code(
    netins_udp_h3_request_t *request)
{
    return request->error_code;
}

int
netins_udp_h3_request_get_last_error_msg(
    netins_udp_h3_request_t *request, char *buffer, size_t buffer_len)
{
    snprintf(buffer, buffer_len, "%s",
        request->error_code == 0 ? "" : "mock data request error");
    return NETINS_UDP_OK;
}

int
netins_udp_h3_request_get_events(netins_udp_h3_request_t *request,
    uint32_t *out_events)
{
    *out_events = request->events;
    return NETINS_UDP_OK;
}

int
netins_udp_h3_request_get_fd(netins_udp_h3_request_t *request)
{
    return request->notify_fds[0];
}

void
netins_udp_v2_h3_request_set_parent_notifier(
    netins_udp_h3_request_t *request,
    netins_udp_v2_notifier_t *parent_notifier)
{
    request->parent_notifier = parent_notifier;
}

int
netins_udp_v2_h3_request_finish_send(
    netins_udp_h3_request_t *request)
{
    return netins_udp_h3_request_send(request, NULL, 0,
        NETINS_UDP_MSG_FIN) == 0 ? NETINS_UDP_OK : NETINS_UDP_ERROR;
}

void
netins_udp_h3_request_close(netins_udp_h3_request_t *request)
{
    if (request == NULL) {
        return;
    }
    if (g_last_data_request == request) {
        g_last_data_request = NULL;
    }
    close(request->notify_fds[0]);
    close(request->notify_fds[1]);
    free(request);
    g_data_request_close_count++;
}

static void
test_logger(int level, const char *message)
{
    assert(level >= NETINS_UDP_LOG_LEVEL_DEBUG);
    assert(level <= NETINS_UDP_LOG_LEVEL_ERROR);
    assert(message != NULL);
    g_log_count[level]++;
    snprintf(g_last_log_message, sizeof(g_last_log_message), "%s",
        message);
}

int
netins_udp_v2_transport_has_pending_resolvers(void)
{
    return g_pending_resolvers;
}

int
netins_udp_v2_transport_client_create(
    const netins_udp_v2_transport_config_t *config,
    netins_udp_v2_transport_client_t **out_client)
{
    g_last_config = *config;
    netins_udp_v2_transport_client_t *client = calloc(1, sizeof(*client));
    assert(client != NULL);
    client->state = NETINS_UDP_V2_TRANSPORT_NEW;
    client->close_timeout_ms = config->close_timeout_ms;
    client->notify = config->notify;
    client->notify_user_data = config->notify_user_data;
    *out_client = client;
    g_last_client = client;
    g_create_count++;
    return 0;
}

int
netins_udp_v2_transport_client_start(
    netins_udp_v2_transport_client_t *client)
{
    pthread_mutex_lock(&g_wait_lock);
    if (g_block_start) {
        g_start_entered = 1;
        pthread_cond_broadcast(&g_wait_cond);
        while (!g_start_continue) {
            pthread_cond_wait(&g_wait_cond, &g_wait_lock);
        }
    }
    pthread_mutex_unlock(&g_wait_lock);
    if (g_start_result != 0) {
        client->state = NETINS_UDP_V2_TRANSPORT_ERROR;
        client->error_code = EIO;
        strcpy(client->error_message, "mock start failed");
        errno = EIO;
        return -1;
    }
    client->state = NETINS_UDP_V2_TRANSPORT_CONNECTING;
    return 0;
}

int
netins_udp_v2_transport_client_wait_connected(
    netins_udp_v2_transport_client_t *client, uint32_t timeout_ms)
{
    assert(timeout_ms != 0);
    pthread_mutex_lock(&g_wait_lock);
    if (g_block_wait) {
        g_wait_entered = 1;
        pthread_cond_broadcast(&g_wait_cond);
        while (!g_close_requested) {
            pthread_cond_wait(&g_wait_cond, &g_wait_lock);
        }
        client->state = NETINS_UDP_V2_TRANSPORT_ERROR;
        client->error_code = ECANCELED;
        strcpy(client->error_message, "mock connect canceled");
        pthread_mutex_unlock(&g_wait_lock);
        errno = ECANCELED;
        return -1;
    }
    pthread_mutex_unlock(&g_wait_lock);
    if (g_wait_result != 0) {
        client->state = NETINS_UDP_V2_TRANSPORT_ERROR;
        client->error_code = ETIMEDOUT;
        strcpy(client->error_message, "mock handshake timeout");
        errno = ETIMEDOUT;
        return -1;
    }
    client->state = NETINS_UDP_V2_TRANSPORT_CONNECTED;
    client->events |= NETINS_UDP_V2_EVENT_CONNECTED;
    if (client->notify != NULL) {
        client->notify(client->notify_user_data,
            NETINS_UDP_V2_EVENT_CONNECTED);
    }
    return 0;
}

int
netins_udp_v2_transport_client_get_state(
    netins_udp_v2_transport_client_t *client)
{
    int state = client->state;
    pthread_mutex_lock(&g_wait_lock);
    if (g_block_get_state) {
        g_get_state_entered = 1;
        pthread_cond_broadcast(&g_wait_cond);
        while (!g_get_state_continue) {
            pthread_cond_wait(&g_wait_cond, &g_wait_lock);
        }
    }
    pthread_mutex_unlock(&g_wait_lock);
    return state;
}

int
netins_udp_v2_transport_client_get_last_error_code(
    netins_udp_v2_transport_client_t *client)
{
    return client->error_code;
}

int
netins_udp_v2_transport_client_get_last_error_msg(
    netins_udp_v2_transport_client_t *client,
    char *buffer, size_t buffer_len)
{
    snprintf(buffer, buffer_len, "%s", client->error_message);
    return 0;
}

int
netins_udp_v2_transport_client_get_snapshot(
    netins_udp_v2_transport_client_t *client,
    netins_udp_v2_transport_snapshot_t *out)
{
    memset(out, 0, sizeof(*out));
    out->state = client->state;
    out->connection_id = 101;
    out->handshake_finished =
        client->state == NETINS_UDP_V2_TRANSPORT_CONNECTED;
    out->multipath_mode = 1;
    out->multipath_active = 1;
    out->path_count = 2;
    out->active_path_count = 2;
    out->bytes_sent = 1000;
    out->bytes_received = 2000;
    out->paths[0].path_id = 10;
    out->paths[0].path_index = 0;
    out->paths[0].path_state = 2;
    out->paths[0].bytes_sent = 400;
    out->paths[1].path_id = 20;
    out->paths[1].path_index = 1;
    out->paths[1].path_state = 2;
    out->paths[1].bytes_sent = 600;
    return 0;
}

int
netins_udp_v2_transport_client_get_events(
    netins_udp_v2_transport_client_t *client, uint32_t *out_events)
{
    *out_events = client->events;
    return 0;
}

void
netins_udp_v2_transport_client_set_notify(
    netins_udp_v2_transport_client_t *client,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    client->notify = notify;
    client->notify_user_data = user_data;
}

void
netins_udp_v2_transport_client_set_close_timeout(
    netins_udp_v2_transport_client_t *client, uint32_t close_timeout_ms)
{
    client->close_timeout_ms = close_timeout_ms;
}

void
netins_udp_v2_transport_client_request_close(
    netins_udp_v2_transport_client_t *client)
{
    pthread_mutex_lock(&g_wait_lock);
    g_close_requested = 1;
    if (client->state != NETINS_UDP_V2_TRANSPORT_ERROR) {
        client->state = NETINS_UDP_V2_TRANSPORT_CLOSING;
    }
    pthread_cond_broadcast(&g_wait_cond);
    pthread_mutex_unlock(&g_wait_lock);
}

void
netins_udp_v2_transport_client_request_shutdown(
    netins_udp_v2_transport_client_t *client, uint64_t application_error)
{
    (void)application_error;
    netins_udp_v2_transport_client_request_close(client);
}

void
netins_udp_v2_transport_client_close(
    netins_udp_v2_transport_client_t *client)
{
    if (g_last_client == client) {
        g_last_client = NULL;
    }
    free(client);
    g_close_count++;
}

int
netins_udp_v2_transport_server_create(
    const netins_udp_v2_transport_server_config_t *config,
    netins_udp_v2_transport_server_t **out_server)
{
    netins_udp_v2_transport_server_t *server = calloc(1, sizeof(*server));
    assert(server != NULL);
    assert(pthread_mutex_init(&server->lock, NULL) == 0);
    assert(pthread_cond_init(&server->cond, NULL) == 0);
    server->state = NETINS_UDP_V2_TRANSPORT_LISTENING;
    server->accepting = 1;
    server->notify = config->notify;
    server->notify_user_data = config->notify_user_data;
    g_last_server_config = *config;
    g_last_server = server;
    g_server_create_count++;
    *out_server = server;
    return 0;
}

int
netins_udp_v2_transport_server_get_state(
    netins_udp_v2_transport_server_t *server)
{
    pthread_mutex_lock(&server->lock);
    int state = server->state;
    pthread_mutex_unlock(&server->lock);
    return state;
}

int
netins_udp_v2_transport_server_get_last_error_code(
    netins_udp_v2_transport_server_t *server)
{
    return server->error_code;
}

int
netins_udp_v2_transport_server_get_last_error_msg(
    netins_udp_v2_transport_server_t *server,
    char *buffer, size_t buffer_len)
{
    snprintf(buffer, buffer_len, "%s", server->error_message);
    return 0;
}

int
netins_udp_v2_transport_server_get_events(
    netins_udp_v2_transport_server_t *server, uint32_t *out_events)
{
    *out_events = server->events;
    return 0;
}

void
netins_udp_v2_transport_server_set_notify(
    netins_udp_v2_transport_server_t *server,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    server->notify = notify;
    server->notify_user_data = user_data;
}

int
netins_udp_v2_transport_server_accept(
    netins_udp_v2_transport_server_t *server,
    uint32_t timeout_ms, int wait_forever,
    netins_udp_v2_transport_server_connection_t **out_connection)
{
    pthread_mutex_lock(&server->lock);
    g_server_accept_entered = 1;
    pthread_cond_broadcast(&server->cond);
    while (server->accepting && !g_server_accept_ready && wait_forever) {
        pthread_cond_wait(&server->cond, &server->lock);
    }
    if (!server->accepting) {
        pthread_mutex_unlock(&server->lock);
        errno = ECANCELED;
        return -1;
    }
    if (!g_server_accept_ready) {
        pthread_mutex_unlock(&server->lock);
        errno = timeout_ms > 0 ? ETIMEDOUT : EAGAIN;
        return -1;
    }
    g_server_accept_ready = 0;
    server->events &= ~NETINS_UDP_V2_EVENT_ACCEPTABLE;
    pthread_mutex_unlock(&server->lock);

    netins_udp_v2_transport_server_connection_t *connection =
        calloc(1, sizeof(*connection));
    assert(connection != NULL);
    connection->state = NETINS_UDP_V2_TRANSPORT_CONNECTED;
    struct sockaddr_in *peer =
        (struct sockaddr_in *)&connection->peer_address;
    peer->sin_family = AF_INET;
    peer->sin_port = htons(43210);
    peer->sin_addr.s_addr = htonl(0xc000020aU);
    connection->peer_address_len = sizeof(*peer);
    connection->snapshot.connection_id = 202;
    connection->snapshot.state = NETINS_UDP_V2_TRANSPORT_CONNECTED;
    connection->snapshot.handshake_finished = 1;
    connection->snapshot.path_count = 1;
    connection->snapshot.active_path_count = 1;
    connection->snapshot.paths[0].path_id = 30;
    connection->snapshot.paths[0].path_state = 2;
    g_last_connection = connection;
    *out_connection = connection;
    return 0;
}

void
netins_udp_v2_transport_server_begin_close(
    netins_udp_v2_transport_server_t *server)
{
    pthread_mutex_lock(&server->lock);
    server->accepting = 0;
    server->state = NETINS_UDP_V2_TRANSPORT_CLOSING;
    pthread_cond_broadcast(&server->cond);
    pthread_mutex_unlock(&server->lock);
}

void
netins_udp_v2_transport_server_close(
    netins_udp_v2_transport_server_t *server)
{
    netins_udp_v2_transport_server_begin_close(server);
    if (g_last_server == server) {
        g_last_server = NULL;
    }
    pthread_cond_destroy(&server->cond);
    pthread_mutex_destroy(&server->lock);
    free(server);
    g_server_close_count++;
}

int
netins_udp_v2_transport_server_connection_get_state(
    netins_udp_v2_transport_server_connection_t *connection)
{
    return connection->state;
}

int
netins_udp_v2_transport_server_connection_get_last_error_code(
    netins_udp_v2_transport_server_connection_t *connection)
{
    return connection->error_code;
}

int
netins_udp_v2_transport_server_connection_get_last_error_msg(
    netins_udp_v2_transport_server_connection_t *connection,
    char *buffer, size_t buffer_len)
{
    snprintf(buffer, buffer_len, "%s", connection->error_message);
    return 0;
}

int
netins_udp_v2_transport_server_connection_get_peer_address(
    netins_udp_v2_transport_server_connection_t *connection,
    struct sockaddr_storage *address, socklen_t *address_len)
{
    *address = connection->peer_address;
    *address_len = connection->peer_address_len;
    return 0;
}

int
netins_udp_v2_transport_server_connection_get_snapshot(
    netins_udp_v2_transport_server_connection_t *connection,
    netins_udp_v2_transport_snapshot_t *out)
{
    *out = connection->snapshot;
    return 0;
}

int
netins_udp_v2_transport_server_connection_get_events(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t *out_events)
{
    *out_events = connection->events;
    return 0;
}

void
netins_udp_v2_transport_server_connection_set_notify(
    netins_udp_v2_transport_server_connection_t *connection,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    connection->notify = notify;
    connection->notify_user_data = user_data;
    if (notify != NULL
        && connection->state == NETINS_UDP_V2_TRANSPORT_CONNECTED)
    {
        notify(user_data, NETINS_UDP_V2_EVENT_CONNECTED);
    }
}

void
netins_udp_v2_transport_server_connection_request_shutdown(
    netins_udp_v2_transport_server_connection_t *connection,
    uint64_t application_error)
{
    connection->shutdown_error = application_error;
    connection->state = NETINS_UDP_V2_TRANSPORT_CLOSING;
}

void
netins_udp_v2_transport_server_connection_close(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t timeout_ms)
{
    (void)timeout_ms;
    if (g_last_connection == connection) {
        g_last_connection = NULL;
    }
    free(connection);
    g_connection_close_count++;
}

void
netins_udp_v2_transport_server_connection_close_after_peer(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t timeout_ms)
{
    /*
     * The production transport waits for the peer's connection close before
     * releasing the accepted connection. This mock has no engine thread or
     * peer, so reuse the existing deterministic close implementation.
     */
    netins_udp_v2_transport_server_connection_close(connection, timeout_ms);
}

static void
set_http3_test_options(netins_udp_t *client)
{
    int protocol = NETINS_UDP_APP_HTTP3;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_APP_PROTOCOL,
        &protocol, sizeof(protocol)) == NETINS_UDP_OK);

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    tls.verify_peer = 0;
    strcpy(tls.server_name, "bad\rname");
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
        &tls, sizeof(tls)) == NETINS_UDP_ERROR);
    assert(errno == EPROTO);
    strcpy(tls.server_name, "test.example");
    strcpy(tls.ca_file, "/tmp/custom-ca.pem");
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_TLS,
        &tls, sizeof(tls)) == NETINS_UDP_OK);
}

static void
set_http3_server_options(netins_udp_t *listener)
{
    int protocol = NETINS_UDP_APP_HTTP3;
    assert(netins_udp_set_opt(listener, NETINS_UDP_OPT_APP_PROTOCOL,
        &protocol, sizeof(protocol)) == NETINS_UDP_OK);

    netins_udp_tls_config_t tls = NETINS_UDP_TLS_CONFIG_INITIALIZER;
    strcpy(tls.certificate_file, "/tmp/server.crt");
    strcpy(tls.private_key_file, "/tmp/server.key");
    assert(netins_udp_set_opt(listener, NETINS_UDP_OPT_TLS,
        &tls, sizeof(tls)) == NETINS_UDP_OK);
}

typedef struct connect_thread_result_s {
    netins_udp_t *client;
    int result;
    int error_code;
} connect_thread_result_t;

typedef struct accept_thread_result_s {
    netins_udp_t *listener;
    netins_udp_t *connection;
    int error_code;
} accept_thread_result_t;

typedef struct state_thread_result_s {
    netins_udp_t *connection;
    int state;
} state_thread_result_t;

static void *
connect_thread_main(void *argument)
{
    connect_thread_result_t *thread_result = argument;
    thread_result->result = netins_udp_connect(thread_result->client,
        "203.0.113.2", 443);
    thread_result->error_code = errno;
    return NULL;
}

static void *
accept_thread_main(void *argument)
{
    accept_thread_result_t *thread_result = argument;
    thread_result->connection = netins_udp_accept(
        thread_result->listener, NULL, NULL);
    thread_result->error_code = errno;
    return NULL;
}

static void *
state_thread_main(void *argument)
{
    state_thread_result_t *thread_result = argument;
    thread_result->state =
        netins_udp_get_state(thread_result->connection);
    return NULL;
}

int
main(void)
{
    errno = 0;
    assert(netins_udp_new(NETINS_UDP_TYPE_QUIC) == NULL);
    assert(errno == EPERM);
    assert(netins_udp_cleanup() == NETINS_UDP_ERROR);
    assert(errno == EPERM);

    assert(netins_udp_init() == NETINS_UDP_OK);
    errno = 0;
    assert(netins_udp_new(999) == NULL);
    assert(errno == EINVAL);

    netins_udp_t *client = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    assert(client != NULL);
    assert(netins_udp_set_logger(NULL, test_logger) == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    int log_level = -1;
    size_t log_level_len = sizeof(log_level);
    assert(netins_udp_get_opt(client, NETINS_UDP_OPT_LOG_LEVEL,
        &log_level, &log_level_len) == NETINS_UDP_OK);
    assert(log_level == NETINS_UDP_LOG_LEVEL_INFO);
    log_level = NETINS_UDP_LOG_LEVEL_ERROR + 1;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_LOG_LEVEL,
        &log_level, sizeof(log_level)) == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    log_level = NETINS_UDP_LOG_LEVEL_DEBUG;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_LOG_LEVEL,
        &log_level, sizeof(log_level)) == NETINS_UDP_OK);
    assert(netins_udp_set_logger(client, test_logger) == NETINS_UDP_OK);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_DEBUG] == 0);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_INFO] == 0);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_NEW);
    assert(netins_udp_cleanup() == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    assert(netins_udp_get_path_count(client, NULL) == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    assert(netins_udp_get_snapshot(client, NULL) == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    assert(netins_udp_get_info(client, NULL) == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    assert(netins_udp_get_statis(client, NULL) == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    assert(netins_udp_get_path_statis(client, 0, NULL)
        == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    assert(netins_udp_get_close_reason(client, NULL)
        == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    assert(netins_udp_get_events(client, NULL) == NETINS_UDP_ERROR);
    assert(errno == EINVAL);

    size_t option_len = 0;
    assert(netins_udp_get_opt(client, NETINS_UDP_OPT_APP_PROTOCOL,
        NULL, &option_len) == NETINS_UDP_OK);
    assert(option_len == sizeof(int));
    unsigned char short_buffer[1];
    option_len = sizeof(short_buffer);
    assert(netins_udp_get_opt(client, NETINS_UDP_OPT_APP_PROTOCOL,
        short_buffer, &option_len) == NETINS_UDP_ERROR);
    assert(errno == ENOBUFS);
    assert(option_len == sizeof(int));

    /*
     * set_opt receives an untyped byte buffer. It must not require natural
     * alignment, especially on the AArch64 delivery target.
     */
    unsigned char unaligned_timeout[sizeof(uint32_t) + 1];
    uint32_t unaligned_timeout_value = 4321;
    memcpy(unaligned_timeout + 1, &unaligned_timeout_value,
        sizeof(unaligned_timeout_value));
    assert(netins_udp_set_opt(client,
        NETINS_UDP_OPT_CONNECT_TIMEOUT_MS, unaligned_timeout + 1,
        sizeof(unaligned_timeout_value)) == NETINS_UDP_OK);

    netins_udp_interfaces_t interfaces =
        NETINS_UDP_INTERFACES_INITIALIZER;
    interfaces.interface_count = 2;
    strcpy(interfaces.names[0], "rmnet_data0");
    strcpy(interfaces.names[1], "usb0");
    unsigned char unaligned_interfaces[
        sizeof(netins_udp_interfaces_t) + 1];
    memcpy(unaligned_interfaces + 1, &interfaces, sizeof(interfaces));
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
        unaligned_interfaces + 1, sizeof(interfaces)) == NETINS_UDP_OK);
    set_http3_test_options(client);

    uint32_t connect_timeout_ms = 1234;
    assert(netins_udp_set_opt(client,
        NETINS_UDP_OPT_CONNECT_TIMEOUT_MS, &connect_timeout_ms,
        sizeof(connect_timeout_ms)) == NETINS_UDP_OK);
    int scheduler = NETINS_UDP_SCHEDULER_ADAPTIVE;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_SCHEDULER,
        &scheduler, sizeof(scheduler)) == NETINS_UDP_OK);
    int congestion_control = NETINS_UDP_CC_BBR2;
    assert(netins_udp_set_opt(client,
        NETINS_UDP_OPT_CONGESTION_CONTROL, &congestion_control,
        sizeof(congestion_control)) == NETINS_UDP_OK);

    assert(netins_udp_connect(client, "192.0.2.1", 8443)
        == NETINS_UDP_OK);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_DEBUG] == 1);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_INFO] == 2);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTED);
    assert(g_create_count == 1);
    assert(g_last_config.enable_multipath == 1);
    assert(g_last_config.interface_count == 2);
    assert(strcmp(g_last_config.interface_names[0], "rmnet_data0") == 0);
    assert(strcmp(g_last_config.interface_names[1], "usb0") == 0);
    assert(strcmp(g_last_config.server_name, "test.example") == 0);
    assert(strcmp(g_last_config.ca_file, "/tmp/custom-ca.pem") == 0);
    assert(g_last_config.connect_timeout_ms == 1234);
    assert(g_last_config.scheduler == NETINS_UDP_SCHEDULER_ADAPTIVE);
    assert(g_last_config.congestion_control == NETINS_UDP_CC_BBR2);

    int notify_fd = netins_udp_get_fd(client);
    assert(notify_fd >= 0);
    struct pollfd notify_poll = {
        .fd = notify_fd,
        .events = POLLIN,
    };
    assert(poll(&notify_poll, 1, 0) == 1);
    uint32_t events = 0;
    assert(netins_udp_get_events(client, &events) == NETINS_UDP_OK);
    assert((events & NETINS_UDP_EVENT_CONNECTED) != 0);
    assert((events & NETINS_UDP_EVENT_WRITABLE) != 0);
    assert(g_data_request_open_count == 0);

    g_data_sent_length = 0;
    memcpy(g_data_received, "server-data", 11);
    g_data_received_length = 11;
    g_data_received_offset = 0;
    assert(netins_udp_send(client, "client-data", 11, 0) == 11);
    assert(g_data_request_open_count == 8);
    assert(g_data_sent_length == 11);
    assert(memcmp(g_data_sent, "client-data", 11) == 0);
    char data_buffer[32] = {0};
    assert(netins_udp_recv(client, data_buffer, 6,
        NETINS_UDP_MSG_PEEK) == 6);
    assert(memcmp(data_buffer, "server", 6) == 0);
    memset(data_buffer, 0, sizeof(data_buffer));
    assert(netins_udp_recv(client, data_buffer,
        sizeof(data_buffer), 0) == 11);
    assert(memcmp(data_buffer, "server-data", 11) == 0);
    assert(netins_udp_recv(client, data_buffer,
        sizeof(data_buffer), 0) == 0);
    assert(netins_udp_send(client, NULL, 0,
        NETINS_UDP_MSG_FIN) == 0);
    netins_udp_v2_h3_parent_snapshot_t rejected_h3_parent;
    assert(netins_udp_v2_connection_acquire_h3(client,
        &rejected_h3_parent) == NETINS_UDP_OK);
    assert(netins_udp_v2_connection_select_h3(client)
        == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    netins_udp_v2_connection_release(client);

    assert(poll(&notify_poll, 1, 0) == 1);
    events = 0;
    assert(netins_udp_get_events(client, &events) == NETINS_UDP_OK);
    assert((events & NETINS_UDP_EVENT_READABLE) != 0);
    assert((events & NETINS_UDP_EVENT_WRITABLE) == 0);
    g_last_client->notify(g_last_client->notify_user_data,
        NETINS_UDP_V2_EVENT_PATH_CHANGED);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_WARN] == 1);
    assert(strstr(g_last_log_message, "snapshot") != NULL);
    events = 0;
    assert(netins_udp_get_events(client, &events) == NETINS_UDP_OK);
    assert((events & NETINS_UDP_EVENT_PATH_CHANGED) != 0);

    assert(netins_udp_is_multipath(client) == 1);
    uint32_t path_count = 0;
    assert(netins_udp_get_path_count(client, &path_count) == NETINS_UDP_OK);
    assert(path_count == 2);
    netins_udp_info_t connection_info = NETINS_UDP_INFO_INITIALIZER;
    assert(netins_udp_get_info(client, &connection_info) == NETINS_UDP_OK);
    assert(connection_info.connection_id == 101);
    netins_udp_statis_t connection_statis =
        NETINS_UDP_STATIS_INITIALIZER;
    assert(netins_udp_get_statis(client, &connection_statis)
        == NETINS_UDP_OK);
    assert(connection_statis.bytes_sent == 11);
    assert(connection_statis.bytes_received == 11);
    netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    assert(netins_udp_get_snapshot(client, &snapshot) == NETINS_UDP_OK);
    assert(snapshot.info.connection_id == 101);
    assert(snapshot.statis.bytes_sent == 11);
    assert(snapshot.statis.bytes_received == 11);
    assert(snapshot.paths[0].path_id == 10);
    assert(snapshot.paths[1].bytes_sent == 600);
    netins_udp_path_statis_t path =
        NETINS_UDP_PATH_STATIS_INITIALIZER;
    assert(netins_udp_get_path_statis(client, 1, &path)
        == NETINS_UDP_OK);
    assert(path.path_id == 20);
    assert(netins_udp_get_path_statis(client, 2, &path)
        == NETINS_UDP_ERROR);
    assert(errno == ENOENT);

    uint32_t close_timeout_ms = 2500;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_CLOSE_TIMEOUT_MS,
        &close_timeout_ms, sizeof(close_timeout_ms)) == NETINS_UDP_OK);
    assert(g_last_client->close_timeout_ms == close_timeout_ms);
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
        &interfaces, sizeof(interfaces)) == NETINS_UDP_ERROR);
    assert(errno == EBUSY);

    /*
     * Force get_state() to hold an older CONNECTED transport sample while
     * shutdown() advances the public handle to CLOSING.  The stale sample
     * must not reopen the handle.
     */
    pthread_mutex_lock(&g_wait_lock);
    g_block_get_state = 1;
    g_get_state_entered = 0;
    g_get_state_continue = 0;
    pthread_mutex_unlock(&g_wait_lock);
    state_thread_result_t state_thread_result = {
        .connection = client,
        .state = NETINS_UDP_STATE_ERROR,
    };
    pthread_t state_thread;
    assert(pthread_create(&state_thread, NULL, state_thread_main,
        &state_thread_result) == 0);
    pthread_mutex_lock(&g_wait_lock);
    while (!g_get_state_entered) {
        pthread_cond_wait(&g_wait_cond, &g_wait_lock);
    }
    pthread_mutex_unlock(&g_wait_lock);
    assert(netins_udp_shutdown(client, 77) == NETINS_UDP_OK);
    pthread_mutex_lock(&g_wait_lock);
    g_get_state_continue = 1;
    pthread_cond_broadcast(&g_wait_cond);
    pthread_mutex_unlock(&g_wait_lock);
    assert(pthread_join(state_thread, NULL) == 0);
    assert(state_thread_result.state == NETINS_UDP_STATE_CLOSING);
    pthread_mutex_lock(&g_wait_lock);
    g_block_get_state = 0;
    pthread_mutex_unlock(&g_wait_lock);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CLOSING);
    netins_udp_close_reason_t close_reason =
        NETINS_UDP_CLOSE_REASON_INITIALIZER;
    assert(netins_udp_get_close_reason(client, &close_reason)
        == NETINS_UDP_OK);
    assert(close_reason.source == NETINS_UDP_CLOSE_LOCAL);
    assert(close_reason.application_error == 77);
    g_last_client->state = NETINS_UDP_V2_TRANSPORT_ERROR;
    g_last_client->error_code = ECONNRESET;
    strcpy(g_last_client->error_message,
        "transport failed after local shutdown");
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_ERROR);
    close_reason =
        (netins_udp_close_reason_t)NETINS_UDP_CLOSE_REASON_INITIALIZER;
    assert(netins_udp_get_close_reason(client, &close_reason)
        == NETINS_UDP_OK);
    assert(close_reason.source == NETINS_UDP_CLOSE_LOCAL);
    assert(close_reason.application_error == 77);

    netins_udp_close(client);
    assert(g_close_count == 1);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    pthread_mutex_lock(&g_wait_lock);
    g_block_start = 1;
    g_start_entered = 0;
    g_start_continue = 0;
    pthread_mutex_unlock(&g_wait_lock);
    connect_thread_result_t start_thread_result = {
        .client = client,
        .result = NETINS_UDP_ERROR,
        .error_code = 0,
    };
    pthread_t start_thread;
    assert(pthread_create(&start_thread, NULL, connect_thread_main,
        &start_thread_result) == 0);
    pthread_mutex_lock(&g_wait_lock);
    while (!g_start_entered) {
        pthread_cond_wait(&g_wait_cond, &g_wait_lock);
    }
    pthread_mutex_unlock(&g_wait_lock);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTING);
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_INTERFACES,
        &interfaces, sizeof(interfaces)) == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    pthread_mutex_lock(&g_wait_lock);
    g_start_continue = 1;
    pthread_cond_broadcast(&g_wait_cond);
    pthread_mutex_unlock(&g_wait_lock);
    assert(pthread_join(start_thread, NULL) == 0);
    assert(start_thread_result.result == NETINS_UDP_OK);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTED);
    pthread_mutex_lock(&g_wait_lock);
    g_block_start = 0;
    pthread_mutex_unlock(&g_wait_lock);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    int nonblock = 1;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_NONBLOCK,
        &nonblock, sizeof(nonblock)) == NETINS_UDP_OK);
    assert(netins_udp_connect(client, "198.51.100.1", 443)
        == NETINS_UDP_ERROR);
    assert(errno == EINPROGRESS);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTING);
    g_last_client->state = NETINS_UDP_V2_TRANSPORT_CONNECTED;
    assert(netins_udp_get_last_error_code(client) == 0);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTED);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    /*
     * Error queries must synchronize an asynchronous transport failure on
     * their own; callers are not required to call get_state/get_events first.
     */
    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    nonblock = 1;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_NONBLOCK,
        &nonblock, sizeof(nonblock)) == NETINS_UDP_OK);
    assert(netins_udp_connect(client, "198.51.100.1", 443)
        == NETINS_UDP_ERROR);
    assert(errno == EINPROGRESS);
    g_last_client->state = NETINS_UDP_V2_TRANSPORT_ERROR;
    g_last_client->error_code = ECONNRESET;
    strcpy(g_last_client->error_message, "asynchronous reset");
    assert(netins_udp_get_last_error_code(client) == ECONNRESET);
    char async_error_message[NETINS_UDP_ERROR_MSG_LEN];
    assert(netins_udp_get_last_error_msg(client, async_error_message,
        sizeof(async_error_message)) == NETINS_UDP_OK);
    assert(strcmp(async_error_message, "asynchronous reset") == 0);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    pthread_mutex_lock(&g_wait_lock);
    g_block_wait = 1;
    g_wait_entered = 0;
    g_close_requested = 0;
    pthread_mutex_unlock(&g_wait_lock);
    connect_thread_result_t thread_result = {
        .client = client,
        .result = NETINS_UDP_OK,
        .error_code = 0,
    };
    pthread_t connect_thread;
    assert(pthread_create(&connect_thread, NULL, connect_thread_main,
        &thread_result) == 0);
    pthread_mutex_lock(&g_wait_lock);
    while (!g_wait_entered) {
        pthread_cond_wait(&g_wait_cond, &g_wait_lock);
    }
    pthread_mutex_unlock(&g_wait_lock);
    netins_udp_close(client);
    assert(pthread_join(connect_thread, NULL) == 0);
    assert(thread_result.result == NETINS_UDP_ERROR);
    assert(thread_result.error_code == ECANCELED);
    pthread_mutex_lock(&g_wait_lock);
    g_block_wait = 0;
    pthread_mutex_unlock(&g_wait_lock);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    memset(g_log_count, 0, sizeof(g_log_count));
    g_last_log_message[0] = '\0';
    assert(netins_udp_set_logger(client, test_logger) == NETINS_UDP_OK);
    g_wait_result = -1;
    assert(netins_udp_connect(client, "198.51.100.2", 443)
        == NETINS_UDP_ERROR);
    assert(errno == ETIMEDOUT);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_ERROR] == 1);
    assert(strstr(g_last_log_message, "mock handshake timeout") != NULL);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_ERROR);
    assert(netins_udp_get_last_error_code(client) == ETIMEDOUT);
    close_reason =
        (netins_udp_close_reason_t)NETINS_UDP_CLOSE_REASON_INITIALIZER;
    assert(netins_udp_get_close_reason(client, &close_reason)
        == NETINS_UDP_OK);
    assert(close_reason.source == NETINS_UDP_CLOSE_HANDSHAKE_TIMEOUT);
    assert(close_reason.system_error == ETIMEDOUT);
    close_timeout_ms = 3000;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_CLOSE_TIMEOUT_MS,
        &close_timeout_ms, sizeof(close_timeout_ms)) == NETINS_UDP_OK);
    assert(g_last_client->close_timeout_ms == close_timeout_ms);
    uint32_t accept_timeout_ms = 100;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS,
        &accept_timeout_ms, sizeof(accept_timeout_ms)) == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    g_wait_result = 0;
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    g_start_result = -1;
    assert(netins_udp_connect(client, "198.51.100.3", 443)
        == NETINS_UDP_ERROR);
    assert(errno == EIO);
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_ERROR);
    assert(netins_udp_get_last_error_code(client) == EIO);
    g_start_result = 0;
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    /*
     * A setup error can happen before a transport object exists.  The handle
     * still promises public state/metadata copy-out until close.
     */
    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    assert(netins_udp_connect(client, NULL, 443) == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    connection_info = (netins_udp_info_t)NETINS_UDP_INFO_INITIALIZER;
    assert(netins_udp_get_info(client, &connection_info)
        == NETINS_UDP_OK);
    assert(connection_info.connection_id
        == NETINS_UDP_INVALID_CONNECTION_ID);
    assert(connection_info.requested_type == NETINS_UDP_TYPE_MPQUIC);
    assert(connection_info.role == NETINS_UDP_ROLE_CLIENT);
    assert(connection_info.state == NETINS_UDP_STATE_ERROR);
    snapshot = (netins_udp_snapshot_t)NETINS_UDP_SNAPSHOT_INITIALIZER;
    assert(netins_udp_get_snapshot(client, &snapshot) == NETINS_UDP_OK);
    assert(snapshot.info.state == NETINS_UDP_STATE_ERROR);
    assert(snapshot.path_count == 0);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    int reserved_raw_protocol = 1;
    assert(netins_udp_set_opt(client, NETINS_UDP_OPT_APP_PROTOCOL,
        &reserved_raw_protocol, sizeof(reserved_raw_protocol))
        == NETINS_UDP_ERROR);
    assert(errno == EOPNOTSUPP);
    assert(netins_udp_get_last_error_code(client) == EOPNOTSUPP);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    assert(netins_udp_v2_child_handle_acquire() == NETINS_UDP_OK);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    netins_udp_v2_child_handle_release();
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    g_pending_resolvers = 1;
    assert(netins_udp_cleanup() == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    g_pending_resolvers = 0;
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    netins_udp_t *listener = netins_udp_new(NETINS_UDP_TYPE_MPQUIC);
    assert(listener != NULL);
    memset(g_log_count, 0, sizeof(g_log_count));
    g_last_log_message[0] = '\0';
    assert(netins_udp_set_logger(listener, test_logger) == NETINS_UDP_OK);
    set_http3_server_options(listener);
    nonblock = 1;
    assert(netins_udp_set_opt(listener, NETINS_UDP_OPT_NONBLOCK,
        &nonblock, sizeof(nonblock)) == NETINS_UDP_OK);
    assert(netins_udp_listen(listener, NULL, 8443) == listener);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_INFO] == 1);
    assert(netins_udp_get_state(listener) == NETINS_UDP_STATE_LISTENING);
    assert(netins_udp_send(listener, "invalid", 7, 0) == -1);
    assert(errno == EOPNOTSUPP);
    assert(netins_udp_recv(listener, data_buffer,
        sizeof(data_buffer), 0) == -1);
    assert(errno == EOPNOTSUPP);
    accept_timeout_ms = 50;
    assert(netins_udp_set_opt(listener, NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS,
        &accept_timeout_ms, sizeof(accept_timeout_ms)) == NETINS_UDP_OK);
    assert(netins_udp_set_opt(listener, NETINS_UDP_OPT_SCHEDULER,
        &scheduler, sizeof(scheduler)) == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    assert(netins_udp_shutdown(listener, 0) == NETINS_UDP_ERROR);
    assert(errno == EOPNOTSUPP);
    assert(netins_udp_get_state(listener) == NETINS_UDP_STATE_LISTENING);
    assert(g_last_server_config.enable_multipath == 1);
    assert(g_last_server_config.port == 8443);
    assert(strcmp(g_last_server_config.certificate_file,
        "/tmp/server.crt") == 0);
    assert(strcmp(g_last_server_config.private_key_file,
        "/tmp/server.key") == 0);
    errno = 0;
    assert(netins_udp_accept(listener, NULL, NULL) == NULL);
    assert(errno == EAGAIN);

    pthread_mutex_lock(&g_last_server->lock);
    g_server_accept_ready = 1;
    g_last_server->events |= NETINS_UDP_V2_EVENT_ACCEPTABLE;
    if (g_last_server->notify != NULL) {
        g_last_server->notify(g_last_server->notify_user_data,
            NETINS_UDP_V2_EVENT_ACCEPTABLE);
    }
    pthread_cond_broadcast(&g_last_server->cond);
    pthread_mutex_unlock(&g_last_server->lock);
    int listener_fd = netins_udp_get_fd(listener);
    assert(listener_fd >= 0);
    events = 0;
    assert(netins_udp_get_events(listener, &events) == NETINS_UDP_OK);
    assert((events & NETINS_UDP_EVENT_ACCEPTABLE) != 0);
    struct sockaddr_storage peer_address;
    socklen_t peer_address_len = sizeof(peer_address);
    netins_udp_t *accepted = netins_udp_accept(listener,
        (struct sockaddr *)&peer_address, &peer_address_len);
    assert(accepted != NULL);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_INFO] == 2);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_WARN] == 1);
    assert(peer_address.ss_family == AF_INET);
    assert(peer_address_len == sizeof(struct sockaddr_in));
    assert(netins_udp_get_state(accepted) == NETINS_UDP_STATE_CONNECTED);
    g_last_connection->events |=
        NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE;
    g_last_connection->notify(g_last_connection->notify_user_data,
        NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE);
    events = 0;
    assert(netins_udp_get_events(accepted, &events) == NETINS_UDP_OK);
    assert((events & NETINS_UDP_EVENT_H3_REQUEST_ACCEPTABLE) != 0);
    assert((events & NETINS_UDP_EVENT_READABLE) != 0);
    assert((events & NETINS_UDP_EVENT_WRITABLE) != 0);
    assert(g_data_request_accept_count == 0);
    g_data_sent_length = 0;
    memcpy(g_data_received, "client-upload", 13);
    g_data_received_length = 13;
    g_data_received_offset = 0;
    memset(data_buffer, 0, sizeof(data_buffer));
    assert(netins_udp_recv(accepted, data_buffer,
        sizeof(data_buffer), 0) == 13);
    assert(memcmp(data_buffer, "client-upload", 13) == 0);
    assert(g_data_request_accept_count == 8);
    assert(g_last_data_request->response.status == 200);
    assert(g_last_data_request->response.content_length
        == NETINS_UDP_CONTENT_LENGTH_UNKNOWN);
    assert(netins_udp_send(accepted, "server-reply", 12,
        NETINS_UDP_MSG_FIN) == 12);
    assert(g_data_sent_length == 12);
    assert(memcmp(g_data_sent, "server-reply", 12) == 0);
    netins_udp_v2_h3_server_parent_snapshot_t rejected_server_h3_parent;
    assert(netins_udp_v2_connection_acquire_h3_server(accepted,
        &rejected_server_h3_parent) == NETINS_UDP_OK);
    assert(netins_udp_v2_connection_select_h3(accepted)
        == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    netins_udp_v2_connection_release(accepted);
    assert(netins_udp_get_fd(accepted) >= 0);
    assert(netins_udp_get_fd(accepted) != listener_fd);
    events = 0;
    assert(netins_udp_get_events(accepted, &events) == NETINS_UDP_OK);
    assert((events & NETINS_UDP_EVENT_CONNECTED) != 0);
    assert((events & NETINS_UDP_EVENT_READABLE) != 0);
    assert((events & NETINS_UDP_EVENT_WRITABLE) != 0);
    g_last_connection->events |=
        NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE;
    g_last_connection->notify(g_last_connection->notify_user_data,
        NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE);
    events = 0;
    assert(netins_udp_get_events(accepted, &events) == NETINS_UDP_OK);
    assert((events & NETINS_UDP_EVENT_H3_REQUEST_ACCEPTABLE) != 0);
    int inherited_log_level = -1;
    size_t inherited_log_level_len = sizeof(inherited_log_level);
    assert(netins_udp_get_opt(accepted, NETINS_UDP_OPT_LOG_LEVEL,
        &inherited_log_level, &inherited_log_level_len) == NETINS_UDP_OK);
    assert(inherited_log_level == NETINS_UDP_LOG_LEVEL_INFO);
    g_last_connection->notify(g_last_connection->notify_user_data,
        NETINS_UDP_V2_EVENT_PATH_CHANGED);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_WARN] == 2);
    assert(netins_udp_set_logger(listener, NULL) == NETINS_UDP_OK);
    g_last_connection->notify(g_last_connection->notify_user_data,
        NETINS_UDP_V2_EVENT_PATH_CHANGED);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_WARN] == 3);
    inherited_log_level = NETINS_UDP_LOG_LEVEL_ERROR;
    assert(netins_udp_set_opt(accepted, NETINS_UDP_OPT_LOG_LEVEL,
        &inherited_log_level, sizeof(inherited_log_level))
        == NETINS_UDP_OK);
    g_last_connection->notify(g_last_connection->notify_user_data,
        NETINS_UDP_V2_EVENT_PATH_CHANGED);
    assert(g_log_count[NETINS_UDP_LOG_LEVEL_WARN] == 3);
    assert(netins_udp_set_logger(accepted, NULL) == NETINS_UDP_OK);
    snapshot = (netins_udp_snapshot_t)NETINS_UDP_SNAPSHOT_INITIALIZER;
    assert(netins_udp_get_snapshot(accepted, &snapshot) == NETINS_UDP_OK);
    assert(snapshot.info.connection_id == 202);
    option_len = sizeof(nonblock);
    nonblock = 0;
    assert(netins_udp_get_opt(accepted, NETINS_UDP_OPT_NONBLOCK,
        &nonblock, &option_len) == NETINS_UDP_OK);
    assert(nonblock == 1);
    uint32_t accepted_send_timeout_ms = 75;
    assert(netins_udp_set_opt(accepted, NETINS_UDP_OPT_SEND_TIMEOUT_MS,
        &accepted_send_timeout_ms, sizeof(accepted_send_timeout_ms))
        == NETINS_UDP_OK);
    assert(netins_udp_set_opt(accepted, NETINS_UDP_OPT_SCHEDULER,
        &scheduler, sizeof(scheduler)) == NETINS_UDP_ERROR);
    assert(errno == EBUSY);

    netins_udp_close(listener);
    assert(g_server_close_count == 1);
    assert(netins_udp_cleanup() == NETINS_UDP_ERROR);
    assert(errno == EBUSY);
    assert(netins_udp_get_state(accepted) == NETINS_UDP_STATE_CONNECTED);
    assert(netins_udp_shutdown(accepted, 1234) == NETINS_UDP_OK);
    assert(g_last_connection->shutdown_error == 1234);
    assert(netins_udp_get_state(accepted) == NETINS_UDP_STATE_CLOSING);
    netins_udp_close(accepted);
    assert(g_connection_close_count == 1);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    listener = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(listener != NULL);
    set_http3_server_options(listener);
    accept_timeout_ms = 25;
    assert(netins_udp_set_opt(listener, NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS,
        &accept_timeout_ms, sizeof(accept_timeout_ms)) == NETINS_UDP_OK);
    assert(netins_udp_listen(listener, "127.0.0.1", 9443) == listener);
    errno = 0;
    assert(netins_udp_accept(listener, NULL, NULL) == NULL);
    assert(errno == ETIMEDOUT);
    netins_udp_close(listener);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    listener = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(listener != NULL);
    set_http3_server_options(listener);
    assert(netins_udp_listen(listener, "", 10443) == listener);
    g_server_accept_entered = 0;
    accept_thread_result_t accept_result = {
        .listener = listener,
        .connection = NULL,
        .error_code = 0,
    };
    pthread_t accept_thread;
    assert(pthread_create(&accept_thread, NULL, accept_thread_main,
        &accept_result) == 0);
    pthread_mutex_lock(&g_last_server->lock);
    while (!g_server_accept_entered) {
        pthread_cond_wait(&g_last_server->cond, &g_last_server->lock);
    }
    pthread_mutex_unlock(&g_last_server->lock);
    netins_udp_close(listener);
    assert(pthread_join(accept_thread, NULL) == 0);
    assert(accept_result.connection == NULL);
    assert(accept_result.error_code == ECANCELED);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    listener = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(listener != NULL);
    int protocol = NETINS_UDP_APP_HTTP3;
    assert(netins_udp_set_opt(listener, NETINS_UDP_OPT_APP_PROTOCOL,
        &protocol, sizeof(protocol)) == NETINS_UDP_OK);
    assert(netins_udp_listen(listener, NULL, 8443) == NULL);
    assert(errno == EINVAL);
    assert(netins_udp_get_state(listener) == NETINS_UDP_STATE_ERROR);
    netins_udp_close(listener);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    listener = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(listener != NULL);
    set_http3_server_options(listener);
    interfaces.interface_count = 1;
    assert(netins_udp_set_opt(listener, NETINS_UDP_OPT_INTERFACES,
        &interfaces, sizeof(interfaces)) == NETINS_UDP_OK);
    assert(netins_udp_listen(listener, NULL, 8443) == NULL);
    assert(errno == EOPNOTSUPP);
    netins_udp_close(listener);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    assert(netins_udp_connect(client, "192.0.2.2", 8443)
        == NETINS_UDP_OK);
    g_data_sent_length = 0;
    assert(netins_udp_send(client, "queued-before-shutdown", 22, 0)
        == 22);
    assert(g_last_data_request != NULL);
    assert(g_last_data_request->send_finished == 0);
    assert(netins_udp_shutdown(client, 0) == NETINS_UDP_OK);
    assert(g_last_data_request->send_finished == 1);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    /*
     * Selecting the advanced request API first must also prevent the simple
     * connection byte API from silently creating its hidden request.
     */
    assert(netins_udp_init() == NETINS_UDP_OK);
    client = netins_udp_new(NETINS_UDP_TYPE_QUIC);
    assert(client != NULL);
    set_http3_test_options(client);
    assert(netins_udp_connect(client, "192.0.2.3", 8443)
        == NETINS_UDP_OK);
    netins_udp_v2_h3_parent_snapshot_t advanced_parent;
    assert(netins_udp_v2_connection_acquire_h3(client, &advanced_parent)
        == NETINS_UDP_OK);
    assert(netins_udp_v2_connection_select_h3(client)
        == NETINS_UDP_OK);
    netins_udp_v2_connection_release(client);
    int data_open_count_before_conflict = g_data_request_open_count;
    assert(netins_udp_send(client, "must-not-open", 13, 0) == -1);
    assert(errno == EBUSY);
    assert(g_data_request_open_count == data_open_count_before_conflict);
    netins_udp_close(client);
    assert(netins_udp_cleanup() == NETINS_UDP_OK);

    assert(g_create_count == g_close_count);
    assert(g_server_create_count == g_server_close_count);
    assert(g_data_request_open_count + g_data_request_accept_count
        == g_data_request_close_count);
    return 0;
}
