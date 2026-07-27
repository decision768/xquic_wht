#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    assert(netins_udp_get_state(client) == NETINS_UDP_STATE_CONNECTED);
    assert(g_create_count == 1);
    assert(g_last_config.enable_multipath == 1);
    assert(g_last_config.interface_count == 2);
    assert(strcmp(g_last_config.interface_names[0], "rmnet_data0") == 0);
    assert(strcmp(g_last_config.interface_names[1], "usb0") == 0);
    assert(strcmp(g_last_config.server_name, "test.example") == 0);
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
    g_last_client->notify(g_last_client->notify_user_data,
        NETINS_UDP_V2_EVENT_PATH_CHANGED);
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
    assert(connection_statis.bytes_sent == 1000);
    netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    assert(netins_udp_get_snapshot(client, &snapshot) == NETINS_UDP_OK);
    assert(snapshot.info.connection_id == 101);
    assert(snapshot.statis.bytes_sent == 1000);
    assert(snapshot.statis.bytes_received == 2000);
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
    g_wait_result = -1;
    assert(netins_udp_connect(client, "198.51.100.2", 443)
        == NETINS_UDP_ERROR);
    assert(errno == ETIMEDOUT);
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
    set_http3_server_options(listener);
    nonblock = 1;
    assert(netins_udp_set_opt(listener, NETINS_UDP_OPT_NONBLOCK,
        &nonblock, sizeof(nonblock)) == NETINS_UDP_OK);
    assert(netins_udp_listen(listener, NULL, 8443) == listener);
    assert(netins_udp_get_state(listener) == NETINS_UDP_STATE_LISTENING);
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
    assert(peer_address.ss_family == AF_INET);
    assert(peer_address_len == sizeof(struct sockaddr_in));
    assert(netins_udp_get_state(accepted) == NETINS_UDP_STATE_CONNECTED);
    assert(netins_udp_get_fd(accepted) >= 0);
    assert(netins_udp_get_fd(accepted) != listener_fd);
    events = 0;
    assert(netins_udp_get_events(accepted, &events) == NETINS_UDP_OK);
    assert((events & NETINS_UDP_EVENT_CONNECTED) != 0);
    g_last_connection->events |=
        NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE;
    g_last_connection->notify(g_last_connection->notify_user_data,
        NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE);
    events = 0;
    assert(netins_udp_get_events(accepted, &events) == NETINS_UDP_OK);
    assert((events & NETINS_UDP_EVENT_H3_REQUEST_ACCEPTABLE) != 0);
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

    assert(g_create_count == g_close_count);
    assert(g_server_create_count == g_server_close_count);
    return 0;
}
