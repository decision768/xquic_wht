#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api/draft/netins_udp_v2.h"
#include "netins_udp_v2_internal.h"
#include "netins_udp_v2_transport.h"

struct netins_udp_client_s {
    int unused;
};

struct netins_udp_s {
    int connected;
    int server_side;
    int nonblock;
    int last_error_code;
    char last_error_msg[NETINS_UDP_ERROR_MSG_LEN];
};

struct netins_udp_stream_s {
    netins_udp_v2_transport_h3_config_t config;
    netins_udp_v2_transport_h3_info_t info;
    unsigned char received[64];
    size_t received_length;
    size_t received_offset;
    int eof_delivered;
    int close_async;
    uint32_t events;
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;
};

struct netins_udp_server_connection_s {
    int unused;
};

struct netins_udp_server_request_s {
    netins_udp_v2_transport_h3_response_config_t response;
    netins_udp_v2_transport_h3_info_t info;
    unsigned char received[64];
    size_t received_length;
    size_t received_offset;
    int eof_delivered;
    int response_configured;
    int close_async;
    uint32_t events;
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;
};

static struct netins_udp_client_s g_client;
static struct netins_udp_server_connection_s g_server_connection;
static netins_udp_v2_transport_h3_config_t g_open_config;
static int g_child_count;
static size_t g_send_chunk;
static int g_send_would_block;
static int g_response_content_length_known = 1;
static pthread_mutex_t g_mock_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mock_cond = PTHREAD_COND_INITIALIZER;
static int g_block_recv;
static int g_recv_entered;
static int g_server_request_available;
static uint32_t g_server_accept_timeout_ms;
static int g_server_accept_wait_forever;

int
netins_udp_v2_connection_acquire_h3(
    netins_udp_t *netins,
    netins_udp_v2_h3_parent_snapshot_t *out)
{
    if (netins == NULL || out == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    if (!netins->connected || netins->server_side) {
        netins->last_error_code = ENOTCONN;
        errno = ENOTCONN;
        return NETINS_UDP_ERROR;
    }
    memset(out, 0, sizeof(*out));
    out->transport = &g_client;
    out->close_timeout_ms = 100;
    return NETINS_UDP_OK;
}

int
netins_udp_v2_connection_acquire_h3_server(
    netins_udp_t *netins,
    netins_udp_v2_h3_server_parent_snapshot_t *out)
{
    if (netins == NULL || out == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    if (!netins->connected || !netins->server_side) {
        netins->last_error_code = ENOTCONN;
        errno = ENOTCONN;
        return NETINS_UDP_ERROR;
    }
    memset(out, 0, sizeof(*out));
    out->transport = &g_server_connection;
    out->nonblock = netins->nonblock;
    out->close_timeout_ms = 100;
    return NETINS_UDP_OK;
}

void
netins_udp_v2_connection_release(netins_udp_t *netins)
{
    (void)netins;
}

void
netins_udp_v2_connection_set_error(netins_udp_t *netins,
    int error_code, const char *message)
{
    netins->last_error_code = error_code;
    snprintf(netins->last_error_msg, sizeof(netins->last_error_msg),
        "%s", message != NULL ? message : "");
    if (error_code != 0) {
        errno = error_code;
    }
}

int
netins_udp_v2_child_handle_acquire(void)
{
    g_child_count++;
    return NETINS_UDP_OK;
}

void
netins_udp_v2_child_handle_release(void)
{
    assert(g_child_count > 0);
    g_child_count--;
}

int
netins_udp_v2_transport_h3_request_open(
    netins_udp_v2_transport_client_t *client,
    const netins_udp_v2_transport_h3_config_t *config,
    netins_udp_v2_transport_request_t **out_request)
{
    assert(client == &g_client);
    g_open_config = *config;
    netins_udp_v2_transport_request_t *request =
        calloc(1, sizeof(*request));
    assert(request != NULL);
    request->config = *config;
    request->info.stream_id = 7;
    request->info.request_headers_ready = 1;
    request->info.response_headers_ready = 1;
    request->info.response_status = 200;
    strcpy(request->info.request_authority, "example.test");
    request->info.response_content_length_known =
        g_response_content_length_known;
    request->info.response_content_length = 5;
    strcpy(request->info.response_content_type, "text/plain");
    request->info.peer_header_count = 1;
    strcpy(request->info.peer_headers[0].name, "x-test");
    strcpy(request->info.peer_headers[0].value, "response");
    memcpy(request->received, "hello", 5);
    request->received_length = 5;
    request->info.receive_finished = 1;
    *out_request = request;
    return 0;
}

int64_t
netins_udp_v2_transport_h3_request_send(
    netins_udp_v2_transport_request_t *request,
    const void *data, size_t length, int fin, uint32_t timeout_ms,
    int wait_forever)
{
    (void)data;
    (void)timeout_ms;
    (void)wait_forever;
    if (g_send_would_block) {
        errno = EAGAIN;
        return -1;
    }
    size_t accepted = g_send_chunk != 0 && g_send_chunk < length
        ? g_send_chunk : length;
    request->info.bytes_accepted += accepted;
    if (fin && accepted == length) {
        request->info.send_finished = 1;
    }
    return (int64_t)accepted;
}

int64_t
netins_udp_v2_transport_h3_request_recv(
    netins_udp_v2_transport_request_t *request,
    void *data, size_t length, int *fin, int peek,
    uint32_t timeout_ms, int wait_forever)
{
    (void)timeout_ms;
    (void)wait_forever;
    pthread_mutex_lock(&g_mock_lock);
    if (g_block_recv) {
        g_recv_entered = 1;
        pthread_cond_broadcast(&g_mock_cond);
        while (!request->close_async) {
            pthread_cond_wait(&g_mock_cond, &g_mock_lock);
        }
        pthread_mutex_unlock(&g_mock_lock);
        errno = ECANCELED;
        return -1;
    }
    pthread_mutex_unlock(&g_mock_lock);

    size_t available = request->received_length
        - request->received_offset;
    if (available == 0) {
        if (request->info.receive_finished) {
            *fin = 1;
            if (!peek) {
                request->eof_delivered = 1;
            }
            return 0;
        }
        errno = EAGAIN;
        return -1;
    }
    size_t copied = available < length ? available : length;
    memcpy(data, request->received + request->received_offset, copied);
    if (!peek) {
        request->received_offset += copied;
        request->info.bytes_delivered += copied;
    }
    *fin = (peek
            ? request->received_offset + copied
            : request->received_offset)
        == request->received_length
        && request->info.receive_finished;
    return (int64_t)copied;
}

int
netins_udp_v2_transport_h3_request_get_info(
    netins_udp_v2_transport_request_t *request,
    netins_udp_v2_transport_h3_info_t *out)
{
    *out = request->info;
    return 0;
}

int
netins_udp_v2_transport_h3_request_get_events(
    netins_udp_v2_transport_request_t *request, uint32_t *out_events)
{
    *out_events = request->events;
    if (request->received_offset < request->received_length
        || (request->info.receive_finished && !request->eof_delivered))
    {
        *out_events |= NETINS_UDP_V2_EVENT_READABLE;
    }
    return 0;
}

void
netins_udp_v2_transport_h3_request_set_notify(
    netins_udp_v2_transport_request_t *request,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    request->notify = notify;
    request->notify_user_data = user_data;
    if (notify != NULL
        && (request->received_length != 0
            || request->info.receive_finished))
    {
        notify(user_data, NETINS_UDP_V2_EVENT_READABLE);
    }
}

void
netins_udp_v2_transport_h3_request_close_async(
    netins_udp_v2_transport_request_t *request)
{
    pthread_mutex_lock(&g_mock_lock);
    request->close_async = 1;
    pthread_cond_broadcast(&g_mock_cond);
    pthread_mutex_unlock(&g_mock_lock);
}

void
netins_udp_v2_transport_h3_request_close(
    netins_udp_v2_transport_request_t *request,
    uint32_t timeout_ms)
{
    assert(timeout_ms == 100);
    free(request);
}

int
netins_udp_v2_transport_server_request_accept(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t timeout_ms, int wait_forever,
    netins_udp_v2_transport_server_request_t **out_request)
{
    assert(connection == &g_server_connection);
    g_server_accept_timeout_ms = timeout_ms;
    g_server_accept_wait_forever = wait_forever;
    if (!g_server_request_available) {
        errno = EAGAIN;
        return -1;
    }
    g_server_request_available = 0;
    netins_udp_v2_transport_server_request_t *request =
        calloc(1, sizeof(*request));
    assert(request != NULL);
    request->info.stream_id = 11;
    request->info.request_headers_ready = 1;
    request->info.receive_finished = 1;
    strcpy(request->info.request_method, "POST");
    strcpy(request->info.request_path, "/upload");
    strcpy(request->info.request_authority, "server.test");
    strcpy(request->info.request_content_type,
        "application/octet-stream");
    request->info.request_content_length_known = 1;
    request->info.request_content_length = 6;
    request->info.peer_header_count = 1;
    strcpy(request->info.peer_headers[0].name, "x-client");
    strcpy(request->info.peer_headers[0].value, "board");
    memcpy(request->received, "upload", 6);
    request->received_length = 6;
    *out_request = request;
    return 0;
}

int
netins_udp_v2_transport_server_request_set_response(
    netins_udp_v2_transport_server_request_t *request,
    const netins_udp_v2_transport_h3_response_config_t *config)
{
    if (request->response_configured) {
        errno = EALREADY;
        return -1;
    }
    request->response = *config;
    request->response_configured = 1;
    request->info.response_status = config->status;
    request->info.response_headers_ready = 1;
    request->info.response_content_length_known =
        !config->content_length_unknown;
    request->info.response_content_length = config->content_length;
    strcpy(request->info.response_content_type, config->content_type);
    return 0;
}

int64_t
netins_udp_v2_transport_server_request_send(
    netins_udp_v2_transport_server_request_t *request,
    const void *data, size_t length, int fin, uint32_t timeout_ms,
    int wait_forever)
{
    (void)data;
    (void)timeout_ms;
    (void)wait_forever;
    size_t accepted = g_send_chunk != 0 && g_send_chunk < length
        ? g_send_chunk : length;
    request->info.bytes_accepted += accepted;
    if (fin && accepted == length) {
        request->info.send_finished = 1;
    }
    return (int64_t)accepted;
}

int64_t
netins_udp_v2_transport_server_request_recv(
    netins_udp_v2_transport_server_request_t *request,
    void *data, size_t length, int *fin, int peek,
    uint32_t timeout_ms, int wait_forever)
{
    (void)timeout_ms;
    (void)wait_forever;
    size_t available = request->received_length
        - request->received_offset;
    if (available == 0) {
        *fin = request->info.receive_finished;
        if (*fin && !peek) {
            request->eof_delivered = 1;
        }
        return request->info.receive_finished ? 0 : -1;
    }
    size_t copied = available < length ? available : length;
    memcpy(data, request->received + request->received_offset, copied);
    if (!peek) {
        request->received_offset += copied;
        request->info.bytes_delivered += copied;
    }
    *fin = (peek
            ? request->received_offset + copied
            : request->received_offset)
        == request->received_length
        && request->info.receive_finished;
    return (int64_t)copied;
}

int
netins_udp_v2_transport_server_request_get_info(
    netins_udp_v2_transport_server_request_t *request,
    netins_udp_v2_transport_h3_info_t *out)
{
    *out = request->info;
    return 0;
}

int
netins_udp_v2_transport_server_request_get_events(
    netins_udp_v2_transport_server_request_t *request,
    uint32_t *out_events)
{
    *out_events = request->events;
    if (request->received_offset < request->received_length
        || (request->info.receive_finished && !request->eof_delivered))
    {
        *out_events |= NETINS_UDP_V2_EVENT_READABLE;
    }
    if (request->response_configured) {
        *out_events |= NETINS_UDP_V2_EVENT_WRITABLE;
    }
    return 0;
}

void
netins_udp_v2_transport_server_request_set_notify(
    netins_udp_v2_transport_server_request_t *request,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    request->notify = notify;
    request->notify_user_data = user_data;
    uint32_t events = request->received_length != 0
        || request->info.receive_finished
        ? NETINS_UDP_V2_EVENT_READABLE : 0;
    if (request->response_configured) {
        events |= NETINS_UDP_V2_EVENT_WRITABLE;
    }
    if (notify != NULL && events != 0) {
        notify(user_data, events);
    }
}

void
netins_udp_v2_transport_server_request_close_async(
    netins_udp_v2_transport_server_request_t *request)
{
    request->close_async = 1;
}

void
netins_udp_v2_transport_server_request_close(
    netins_udp_v2_transport_server_request_t *request,
    uint32_t timeout_ms)
{
    assert(timeout_ms == 100);
    assert(request->close_async);
    free(request);
}

typedef struct recv_thread_result_s {
    netins_udp_h3_request_t *request;
    ssize_t result;
    int error_code;
} recv_thread_result_t;

static void *
recv_thread_main(void *argument)
{
    recv_thread_result_t *result = argument;
    unsigned char byte;
    result->result = netins_udp_h3_request_recv(result->request,
        &byte, sizeof(byte), 0);
    result->error_code = errno;
    return NULL;
}

static netins_udp_h3_request_t *
open_request(netins_udp_t *connection, const char *method,
    uint64_t content_length)
{
    netins_udp_h3_request_config_t config;
    netins_udp_h3_request_config_init(&config);
    strcpy(config.method, method);
    config.content_length = content_length;
    assert(netins_udp_h3_request_config_add_header(&config,
        "x-stream-index", "0") == NETINS_UDP_OK);

    netins_udp_h3_request_t *request = NULL;
    assert(netins_udp_h3_request_open(connection, &config, &request)
        == NETINS_UDP_OK);
    assert(request != NULL);
    assert(g_open_config.header_count == 1);
    assert(strcmp(g_open_config.headers[0].name,
        "x-stream-index") == 0);
    return request;
}

int
main(void)
{
    netins_udp_h3_request_config_t config;
    netins_udp_h3_request_config_init(&config);
    assert(config.struct_size == sizeof(config));
    assert(config.api_version == NETINS_UDP_API_VERSION);
    assert(strcmp(config.method, "GET") == 0);
    assert(strcmp(config.path, "/") == 0);
    assert(strcmp(config.content_type,
        "application/octet-stream") == 0);
    assert(netins_udp_h3_request_config_add_header(&config,
        "x-test", "value") == NETINS_UDP_OK);
    assert(config.header_count == 1);
    assert(netins_udp_h3_request_config_add_header(&config,
        "content-length", "1") == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    assert(netins_udp_h3_request_config_add_header(&config,
        "X-Test", "value") == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    assert(netins_udp_h3_request_config_add_header(&config,
        "x-test", "bad\rvalue") == NETINS_UDP_ERROR);
    assert(errno == EINVAL);

    netins_udp_h3_response_config_t response;
    netins_udp_h3_response_config_init(&response);
    assert(response.status == 200);
    assert(netins_udp_h3_response_config_add_header(&response,
        "x-response", "yes") == NETINS_UDP_OK);

    netins_udp_t connection = {0};
    netins_udp_h3_request_t *request = NULL;
    assert(netins_udp_h3_request_open(&connection, &config, &request)
        == NETINS_UDP_ERROR);
    assert(errno == ENOTCONN);

    connection.connected = 1;
    assert(netins_udp_h3_request_open(&connection, NULL, &request)
        == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    request = open_request(&connection, "POST", 5);
    assert(g_child_count == 1);
    int request_fd = netins_udp_h3_request_get_fd(request);
    assert(request_fd >= 0);
    struct pollfd request_poll = {
        .fd = request_fd,
        .events = POLLIN,
    };
    assert(poll(&request_poll, 1, 0) == 1);
    uint32_t request_events = 0;
    assert(netins_udp_h3_request_get_events(request, &request_events)
        == NETINS_UDP_OK);
    assert((request_events & NETINS_UDP_EVENT_READABLE) != 0);
    assert(netins_udp_h3_request_set_response(request, &response)
        == NETINS_UDP_ERROR);
    assert(errno == EOPNOTSUPP);
    assert(netins_udp_h3_request_send(request, "hello", 5, 0) == -1);
    assert(errno == EINVAL);
    g_send_chunk = 2;
    assert(netins_udp_h3_request_send(request, "hello", 5,
        NETINS_UDP_MSG_FIN) == 2);
    g_send_chunk = 0;
    assert(netins_udp_h3_request_send(request, "llo", 3,
        NETINS_UDP_MSG_FIN) == 3);
    assert(netins_udp_h3_request_send(request, NULL, 0,
        NETINS_UDP_MSG_FIN) == -1);
    assert(errno == EPIPE);

    char buffer[8] = {0};
    assert(netins_udp_h3_request_recv(request, buffer, 3,
        NETINS_UDP_MSG_PEEK) == 3);
    assert(memcmp(buffer, "hel", 3) == 0);
    memset(buffer, 0, sizeof(buffer));
    assert(netins_udp_h3_request_recv(request, buffer, 3,
        NETINS_UDP_MSG_PEEK) == 3);
    assert(memcmp(buffer, "hel", 3) == 0);
    assert(netins_udp_h3_request_recv(request, buffer, 3, 0) == 3);
    assert(netins_udp_h3_request_recv(request, buffer, 3, 0) == 2);
    assert(netins_udp_h3_request_recv(request, buffer, 3, 0) == 0);
    request_events = UINT32_MAX;
    assert(netins_udp_h3_request_get_events(request, &request_events)
        == NETINS_UDP_OK);
    assert((request_events & NETINS_UDP_EVENT_READABLE) == 0);
    request_poll.revents = 0;
    assert(poll(&request_poll, 1, 0) == 0);

    netins_udp_h3_request_info_t info =
        NETINS_UDP_H3_REQUEST_INFO_INITIALIZER;
    assert(netins_udp_h3_request_get_info(request, &info)
        == NETINS_UDP_OK);
    assert(info.stream_id == 7);
    assert(info.response_status == 200);
    assert(strcmp(info.authority, "example.test") == 0);
    assert(info.response_content_length == 5);
    assert(info.bytes_sent == 5);
    assert(info.bytes_received == 5);
    assert(info.peer_header_count == 1);
    assert(strcmp(info.peer_headers[0].name, "x-test") == 0);
    netins_udp_h3_request_close(request);
    assert(g_child_count == 0);

    request = open_request(&connection, "GET", 0);
    assert(netins_udp_h3_request_send(request, "x", 1,
        NETINS_UDP_MSG_FIN) == -1);
    assert(errno == EOPNOTSUPP);
    netins_udp_h3_request_close(request);

    g_response_content_length_known = 0;
    request = open_request(&connection, "GET", 0);
    info = (netins_udp_h3_request_info_t)
        NETINS_UDP_H3_REQUEST_INFO_INITIALIZER;
    assert(netins_udp_h3_request_get_info(request, &info)
        == NETINS_UDP_OK);
    assert(info.response_content_length
        == NETINS_UDP_CONTENT_LENGTH_UNKNOWN);
    netins_udp_h3_request_close(request);
    g_response_content_length_known = 1;

    netins_udp_t server_connection = {
        .connected = 1,
        .server_side = 1,
        .nonblock = 1,
    };
    assert(netins_udp_h3_request_accept(&server_connection, &request)
        == NETINS_UDP_ERROR);
    assert(errno == EAGAIN);
    assert(g_server_accept_timeout_ms == 0);
    assert(g_server_accept_wait_forever == 0);
    server_connection.nonblock = 0;
    g_server_request_available = 1;
    assert(netins_udp_h3_request_accept(&server_connection, &request)
        == NETINS_UDP_OK);
    assert(request != NULL);
    assert(g_child_count == 1);
    assert(netins_udp_h3_request_get_fd(request) >= 0);
    request_events = 0;
    assert(netins_udp_h3_request_get_events(request, &request_events)
        == NETINS_UDP_OK);
    assert((request_events & NETINS_UDP_EVENT_READABLE) != 0);
    info = (netins_udp_h3_request_info_t)
        NETINS_UDP_H3_REQUEST_INFO_INITIALIZER;
    assert(netins_udp_h3_request_get_info(request, &info)
        == NETINS_UDP_OK);
    assert(info.stream_id == 11);
    assert(strcmp(info.method, "POST") == 0);
    assert(strcmp(info.path, "/upload") == 0);
    assert(strcmp(info.authority, "server.test") == 0);
    assert(info.request_content_length == 6);
    assert(info.peer_header_count == 1);
    memset(buffer, 0, sizeof(buffer));
    assert(netins_udp_h3_request_recv(request, buffer,
        sizeof(buffer), 0) == 6);
    assert(memcmp(buffer, "upload", 6) == 0);
    assert(netins_udp_h3_request_recv(request, buffer,
        sizeof(buffer), 0) == 0);
    assert(netins_udp_h3_request_send(request, "OK", 2,
        NETINS_UDP_MSG_FIN) == -1);
    assert(errno == EINVAL);

    netins_udp_h3_response_config_init(&response);
    response.status = 201;
    response.content_length = 2;
    strcpy(response.content_type, "text/plain");
    response.status = 199;
    assert(netins_udp_h3_request_set_response(request, &response)
        == NETINS_UDP_ERROR);
    assert(errno == EINVAL);
    response.status = 201;
    assert(netins_udp_h3_request_set_response(request, &response)
        == NETINS_UDP_OK);
    assert(netins_udp_h3_request_set_response(request, &response)
        == NETINS_UDP_ERROR);
    assert(errno == EALREADY);
    assert(netins_udp_h3_request_send(request, "OK", 2, 0) == -1);
    assert(errno == EINVAL);
    assert(netins_udp_h3_request_send(request, "OK", 2,
        NETINS_UDP_MSG_FIN) == 2);
    info = (netins_udp_h3_request_info_t)
        NETINS_UDP_H3_REQUEST_INFO_INITIALIZER;
    assert(netins_udp_h3_request_get_info(request, &info)
        == NETINS_UDP_OK);
    assert(info.response_status == 201);
    assert(info.response_content_length == 2);
    assert(info.bytes_sent == 2);
    assert(info.bytes_received == 6);
    netins_udp_h3_request_close(request);
    assert(g_child_count == 0);

    request = open_request(&connection, "POST",
        NETINS_UDP_CONTENT_LENGTH_UNKNOWN);
    g_send_would_block = 1;
    assert(netins_udp_h3_request_send(request, "abc", 3,
        NETINS_UDP_MSG_DONTWAIT) == -1);
    assert(errno == EAGAIN);
    assert(netins_udp_h3_request_get_last_error_code(request)
        == EAGAIN);
    g_send_would_block = 0;
    assert(netins_udp_h3_request_send(request, "abc", 3, 0) == 3);
    assert(netins_udp_h3_request_send(request, NULL, 0,
        NETINS_UDP_MSG_FIN) == 0);
    netins_udp_h3_request_close(request);

    request = open_request(&connection, "GET", 0);
    pthread_mutex_lock(&g_mock_lock);
    g_block_recv = 1;
    g_recv_entered = 0;
    pthread_mutex_unlock(&g_mock_lock);
    recv_thread_result_t recv_result = {
        .request = request,
        .result = 0,
        .error_code = 0,
    };
    pthread_t recv_thread;
    assert(pthread_create(&recv_thread, NULL, recv_thread_main,
        &recv_result) == 0);
    pthread_mutex_lock(&g_mock_lock);
    while (!g_recv_entered) {
        pthread_cond_wait(&g_mock_cond, &g_mock_lock);
    }
    pthread_mutex_unlock(&g_mock_lock);
    netins_udp_h3_request_close(request);
    assert(pthread_join(recv_thread, NULL) == 0);
    assert(recv_result.result == -1);
    assert(recv_result.error_code == ECANCELED);
    pthread_mutex_lock(&g_mock_lock);
    g_block_recv = 0;
    pthread_mutex_unlock(&g_mock_lock);
    assert(g_child_count == 0);

    strcpy(config.method, "PUT");
    request = NULL;
    assert(netins_udp_h3_request_open(&connection, &config, &request)
        == NETINS_UDP_ERROR);
    assert(errno == EOPNOTSUPP);
    assert(request == NULL);

    assert(g_child_count == 0);
    return 0;
}
