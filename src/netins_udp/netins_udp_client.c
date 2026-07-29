#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "netins_udp_backend.h"
#include "netins_udp_client_internal.h"
#include "netins_udp_observer_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <openssl/x509.h>
#include <openssl/x509_vfy.h>

#include <xquic/xqc_errno.h>
#include <xquic/xqc_http3.h>

#define NETINS_UDP_CLIENT_DEFAULT_TIMEOUT_MS 20000U
#define NETINS_UDP_CLIENT_DEFAULT_SOCKET_BUF (16 * 1024 * 1024)
#define NETINS_UDP_CLIENT_DEFAULT_CWND       100U
#define NETINS_UDP_CLIENT_REFRESH_US         500000ULL
#define NETINS_UDP_CLIENT_PATH_RETRY_US      200000ULL
#define NETINS_UDP_CLIENT_CLOSE_WAIT_MS      500U
#define NETINS_UDP_CLIENT_PATH_FAILOVER_PTO  2U
#define NETINS_UDP_STREAM_DEFAULT_SEND_QUEUE (4U * 1024U * 1024U)
#define NETINS_UDP_STREAM_DEFAULT_RECV_QUEUE (16U * 1024U * 1024U)
#define NETINS_UDP_CLIENT_SEND_QUEUE_BUDGET   (64U * 1024U * 1024U)
#define NETINS_UDP_CLIENT_RECV_QUEUE_BUDGET   (256U * 1024U * 1024U)
#define NETINS_UDP_STREAM_RECV_CHUNK         (64U * 1024U)
#define NETINS_UDP_STREAM_INVALID_ID         UINT64_MAX

typedef struct netins_udp_buffer_s {
    struct netins_udp_buffer_s *next;
    size_t length;
    size_t offset;
    unsigned char data[];
} netins_udp_buffer_t;

typedef struct netins_udp_client_h3_extension_s {
    int request_body_length_unknown;
    int response_content_length_known;
    uint32_t request_header_count;
    netins_udp_client_h3_header_t
        request_headers[NETINS_UDP_CLIENT_H3_MAX_HEADERS];
    uint32_t peer_header_count;
    netins_udp_client_h3_header_t
        peer_headers[NETINS_UDP_CLIENT_H3_MAX_HEADERS];
} netins_udp_client_h3_extension_t;

typedef struct netins_udp_client_rx_batch_s {
    size_t slot_capacity;
    unsigned char *storage;
#if defined(__linux__)
    struct mmsghdr messages[NETINS_UDP_BACKEND_RX_BATCH_CAPACITY];
    struct iovec iovecs[NETINS_UDP_BACKEND_RX_BATCH_CAPACITY];
#endif
} netins_udp_client_rx_batch_t;

struct netins_udp_stream_s {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int ref_count;
    int public_ref;
    int request_ref;

    netins_udp_client_t *client;
    struct netins_udp_stream_s *next;
    xqc_h3_request_t *request;
    netins_udp_stream_config_t config;
    netins_udp_client_h3_extension_t *h3_extension;
    int state;
    int stream_error;
    int stream_system_error;
    uint64_t stream_id;

    int request_headers_sent;
    int response_headers_received;
    int send_fin_requested;
    int send_finished;
    int recv_finished;
    int recv_eof_delivered;
    int close_requested;
    int close_sent;
    int close_notified;
    int read_pending;
    int budget_reserved;

    int response_status;
    uint64_t response_content_length;
    char response_content_type[NETINS_UDP_CONTENT_TYPE_LEN];

    uint64_t bytes_accepted;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_delivered;
    size_t send_queue_bytes;
    size_t recv_queue_bytes;
    netins_udp_buffer_t *send_head;
    netins_udp_buffer_t *send_tail;
    netins_udp_buffer_t *recv_head;
    netins_udp_buffer_t *recv_tail;
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;
};

typedef struct netins_udp_client_path_s {
    int fd;
    int active;
    int write_blocked;
    uint64_t path_id;
    char interface_name[NETINS_UDP_INTERFACE_NAME_LEN];
    struct sockaddr_storage local_addr;
    socklen_t local_addrlen;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addrlen;
    netins_udp_client_rx_batch_t *rx_batch;
} netins_udp_client_path_t;

struct netins_udp_client_s {
    netins_udp_client_config_t config;
    pthread_mutex_t lock;
    pthread_cond_t state_cond;
    pthread_t thread;
    int thread_started;
    int connect_started;
    int stop_requested;
    int close_requested;
    int close_sent;
    uint64_t close_application_error;
    int state;
    int last_error_code;
    char last_error_msg[NETINS_UDP_ERROR_MSG_LEN];

    int wake_pipe[2];
    xqc_engine_t *engine;
    xqc_h3_conn_t *h3_conn;
    xqc_cid_t cid;
    int cid_valid;
    netins_udp_observer_t *observer;

    netins_udp_client_path_t paths[NETINS_UDP_MAX_PATHS];
    uint32_t path_count;
    size_t rx_next_path_start;
    netins_udp_backend_rx_policy_t rx_policy;
    int path_creation_ready;
    uint64_t next_wake_us;
    uint64_t last_refresh_us;
    uint64_t last_path_attempt_us;
    uint64_t connect_deadline_us;

    netins_udp_stream_t *streams;
    size_t stream_count;
    size_t reserved_send_queue_bytes;
    size_t reserved_recv_queue_bytes;

    xqc_cong_ctrl_callback_t congestion_control;
    xqc_scheduler_callback_t scheduler;
    uint32_t idle_timeout_ms;
    uint32_t close_timeout_ms;
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;

    char ciphers[256];
    char groups[128];
    X509_STORE *ca_store;
};

static void
netins_udp_client_notify(netins_udp_client_t *client, uint32_t events)
{
    if (client == NULL || events == 0) {
        return;
    }
    pthread_mutex_lock(&client->lock);
    if (client->notify != NULL) {
        client->notify(client->notify_user_data, events);
    }
    pthread_mutex_unlock(&client->lock);
}

static void
netins_udp_stream_notify(netins_udp_stream_t *stream, uint32_t events)
{
    if (stream == NULL || events == 0) {
        return;
    }
    pthread_mutex_lock(&stream->lock);
    if (stream->notify != NULL) {
        stream->notify(stream->notify_user_data, events);
    }
    pthread_mutex_unlock(&stream->lock);
}

static void
netins_udp_stream_free_queue(netins_udp_buffer_t *head)
{
    while (head != NULL) {
        netins_udp_buffer_t *next = head->next;
        free(head);
        head = next;
    }
}

static void
netins_udp_stream_destroy(netins_udp_stream_t *stream)
{
    netins_udp_stream_free_queue(stream->send_head);
    netins_udp_stream_free_queue(stream->recv_head);
    free(stream->h3_extension);
    pthread_cond_destroy(&stream->cond);
    pthread_mutex_destroy(&stream->lock);
    free(stream);
}

static void
netins_udp_stream_retain(netins_udp_stream_t *stream)
{
    pthread_mutex_lock(&stream->lock);
    stream->ref_count++;
    pthread_mutex_unlock(&stream->lock);
}

static void
netins_udp_stream_release(netins_udp_stream_t *stream)
{
    int destroy = 0;
    pthread_mutex_lock(&stream->lock);
    if (--stream->ref_count == 0) {
        destroy = 1;
    }
    pthread_mutex_unlock(&stream->lock);
    if (destroy) {
        netins_udp_stream_destroy(stream);
    }
}

static void
netins_udp_client_detach_stream(netins_udp_client_t *client,
    netins_udp_stream_t *stream)
{
    int release_list_ref = 0;
    if (client == NULL || stream == NULL) {
        return;
    }

    pthread_mutex_lock(&client->lock);
    netins_udp_stream_t **cursor = &client->streams;
    while (*cursor != NULL) {
        if (*cursor == stream) {
            *cursor = stream->next;
            stream->next = NULL;
            if (client->stream_count > 0) {
                client->stream_count--;
            }
            if (stream->budget_reserved) {
                stream->budget_reserved = 0;
                if (client->reserved_send_queue_bytes
                    >= stream->config.send_queue_limit_bytes)
                {
                    client->reserved_send_queue_bytes -=
                        stream->config.send_queue_limit_bytes;
                } else {
                    client->reserved_send_queue_bytes = 0;
                }
                if (client->reserved_recv_queue_bytes
                    >= stream->config.recv_queue_limit_bytes)
                {
                    client->reserved_recv_queue_bytes -=
                        stream->config.recv_queue_limit_bytes;
                } else {
                    client->reserved_recv_queue_bytes = 0;
                }
            }
            release_list_ref = 1;
            break;
        }
        cursor = &(*cursor)->next;
    }
    pthread_mutex_unlock(&client->lock);

    if (release_list_ref) {
        netins_udp_stream_release(stream);
    }
}

static void
netins_udp_stream_mark_error_detail(netins_udp_stream_t *stream,
    int raw_error, int system_error)
{
    pthread_mutex_lock(&stream->lock);
    if (stream->state != NETINS_UDP_STREAM_STATE_CLOSED) {
        stream->state = NETINS_UDP_STREAM_STATE_ERROR;
    }
    if (stream->stream_error == 0) {
        stream->stream_error = raw_error != 0 ? raw_error : EIO;
        stream->stream_system_error =
            system_error != 0 ? system_error : EIO;
    }
    stream->close_requested = 1;
    pthread_cond_broadcast(&stream->cond);
    pthread_mutex_unlock(&stream->lock);
    netins_udp_stream_notify(stream, NETINS_UDP_V2_EVENT_ERROR);
}

static netins_udp_buffer_t *
netins_udp_stream_copy_buffer(const void *data, size_t size)
{
    if (size > SIZE_MAX - sizeof(netins_udp_buffer_t)) {
        return NULL;
    }
    netins_udp_buffer_t *buffer = malloc(sizeof(*buffer) + size);
    if (buffer == NULL) {
        return NULL;
    }
    buffer->next = NULL;
    buffer->length = size;
    buffer->offset = 0;
    if (size > 0 && data != NULL) {
        memcpy(buffer->data, data, size);
    }
    return buffer;
}

static int
netins_udp_stream_header_name_equals(const xqc_http_header_t *header,
    const char *name)
{
    size_t name_len = strlen(name);
    return header->name.iov_len == name_len
        && strncasecmp((const char *)header->name.iov_base, name,
            name_len) == 0;
}

static int
netins_udp_stream_copy_header_value(char *destination,
    size_t destination_size, const xqc_http_header_t *header)
{
    if (destination == NULL || destination_size == 0 || header == NULL
        || header->value.iov_len >= destination_size)
    {
        return -1;
    }
    size_t length = header->value.iov_len;
    memcpy(destination, header->value.iov_base, length);
    destination[length] = '\0';
    return 0;
}

static void
netins_udp_stream_mark_error(netins_udp_stream_t *stream, int error_code)
{
    int system_error = error_code != 0 ? error_code : EIO;
    netins_udp_stream_mark_error_detail(stream, system_error, system_error);
}

static int
netins_udp_stream_transport_errno(int raw_error)
{
    switch (raw_error) {
    case XQC_ENULLPTR:
    case XQC_EMALLOC:
    case XQC_H3_EMALLOC:
        return ENOMEM;
    case XQC_ENOBUF:
    case XQC_ELIMIT:
    case XQC_H3_BUFFER_EXCEED:
        return ENOBUFS;
    case XQC_EPARAM:
    case XQC_H3_EPARAM:
        return EINVAL;
    case XQC_ESTREAM_RESET:
    case H3_REQUEST_INCOMPLETE:
        return ECONNRESET;
    case XQC_CLOSING:
    case H3_REQUEST_CANCELLED:
        return ECANCELED;
    case XQC_EPROTO:
    case XQC_EFINAL_SIZE:
    case XQC_H3_INVALID_HEADER:
    case XQC_H3_DECODE_ERROR:
        return EPROTO;
    default:
        return EIO;
    }
}

static void
netins_udp_stream_mark_transport_error(netins_udp_stream_t *stream,
    int raw_error)
{
    netins_udp_stream_mark_error_detail(stream, raw_error,
        netins_udp_stream_transport_errno(raw_error));
}

static int
netins_udp_stream_parse_decimal_u64(const xqc_http_header_t *header,
    uint64_t *out)
{
    if (header == NULL || out == NULL || header->value.iov_len == 0) {
        return -1;
    }

    uint64_t parsed = 0;
    const unsigned char *value = header->value.iov_base;
    for (size_t i = 0; i < header->value.iov_len; ++i) {
        if (value[i] < '0' || value[i] > '9'
            || parsed > (UINT64_MAX - (uint64_t)(value[i] - '0')) / 10U)
        {
            return -1;
        }
        parsed = parsed * 10U + (uint64_t)(value[i] - '0');
    }
    *out = parsed;
    return 0;
}

static int
netins_udp_stream_parse_status(const xqc_http_header_t *header, int *out)
{
    if (header == NULL || out == NULL || header->value.iov_len != 3) {
        return -1;
    }
    const unsigned char *value = header->value.iov_base;
    if (value[0] < '0' || value[0] > '9'
        || value[1] < '0' || value[1] > '9'
        || value[2] < '0' || value[2] > '9')
    {
        return -1;
    }
    int status = (value[0] - '0') * 100
        + (value[1] - '0') * 10 + (value[2] - '0');
    if (status < 100 || status > 599 || status == 101) {
        return -1;
    }
    *out = status;
    return 0;
}

static int
netins_udp_stream_parse_response_headers(netins_udp_stream_t *stream,
    const xqc_http_headers_t *headers)
{
    int status_seen = 0;
    int status = 0;
    int content_length_seen = 0;
    uint64_t content_length = 0;
    char content_type[NETINS_UDP_CONTENT_TYPE_LEN] = {0};
    int content_type_seen = 0;
    uint32_t peer_header_count = 0;
    netins_udp_client_h3_header_t
        peer_headers[NETINS_UDP_CLIENT_H3_MAX_HEADERS];
    memset(peer_headers, 0, sizeof(peer_headers));

    for (size_t i = 0; i < headers->count; ++i) {
        const xqc_http_header_t *header = &headers->headers[i];
        if (netins_udp_stream_header_name_equals(header, ":status")) {
            if (status_seen
                || netins_udp_stream_parse_status(header, &status) != 0)
            {
                return -1;
            }
            status_seen = 1;

        } else if (netins_udp_stream_header_name_equals(header,
                       "content-length"))
        {
            if (content_length_seen
                || netins_udp_stream_parse_decimal_u64(header,
                    &content_length) != 0)
            {
                return -1;
            }
            content_length_seen = 1;

        } else if (netins_udp_stream_header_name_equals(header,
                       "content-type"))
        {
            if (content_type_seen
                || netins_udp_stream_copy_header_value(content_type,
                    sizeof(content_type), header) != 0)
            {
                return -1;
            }
            content_type_seen = 1;

        } else if (header->name.iov_len > 0
            && ((const unsigned char *)header->name.iov_base)[0] != ':'
            && !netins_udp_stream_header_name_equals(header, "host")
            && peer_header_count < NETINS_UDP_CLIENT_H3_MAX_HEADERS
            && header->name.iov_len
                < sizeof(peer_headers[peer_header_count].name)
            && header->value.iov_len
                < sizeof(peer_headers[peer_header_count].value))
        {
            netins_udp_client_h3_header_t *destination =
                &peer_headers[peer_header_count++];
            memcpy(destination->name, header->name.iov_base,
                header->name.iov_len);
            destination->name[header->name.iov_len] = '\0';
            memcpy(destination->value, header->value.iov_base,
                header->value.iov_len);
            destination->value[header->value.iov_len] = '\0';
        }
    }

    if (!status_seen || (status < 200 && content_length_seen)
        || (status == 204 && content_length_seen)
        || (status == 205
            && content_length_seen && content_length != 0))
    {
        return -1;
    }

    /*
     * Informational responses are valid HTTP/3 header blocks, but the public
     * API exposes only the final response.  Ignore 1xx metadata and wait for
     * the following final header block.
     */
    if (status < 200) {
        return 0;
    }

    pthread_mutex_lock(&stream->lock);
    if (stream->response_headers_received) {
        pthread_mutex_unlock(&stream->lock);
        return -1;
    }
    stream->response_status = status;
    stream->response_content_length = content_length;
    snprintf(stream->response_content_type,
        sizeof(stream->response_content_type), "%s", content_type);
    if (stream->h3_extension != NULL) {
        stream->h3_extension->response_content_length_known =
            content_length_seen;
        stream->h3_extension->peer_header_count = peer_header_count;
        memcpy(stream->h3_extension->peer_headers, peer_headers,
            sizeof(peer_headers));
    }
    stream->response_headers_received = 1;
    pthread_cond_broadcast(&stream->cond);
    pthread_mutex_unlock(&stream->lock);
    netins_udp_stream_notify(stream, NETINS_UDP_V2_EVENT_READABLE);
    return 0;
}

static int
netins_udp_stream_response_body_forbidden_locked(
    const netins_udp_stream_t *stream)
{
    return strcasecmp(stream->config.method, "HEAD") == 0
        || stream->response_status == 204
        || stream->response_status == 205
        || stream->response_status == 304;
}

static int
netins_udp_stream_finish_response(netins_udp_stream_t *stream)
{
    int protocol_error = 0;
    pthread_mutex_lock(&stream->lock);
    int body_forbidden =
        netins_udp_stream_response_body_forbidden_locked(stream);
    if (!stream->response_headers_received
        || (body_forbidden && stream->bytes_received != 0)
        || (!body_forbidden && stream->h3_extension != NULL
            && stream->h3_extension->response_content_length_known
            && stream->bytes_received != stream->response_content_length))
    {
        protocol_error = 1;
    } else {
        stream->recv_finished = 1;
        stream->read_pending = 0;
        pthread_cond_broadcast(&stream->cond);
    }
    pthread_mutex_unlock(&stream->lock);

    if (protocol_error) {
        netins_udp_stream_mark_error(stream, EPROTO);
        return -1;
    }
    netins_udp_stream_notify(stream, NETINS_UDP_V2_EVENT_READABLE);
    return 0;
}

static void netins_udp_client_process_streams(netins_udp_client_t *client);
static void netins_udp_client_detach_all_streams(
    netins_udp_client_t *client, int error_code);

static uint64_t
netins_udp_client_now_us(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL
        + (uint64_t)ts.tv_nsec / 1000ULL;
}

static uint64_t
netins_udp_client_packet_time_us(void)
{
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static void
netins_udp_client_set_error_locked(netins_udp_client_t *client, int error_code,
    const char *message)
{
    client->last_error_code = error_code;
    if (message == NULL) {
        client->last_error_msg[0] = '\0';
    } else {
        snprintf(client->last_error_msg, sizeof(client->last_error_msg),
            "%s", message);
    }
}

static void
netins_udp_client_set_error(netins_udp_client_t *client, int error_code,
    const char *message)
{
    if (client == NULL) {
        return;
    }
    pthread_mutex_lock(&client->lock);
    netins_udp_client_set_error_locked(client, error_code, message);
    pthread_mutex_unlock(&client->lock);
}

static void
netins_udp_client_set_errno_error(netins_udp_client_t *client,
    const char *operation)
{
    int error_code = errno;
    char message[NETINS_UDP_ERROR_MSG_LEN];
    snprintf(message, sizeof(message), "%s: %s",
        operation != NULL ? operation : "network operation",
        strerror(error_code));
    netins_udp_client_set_error(client, error_code, message);
}

static void
netins_udp_client_fail_runtime(netins_udp_client_t *client, int error_code,
    const char *message)
{
    pthread_mutex_lock(&client->lock);
    if (client->state != NETINS_UDP_STATE_CLOSED) {
        client->state = NETINS_UDP_STATE_ERROR;
    }
    netins_udp_client_set_error_locked(client, error_code, message);
    pthread_cond_broadcast(&client->state_cond);
    pthread_mutex_unlock(&client->lock);

    if (client->observer != NULL) {
        (void)netins_udp_backend_connection_error(client->observer,
            error_code, message);
    }
    netins_udp_client_notify(client, NETINS_UDP_V2_EVENT_ERROR);
}

static int
netins_udp_client_handle_path_error(netins_udp_client_t *client,
    netins_udp_client_path_t *path, int error_code, const char *operation)
{
    char message[NETINS_UDP_ERROR_MSG_LEN];
    snprintf(message, sizeof(message), "%s: %s",
        operation != NULL ? operation : "network operation",
        strerror(error_code));

    if (client->engine != NULL && client->cid_valid && path != NULL
        && path->active && path->path_id != UINT64_MAX
        && xqc_conn_close_path(client->engine, &client->cid, path->path_id)
            == XQC_OK)
    {
        netins_udp_client_set_error(client, error_code, message);
        client->last_path_attempt_us = 0;
        return NETINS_UDP_OK;
    }

    netins_udp_client_fail_runtime(client, error_code, message);
    if (client->engine != NULL && client->cid_valid) {
        (void)xqc_h3_conn_close(client->engine, &client->cid);
    }
    return NETINS_UDP_ERROR;
}

static void
netins_udp_client_wake(netins_udp_client_t *client)
{
    if (client == NULL || client->wake_pipe[1] < 0) {
        return;
    }
    unsigned char byte = 1;
    ssize_t written;
    do {
        written = write(client->wake_pipe[1], &byte, sizeof(byte));
    } while (written < 0 && errno == EINTR);
    (void)written;
}

static void
netins_udp_client_drain_wake_pipe(netins_udp_client_t *client)
{
    unsigned char buffer[64];
    while (client->wake_pipe[0] >= 0) {
        ssize_t received = read(client->wake_pipe[0], buffer, sizeof(buffer));
        if (received > 0) {
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

static int
netins_udp_client_make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return NETINS_UDP_ERROR;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return NETINS_UDP_ERROR;
    }
    flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return NETINS_UDP_ERROR;
    }
    return NETINS_UDP_OK;
}

static int
netins_udp_client_init_wake_pipe(netins_udp_client_t *client)
{
    if (pipe(client->wake_pipe) != 0) {
        netins_udp_client_set_errno_error(client, "pipe");
        return NETINS_UDP_ERROR;
    }
    if (netins_udp_client_make_nonblocking(client->wake_pipe[0])
            != NETINS_UDP_OK
        || netins_udp_client_make_nonblocking(client->wake_pipe[1])
            != NETINS_UDP_OK)
    {
        netins_udp_client_set_errno_error(client, "fcntl wake pipe");
        close(client->wake_pipe[0]);
        close(client->wake_pipe[1]);
        client->wake_pipe[0] = -1;
        client->wake_pipe[1] = -1;
        return NETINS_UDP_ERROR;
    }
    return NETINS_UDP_OK;
}

typedef struct netins_udp_resolver_task_s {
    struct netins_udp_resolver_task_s *next_abandoned;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    int done;
    int abandoned;
    int gai_error;
    int address_error;
    char host[NETINS_UDP_SERVER_ADDRESS_LEN];
    char port[16];
    struct sockaddr_storage peer_addr;
    socklen_t peer_addrlen;
} netins_udp_resolver_task_t;

static pthread_mutex_t g_netins_udp_resolver_count_lock =
    PTHREAD_MUTEX_INITIALIZER;
static size_t g_netins_udp_resolver_count;
static netins_udp_resolver_task_t *g_netins_udp_abandoned_resolvers;

static int netins_udp_client_check_startup_abort(
    netins_udp_client_t *client);
static void netins_udp_resolver_task_destroy(
    netins_udp_resolver_task_t *task);

static void
netins_udp_client_resolver_count_add(int delta)
{
    pthread_mutex_lock(&g_netins_udp_resolver_count_lock);
    if (delta > 0) {
        g_netins_udp_resolver_count++;
    } else if (g_netins_udp_resolver_count > 0) {
        g_netins_udp_resolver_count--;
    }
    pthread_mutex_unlock(&g_netins_udp_resolver_count_lock);
}

int
netins_udp_client_has_pending_resolvers(void)
{
    /*
     * Abandoned getaddrinfo tasks stay joinable. Reap only tasks whose start
     * routines have fully returned; this prevents final library cleanup from
     * succeeding while a detached thread still executes code in libxquic.
     */
    for (;;) {
        netins_udp_resolver_task_t *finished = NULL;

        pthread_mutex_lock(&g_netins_udp_resolver_count_lock);
        netins_udp_resolver_task_t **link =
            &g_netins_udp_abandoned_resolvers;
        while (*link != NULL) {
            netins_udp_resolver_task_t *candidate = *link;
            pthread_mutex_lock(&candidate->lock);
            int done = candidate->done;
            pthread_mutex_unlock(&candidate->lock);
            if (done) {
                *link = candidate->next_abandoned;
                candidate->next_abandoned = NULL;
                finished = candidate;
                break;
            }
            link = &candidate->next_abandoned;
        }
        pthread_mutex_unlock(&g_netins_udp_resolver_count_lock);

        if (finished == NULL) {
            break;
        }
        if (pthread_join(finished->thread, NULL) != 0) {
            pthread_mutex_lock(&g_netins_udp_resolver_count_lock);
            finished->next_abandoned =
                g_netins_udp_abandoned_resolvers;
            g_netins_udp_abandoned_resolvers = finished;
            pthread_mutex_unlock(&g_netins_udp_resolver_count_lock);
            break;
        }
        netins_udp_resolver_task_destroy(finished);
        netins_udp_client_resolver_count_add(-1);
    }

    pthread_mutex_lock(&g_netins_udp_resolver_count_lock);
    int pending = g_netins_udp_resolver_count != 0;
    pthread_mutex_unlock(&g_netins_udp_resolver_count_lock);
    return pending;
}

static void
netins_udp_resolver_task_destroy(netins_udp_resolver_task_t *task)
{
    pthread_cond_destroy(&task->cond);
    pthread_mutex_destroy(&task->lock);
    free(task);
}

static void *
netins_udp_resolver_thread_main(void *argument)
{
    netins_udp_resolver_task_t *task = argument;
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    int gai_error = getaddrinfo(task->host, task->port, &hints, &result);
    int address_error = EAFNOSUPPORT;
    struct sockaddr_storage peer_addr;
    socklen_t peer_addrlen = 0;
    memset(&peer_addr, 0, sizeof(peer_addr));
    if (gai_error == 0) {
        for (struct addrinfo *item = result; item != NULL;
             item = item->ai_next)
        {
            if ((item->ai_family == AF_INET || item->ai_family == AF_INET6)
                && item->ai_addrlen <= sizeof(peer_addr))
            {
                memcpy(&peer_addr, item->ai_addr, item->ai_addrlen);
                peer_addrlen = (socklen_t)item->ai_addrlen;
                address_error = 0;
                break;
            }
        }
    }
    if (result != NULL) {
        freeaddrinfo(result);
    }

    pthread_mutex_lock(&task->lock);
    if (task->abandoned) {
        task->done = 1;
        pthread_cond_broadcast(&task->cond);
        pthread_mutex_unlock(&task->lock);
        return NULL;
    }
    task->gai_error = gai_error;
    task->address_error = address_error;
    task->peer_addr = peer_addr;
    task->peer_addrlen = peer_addrlen;
    task->done = 1;
    pthread_cond_broadcast(&task->cond);
    pthread_mutex_unlock(&task->lock);
    return NULL;
}

static void
netins_udp_client_register_abandoned_resolver(
    netins_udp_resolver_task_t *task)
{
    pthread_mutex_lock(&g_netins_udp_resolver_count_lock);
    task->next_abandoned = g_netins_udp_abandoned_resolvers;
    g_netins_udp_abandoned_resolvers = task;
    pthread_mutex_unlock(&g_netins_udp_resolver_count_lock);
}

static int
netins_udp_client_make_resolver_deadline(struct timespec *deadline)
{
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        return errno != 0 ? errno : EIO;
    }
    deadline->tv_nsec += 100000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int
netins_udp_client_resolve_peer(netins_udp_client_t *client,
    struct sockaddr_storage *peer_addr, socklen_t *peer_addrlen)
{
    netins_udp_resolver_task_t *task = calloc(1, sizeof(*task));
    if (task == NULL) {
        netins_udp_client_set_error(client, ENOMEM,
            "failed to allocate DNS resolver task");
        return NETINS_UDP_ERROR;
    }
    int init_error = pthread_mutex_init(&task->lock, NULL);
    if (init_error != 0) {
        free(task);
        netins_udp_client_set_error(client, init_error,
            "failed to initialize DNS resolver task");
        return NETINS_UDP_ERROR;
    }
    init_error = pthread_cond_init(&task->cond, NULL);
    if (init_error != 0) {
        pthread_mutex_destroy(&task->lock);
        free(task);
        netins_udp_client_set_error(client, init_error,
            "failed to initialize DNS resolver task");
        return NETINS_UDP_ERROR;
    }
    snprintf(task->host, sizeof(task->host), "%s",
        client->config.server_address);
    snprintf(task->port, sizeof(task->port), "%u",
        (unsigned)client->config.server_port);

    netins_udp_client_resolver_count_add(1);
    int thread_error = pthread_create(&task->thread, NULL,
        netins_udp_resolver_thread_main, task);
    if (thread_error != 0) {
        netins_udp_client_resolver_count_add(-1);
        netins_udp_resolver_task_destroy(task);
        netins_udp_client_set_error(client, thread_error,
            "failed to start DNS resolver task");
        return NETINS_UDP_ERROR;
    }
    pthread_t resolver_thread = task->thread;

    int abandoned = 0;
    pthread_mutex_lock(&task->lock);
    while (!task->done) {
        struct timespec deadline;
        int deadline_error =
            netins_udp_client_make_resolver_deadline(&deadline);
        if (deadline_error != 0) {
            task->abandoned = 1;
            abandoned = 1;
            pthread_mutex_unlock(&task->lock);
            netins_udp_client_register_abandoned_resolver(task);
            netins_udp_client_set_error(client, deadline_error,
                "failed to create DNS resolver wait deadline");
            return NETINS_UDP_ERROR;
        }
        int wait_error =
            pthread_cond_timedwait(&task->cond, &task->lock, &deadline);
        if (wait_error != 0 && wait_error != ETIMEDOUT) {
            task->abandoned = 1;
            abandoned = 1;
            pthread_mutex_unlock(&task->lock);
            netins_udp_client_register_abandoned_resolver(task);
            netins_udp_client_set_error(client, wait_error,
                "failed while waiting for DNS resolver");
            return NETINS_UDP_ERROR;
        }
        if (task->done) {
            break;
        }
        pthread_mutex_unlock(&task->lock);
        int abort_requested =
            netins_udp_client_check_startup_abort(client);
        pthread_mutex_lock(&task->lock);
        if (abort_requested && !task->done) {
            task->abandoned = 1;
            abandoned = 1;
            break;
        }
    }
    int gai_error = task->gai_error;
    int address_error = task->address_error;
    struct sockaddr_storage resolved_addr = task->peer_addr;
    socklen_t resolved_addrlen = task->peer_addrlen;
    pthread_mutex_unlock(&task->lock);

    if (abandoned) {
        netins_udp_client_register_abandoned_resolver(task);
        return NETINS_UDP_ERROR;
    }
    int join_error = pthread_join(resolver_thread, NULL);
    if (join_error != 0) {
        netins_udp_client_register_abandoned_resolver(task);
        netins_udp_client_set_error(client, join_error,
            "failed to join DNS resolver task");
        return NETINS_UDP_ERROR;
    }
    netins_udp_resolver_task_destroy(task);
    netins_udp_client_resolver_count_add(-1);

    if (netins_udp_client_check_startup_abort(client)) {
        return NETINS_UDP_ERROR;
    }
    if (gai_error != 0) {
        netins_udp_client_set_error(client, EHOSTUNREACH,
            gai_strerror(gai_error));
        return NETINS_UDP_ERROR;
    }
    if (address_error != 0 || resolved_addrlen == 0) {
        netins_udp_client_set_error(client, EAFNOSUPPORT,
            "server address has no supported UDP address");
        return NETINS_UDP_ERROR;
    }
    *peer_addr = resolved_addr;
    *peer_addrlen = resolved_addrlen;
    return NETINS_UDP_OK;
}

static int
netins_udp_client_alloc_rx_batch(netins_udp_client_path_t *path)
{
    netins_udp_client_rx_batch_t *batch = calloc(1, sizeof(*batch));
    if (batch == NULL) {
        return NETINS_UDP_ERROR;
    }
    batch->slot_capacity = 65536U;
    batch->storage = malloc(batch->slot_capacity
        * NETINS_UDP_BACKEND_RX_BATCH_CAPACITY);
    if (batch->storage == NULL) {
        free(batch);
        return NETINS_UDP_ERROR;
    }
#if defined(__linux__)
    for (size_t i = 0; i < NETINS_UDP_BACKEND_RX_BATCH_CAPACITY; ++i) {
        batch->iovecs[i].iov_base = batch->storage
            + i * batch->slot_capacity;
        batch->iovecs[i].iov_len = batch->slot_capacity;
        batch->messages[i].msg_hdr.msg_iov = &batch->iovecs[i];
        batch->messages[i].msg_hdr.msg_iovlen = 1;
    }
#endif
    path->rx_batch = batch;
    return NETINS_UDP_OK;
}

static int
netins_udp_client_create_socket(netins_udp_client_t *client,
    netins_udp_client_path_t *path, const char *interface_name,
    const struct sockaddr_storage *peer_addr, socklen_t peer_addrlen)
{
    int fd = socket(peer_addr->ss_family, SOCK_DGRAM, 0);
    if (fd < 0) {
        netins_udp_client_set_errno_error(client, "socket");
        return NETINS_UDP_ERROR;
    }

    if (netins_udp_client_make_nonblocking(fd) != NETINS_UDP_OK) {
        netins_udp_client_set_errno_error(client, "fcntl socket");
        close(fd);
        return NETINS_UDP_ERROR;
    }

    int socket_buf = client->config.kernel_sndbuf;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &socket_buf,
            sizeof(socket_buf)) != 0)
    {
        netins_udp_client_set_errno_error(client, "setsockopt SO_SNDBUF");
        close(fd);
        return NETINS_UDP_ERROR;
    }
    socket_buf = client->config.kernel_rcvbuf;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &socket_buf,
            sizeof(socket_buf)) != 0)
    {
        netins_udp_client_set_errno_error(client, "setsockopt SO_RCVBUF");
        close(fd);
        return NETINS_UDP_ERROR;
    }

    if (interface_name != NULL && interface_name[0] != '\0') {
#if defined(SO_BINDTODEVICE)
        if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name,
                (socklen_t)(strlen(interface_name) + 1)) != 0)
        {
            netins_udp_client_set_errno_error(client,
                "setsockopt SO_BINDTODEVICE");
            close(fd);
            return NETINS_UDP_ERROR;
        }
#else
        netins_udp_client_set_error(client, ENOTSUP,
            "binding a UDP path to an interface is not supported");
        close(fd);
        return NETINS_UDP_ERROR;
#endif
    }

    if (connect(fd, (const struct sockaddr *)peer_addr, peer_addrlen) != 0) {
        netins_udp_client_set_errno_error(client, "connect UDP socket");
        close(fd);
        return NETINS_UDP_ERROR;
    }

    memset(path, 0, sizeof(*path));
    path->fd = fd;
    path->path_id = UINT64_MAX;
    path->peer_addr = *peer_addr;
    path->peer_addrlen = peer_addrlen;
    if (interface_name != NULL) {
        snprintf(path->interface_name, sizeof(path->interface_name), "%s",
            interface_name);
    }
    path->local_addrlen = sizeof(path->local_addr);
    if (getsockname(fd, (struct sockaddr *)&path->local_addr,
            &path->local_addrlen) != 0)
    {
        netins_udp_client_set_errno_error(client, "getsockname");
        close(fd);
        path->fd = -1;
        return NETINS_UDP_ERROR;
    }
    if (netins_udp_client_alloc_rx_batch(path) != NETINS_UDP_OK) {
        netins_udp_client_set_error(client, ENOMEM,
            "failed to allocate the bounded UDP receive batch");
        close(fd);
        path->fd = -1;
        return NETINS_UDP_ERROR;
    }
    return NETINS_UDP_OK;
}

static void
netins_udp_client_close_path(netins_udp_client_path_t *path)
{
    if (path->fd >= 0) {
        close(path->fd);
    }
    path->fd = -1;
    path->active = 0;
    path->write_blocked = 0;
    if (path->rx_batch != NULL) {
        free(path->rx_batch->storage);
        free(path->rx_batch);
        path->rx_batch = NULL;
    }
}

static netins_udp_client_path_t *
netins_udp_client_find_path(netins_udp_client_t *client, uint64_t path_id)
{
    for (uint32_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        if (client->paths[i].active && client->paths[i].path_id == path_id) {
            return &client->paths[i];
        }
    }
    for (uint32_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        if (client->paths[i].active) {
            return &client->paths[i];
        }
    }
    return NULL;
}

static void
netins_udp_client_update_path_metadata(netins_udp_client_t *client,
    netins_udp_client_path_t *path)
{
    if (client->observer == NULL || path == NULL || !path->active) {
        return;
    }
    (void)netins_udp_backend_set_path_metadata(client->observer, path->path_id,
        path->interface_name[0] != '\0' ? path->interface_name : NULL,
        (const struct sockaddr *)&path->local_addr, path->local_addrlen,
        (const struct sockaddr *)&path->peer_addr, path->peer_addrlen);
}

static void
netins_udp_client_set_event_timer(xqc_usec_t wake_after, void *engine_user_data)
{
    netins_udp_client_t *client = engine_user_data;
    uint64_t now = netins_udp_client_now_us();
    pthread_mutex_lock(&client->lock);
    client->next_wake_us = now + (wake_after > 0 ? wake_after : 1);
    pthread_mutex_unlock(&client->lock);
}

static ssize_t
netins_udp_client_write_socket_ex(uint64_t path_id,
    const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *conn_user_data)
{
    netins_udp_client_t *client = conn_user_data;
    netins_udp_client_path_t *path =
        netins_udp_client_find_path(client, path_id);
    if (path == NULL || path->fd < 0) {
        return XQC_SOCKET_EAGAIN;
    }

    ssize_t sent;
    do {
        sent = sendto(path->fd, buf, size, 0, peer_addr, peer_addrlen);
    } while (sent < 0 && errno == EINTR);

    if (sent >= 0) {
        path->write_blocked = 0;
        return sent;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        path->write_blocked = 1;
        return XQC_SOCKET_EAGAIN;
    }
    netins_udp_client_set_errno_error(client, "sendto");
    return XQC_SOCKET_ERROR;
}

static ssize_t
netins_udp_client_write_socket(const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen,
    void *conn_user_data)
{
    return netins_udp_client_write_socket_ex(XQC_INITIAL_PATH_ID, buf, size,
        peer_addr, peer_addrlen, conn_user_data);
}

static void
netins_udp_client_ignore_token(const unsigned char *token, uint32_t token_len,
    void *conn_user_data)
{
    (void)token;
    (void)token_len;
    (void)conn_user_data;
}

static void
netins_udp_client_ignore_string(const char *data, size_t data_len,
    void *conn_user_data)
{
    (void)data;
    (void)data_len;
    (void)conn_user_data;
}

static int
netins_udp_client_verify_certificate(const unsigned char *certs[],
    const size_t cert_len[], size_t certs_len, void *conn_user_data)
{
    netins_udp_client_t *client = conn_user_data;
    if (client == NULL || client->ca_store == NULL || certs == NULL
        || cert_len == NULL || certs_len == 0
        || cert_len[0] > (size_t)LONG_MAX)
    {
        return XQC_ERROR;
    }

    const unsigned char *cursor = certs[0];
    X509 *leaf = d2i_X509(NULL, &cursor, (long)cert_len[0]);
    STACK_OF(X509) *untrusted = sk_X509_new_null();
    X509_STORE_CTX *store_context = X509_STORE_CTX_new();
    if (leaf == NULL || untrusted == NULL || store_context == NULL) {
        X509_free(leaf);
        sk_X509_free(untrusted);
        X509_STORE_CTX_free(store_context);
        return XQC_ERROR;
    }

    int ok = 1;
    for (size_t i = 1; i < certs_len; ++i) {
        if (cert_len[i] > (size_t)LONG_MAX) {
            ok = 0;
            break;
        }
        cursor = certs[i];
        X509 *certificate = d2i_X509(NULL, &cursor, (long)cert_len[i]);
        if (certificate == NULL || !sk_X509_push(untrusted, certificate)) {
            X509_free(certificate);
            ok = 0;
            break;
        }
    }
    if (ok && X509_STORE_CTX_init(store_context, client->ca_store,
            leaf, untrusted) != 1)
    {
        ok = 0;
    }
    const char *server_name = client->config.server_name[0] != '\0'
        ? client->config.server_name : client->config.server_address;
    if (ok && server_name[0] != '\0') {
        X509_VERIFY_PARAM *verify_param =
            X509_STORE_CTX_get0_param(store_context);
        struct in_addr ipv4;
        struct in6_addr ipv6;
        int name_result;
        if (inet_pton(AF_INET, server_name, &ipv4) == 1
            || inet_pton(AF_INET6, server_name, &ipv6) == 1)
        {
            name_result =
                X509_VERIFY_PARAM_set1_ip_asc(verify_param, server_name);
        } else {
            name_result =
                X509_VERIFY_PARAM_set1_host(verify_param, server_name, 0);
        }
        if (name_result != 1) {
            ok = 0;
        }
    }
    if (ok) {
        ok = X509_verify_cert(store_context) == 1;
    }

    X509_STORE_CTX_free(store_context);
    sk_X509_pop_free(untrusted, X509_free);
    X509_free(leaf);
    return ok ? XQC_OK : XQC_ERROR;
}

static void
netins_udp_client_update_cid(xqc_connection_t *conn,
    const xqc_cid_t *retire_cid, const xqc_cid_t *new_cid,
    void *conn_user_data)
{
    (void)conn;
    (void)retire_cid;
    netins_udp_client_t *client = conn_user_data;
    if (client == NULL || new_cid == NULL) {
        return;
    }

    pthread_mutex_lock(&client->lock);
    memcpy(&client->cid, new_cid, sizeof(client->cid));
    client->cid_valid = 1;
    pthread_mutex_unlock(&client->lock);
    if (client->observer != NULL) {
        (void)netins_udp_backend_connection_update_cid(client->observer,
            new_cid);
    }
}

static void
netins_udp_client_ready_to_create_path(const xqc_cid_t *cid,
    void *conn_user_data)
{
    (void)cid;
    netins_udp_client_t *client = conn_user_data;
    pthread_mutex_lock(&client->lock);
    client->path_creation_ready = 1;
    pthread_mutex_unlock(&client->lock);
}

static void
netins_udp_client_path_removed(const xqc_cid_t *cid, uint64_t path_id,
    void *conn_user_data)
{
    (void)cid;
    netins_udp_client_t *client = conn_user_data;
    for (uint32_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        if (client->paths[i].active && client->paths[i].path_id == path_id) {
            netins_udp_client_close_path(&client->paths[i]);
            if (client->path_count > 0) {
                client->path_count--;
            }
            break;
        }
    }
    netins_udp_client_notify(client, NETINS_UDP_V2_EVENT_PATH_CHANGED);
}

static int
netins_udp_client_h3_conn_create(xqc_h3_conn_t *h3_conn,
    const xqc_cid_t *cid, void *h3c_user_data)
{
    (void)cid;
    netins_udp_client_t *client = h3c_user_data;
    pthread_mutex_lock(&client->lock);
    client->h3_conn = h3_conn;
    pthread_mutex_unlock(&client->lock);
    return XQC_OK;
}

static int
netins_udp_client_h3_conn_close(xqc_h3_conn_t *h3_conn,
    const xqc_cid_t *cid, void *h3c_user_data)
{
    (void)cid;
    netins_udp_client_t *client = h3c_user_data;
    int conn_error = h3_conn != NULL ? xqc_h3_conn_get_errno(h3_conn) : 0;
    if (conn_error == H3_NO_ERROR) {
        conn_error = 0;
    }

    pthread_mutex_lock(&client->lock);
    client->h3_conn = NULL;
    if (conn_error != 0 || client->state == NETINS_UDP_STATE_ERROR) {
        client->state = NETINS_UDP_STATE_ERROR;
        if (conn_error != 0) {
            netins_udp_client_set_error_locked(client, conn_error,
                "QUIC/H3 connection closed with an error");
        }
    } else {
        client->state = NETINS_UDP_STATE_CLOSED;
    }
    pthread_cond_broadcast(&client->state_cond);
    pthread_mutex_unlock(&client->lock);

    if (client->observer != NULL) {
        (void)netins_udp_backend_connection_refresh(client->observer);
        (void)netins_udp_backend_connection_closed(client->observer,
            conn_error);
    }
    netins_udp_client_notify(client, conn_error != 0
        ? NETINS_UDP_V2_EVENT_ERROR : NETINS_UDP_V2_EVENT_CLOSED);
    return XQC_OK;
}

static void
netins_udp_client_h3_handshake_finished(xqc_h3_conn_t *h3_conn,
    void *h3c_user_data)
{
    (void)h3_conn;
    netins_udp_client_t *client = h3c_user_data;
    pthread_mutex_lock(&client->lock);
    client->state = NETINS_UDP_STATE_CONNECTED;
    client->last_error_code = 0;
    client->last_error_msg[0] = '\0';
    pthread_cond_broadcast(&client->state_cond);
    pthread_mutex_unlock(&client->lock);

    if (client->observer != NULL) {
        (void)netins_udp_backend_connection_handshake_finished(
            client->observer);
        (void)netins_udp_backend_connection_refresh(client->observer);
    }
    netins_udp_client_notify(client, NETINS_UDP_V2_EVENT_CONNECTED);
}

static int
netins_udp_stream_h3_create(xqc_h3_request_t *request, void *user_data)
{
    netins_udp_stream_t *stream = user_data;
    if (stream == NULL) {
        return XQC_ERROR;
    }
    pthread_mutex_lock(&stream->lock);
    stream->request = request;
    stream->stream_id = (uint64_t)xqc_h3_stream_id(request);
    pthread_cond_broadcast(&stream->cond);
    pthread_mutex_unlock(&stream->lock);
    return XQC_OK;
}

static void
netins_udp_stream_drain_response(netins_udp_stream_t *stream,
    xqc_h3_request_t *request)
{
    while (1) {
        pthread_mutex_lock(&stream->lock);
        size_t available = stream->config.recv_queue_limit_bytes
            - stream->recv_queue_bytes;
        if (available == 0 || stream->recv_finished
            || stream->close_requested)
        {
            stream->read_pending = available == 0 && !stream->recv_finished;
            pthread_mutex_unlock(&stream->lock);
            return;
        }
        size_t capacity = available < NETINS_UDP_STREAM_RECV_CHUNK
            ? available : NETINS_UDP_STREAM_RECV_CHUNK;
        pthread_mutex_unlock(&stream->lock);

        netins_udp_buffer_t *buffer =
            netins_udp_stream_copy_buffer(NULL, capacity);
        if (buffer == NULL) {
            netins_udp_stream_mark_error(stream, ENOMEM);
            return;
        }

        uint8_t fin = 0;
        ssize_t received = xqc_h3_request_recv_body(request, buffer->data,
            capacity, &fin);
        if (received == -XQC_EAGAIN) {
            free(buffer);
            pthread_mutex_lock(&stream->lock);
            stream->read_pending = 0;
            pthread_mutex_unlock(&stream->lock);
            return;
        }
        if (received < 0) {
            free(buffer);
            netins_udp_stream_mark_transport_error(stream, (int)-received);
            return;
        }

        int protocol_error = 0;
        pthread_mutex_lock(&stream->lock);
        if (received > 0) {
            if (!stream->response_headers_received
                || netins_udp_stream_response_body_forbidden_locked(stream)
                || (stream->h3_extension != NULL
                && stream->h3_extension->response_content_length_known
                && (stream->bytes_received
                        > stream->response_content_length
                    || (uint64_t)received
                        > stream->response_content_length
                            - stream->bytes_received)))
            {
                free(buffer);
                protocol_error = 1;
            } else {
                buffer->length = (size_t)received;
                if (stream->recv_tail == NULL) {
                    stream->recv_head = buffer;
                } else {
                    stream->recv_tail->next = buffer;
                }
                stream->recv_tail = buffer;
                stream->recv_queue_bytes += (size_t)received;
                stream->bytes_received += (uint64_t)received;
            }
        } else {
            free(buffer);
        }
        if (!protocol_error && fin
            && (!stream->response_headers_received
                || (netins_udp_stream_response_body_forbidden_locked(stream)
                    && stream->bytes_received != 0)
                || (!netins_udp_stream_response_body_forbidden_locked(stream)
                    && stream->h3_extension != NULL
                    && stream->h3_extension->response_content_length_known
                    && stream->bytes_received
                        != stream->response_content_length)))
        {
            protocol_error = 1;
        } else if (!protocol_error && fin) {
            stream->recv_finished = 1;
            stream->read_pending = 0;
        }
        pthread_cond_broadcast(&stream->cond);
        pthread_mutex_unlock(&stream->lock);
        if (protocol_error) {
            netins_udp_stream_mark_error(stream, EPROTO);
            return;
        }
        if (received > 0 || fin) {
            netins_udp_stream_notify(stream,
                NETINS_UDP_V2_EVENT_READABLE);
        }

        if (fin || received == 0) {
            return;
        }
    }
}

static int
netins_udp_stream_h3_read(xqc_h3_request_t *request,
    xqc_request_notify_flag_t flag, void *user_data)
{
    netins_udp_stream_t *stream = user_data;
    if (stream == NULL) {
        return XQC_ERROR;
    }

    if (flag & (XQC_REQ_NOTIFY_READ_HEADER | XQC_REQ_NOTIFY_READ_TRAILER)) {
        uint8_t fin = 0;
        xqc_http_headers_t *headers =
            xqc_h3_request_recv_headers(request, &fin);
        if (headers == NULL) {
            netins_udp_stream_mark_error(stream, EPROTO);
            return XQC_OK;
        }
        if (flag & XQC_REQ_NOTIFY_READ_HEADER) {
            if (netins_udp_stream_parse_response_headers(stream, headers)
                != 0)
            {
                netins_udp_stream_mark_error(stream, EPROTO);
                return XQC_OK;
            }
        }
        if (fin) {
            /*
             * A response that ends in headers/trailers may not produce a body
             * callback. finish_response validates Content-Length and wakes the
             * request descriptor only after EOF is visible.
             */
            (void)netins_udp_stream_finish_response(stream);
        }
    }

    if (flag & XQC_REQ_NOTIFY_READ_BODY) {
        netins_udp_stream_drain_response(stream, request);
    }

    if (flag & XQC_REQ_NOTIFY_READ_EMPTY_FIN) {
        (void)netins_udp_stream_finish_response(stream);
    }
    return XQC_OK;
}

static int
netins_udp_stream_h3_write(xqc_h3_request_t *request, void *user_data)
{
    (void)request;
    netins_udp_stream_t *stream = user_data;
    if (stream == NULL) {
        return XQC_ERROR;
    }
    pthread_mutex_lock(&stream->lock);
    netins_udp_client_wake(stream->client);
    pthread_mutex_unlock(&stream->lock);
    netins_udp_stream_notify(stream, NETINS_UDP_V2_EVENT_WRITABLE);
    return XQC_OK;
}

static void
netins_udp_stream_h3_closing(xqc_h3_request_t *request, xqc_int_t error,
    void *user_data)
{
    (void)request;
    netins_udp_stream_t *stream = user_data;
    if (stream == NULL) {
        return;
    }
    pthread_mutex_lock(&stream->lock);
    if (error != 0) {
        if (stream->stream_error == 0) {
            stream->stream_error = (int)error;
            stream->stream_system_error =
                netins_udp_stream_transport_errno((int)error);
        }
        stream->state = NETINS_UDP_STREAM_STATE_ERROR;
    } else if (stream->state != NETINS_UDP_STREAM_STATE_ERROR) {
        stream->state = NETINS_UDP_STREAM_STATE_CLOSING;
    }
    pthread_cond_broadcast(&stream->cond);
    pthread_mutex_unlock(&stream->lock);
    if (error != 0) {
        netins_udp_stream_notify(stream, NETINS_UDP_V2_EVENT_ERROR);
    }
}

static int
netins_udp_stream_h3_close(xqc_h3_request_t *request, void *user_data)
{
    (void)request;
    netins_udp_stream_t *stream = user_data;
    if (stream == NULL) {
        return XQC_OK;
    }

    pthread_mutex_lock(&stream->lock);
    netins_udp_client_t *client = stream->client;
    int release_request_ref = stream->request_ref;
    stream->request_ref = 0;
    stream->request = NULL;
    stream->client = NULL;
    stream->close_notified = 1;
    stream->send_finished = 1;
    if (stream->stream_error != 0) {
        stream->state = NETINS_UDP_STREAM_STATE_ERROR;
    } else {
        stream->state = NETINS_UDP_STREAM_STATE_CLOSED;
    }
    uint32_t terminal_event = stream->stream_error != 0
        ? NETINS_UDP_V2_EVENT_ERROR : NETINS_UDP_V2_EVENT_CLOSED;
    pthread_cond_broadcast(&stream->cond);
    pthread_mutex_unlock(&stream->lock);

    netins_udp_stream_notify(stream, terminal_event);
    netins_udp_client_detach_stream(client, stream);
    if (release_request_ref) {
        netins_udp_stream_release(stream);
    }
    return XQC_OK;
}

static int
netins_udp_client_create_engine(netins_udp_client_t *client)
{
    xqc_config_t engine_config;
    if (xqc_engine_get_default_config(&engine_config, XQC_ENGINE_CLIENT)
        != XQC_OK)
    {
        netins_udp_client_set_error(client, EIO,
            "xqc_engine_get_default_config failed");
        return NETINS_UDP_ERROR;
    }

    xqc_engine_callback_t engine_callbacks;
    memset(&engine_callbacks, 0, sizeof(engine_callbacks));
    engine_callbacks.set_event_timer = netins_udp_client_set_event_timer;

    xqc_transport_callbacks_t transport_callbacks;
    memset(&transport_callbacks, 0, sizeof(transport_callbacks));
    transport_callbacks.write_socket = netins_udp_client_write_socket;
    transport_callbacks.write_socket_ex = netins_udp_client_write_socket_ex;
    transport_callbacks.save_token = netins_udp_client_ignore_token;
    transport_callbacks.save_session_cb = netins_udp_client_ignore_string;
    transport_callbacks.save_tp_cb = netins_udp_client_ignore_string;
    transport_callbacks.conn_update_cid_notify =
        netins_udp_client_update_cid;
    if (client->config.verify_certificate) {
        transport_callbacks.cert_verify_cb =
            netins_udp_client_verify_certificate;
    }
    transport_callbacks.ready_to_create_path_notify =
        netins_udp_client_ready_to_create_path;
    transport_callbacks.path_removed_notify = netins_udp_client_path_removed;

    xqc_engine_ssl_config_t ssl_config;
    memset(&ssl_config, 0, sizeof(ssl_config));
    ssl_config.ciphers = client->ciphers;
    ssl_config.groups = client->groups;

    client->engine = xqc_engine_create(XQC_ENGINE_CLIENT, &engine_config,
        &ssl_config, &engine_callbacks, &transport_callbacks, client);
    if (client->engine == NULL) {
        netins_udp_client_set_error(client, EIO,
            "xqc_engine_create failed");
        return NETINS_UDP_ERROR;
    }

    xqc_h3_callbacks_t h3_callbacks;
    memset(&h3_callbacks, 0, sizeof(h3_callbacks));
    h3_callbacks.h3c_cbs.h3_conn_create_notify =
        netins_udp_client_h3_conn_create;
    h3_callbacks.h3c_cbs.h3_conn_close_notify =
        netins_udp_client_h3_conn_close;
    h3_callbacks.h3c_cbs.h3_conn_handshake_finished =
        netins_udp_client_h3_handshake_finished;
    h3_callbacks.h3r_cbs.h3_request_create_notify =
        netins_udp_stream_h3_create;
    h3_callbacks.h3r_cbs.h3_request_close_notify =
        netins_udp_stream_h3_close;
    h3_callbacks.h3r_cbs.h3_request_read_notify =
        netins_udp_stream_h3_read;
    h3_callbacks.h3r_cbs.h3_request_write_notify =
        netins_udp_stream_h3_write;
    h3_callbacks.h3r_cbs.h3_request_closing_notify =
        netins_udp_stream_h3_closing;
    if (xqc_h3_ctx_init(client->engine, &h3_callbacks) != XQC_OK) {
        netins_udp_client_set_error(client, EIO,
            "xqc_h3_ctx_init failed");
        xqc_engine_destroy(client->engine);
        client->engine = NULL;
        return NETINS_UDP_ERROR;
    }
    return NETINS_UDP_OK;
}

static int
netins_udp_client_start_connection(netins_udp_client_t *client)
{
    xqc_conn_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    settings.cong_ctrl_callback = client->congestion_control;
    settings.cc_params.customize_on = 1;
    settings.cc_params.init_cwnd = client->config.initial_cwnd_packets;
    settings.proto_version = XQC_VERSION_V1;
    settings.idle_time_out = client->idle_timeout_ms;
    settings.spurious_loss_detect_on = 1;
    settings.scheduler_callback = client->scheduler;
    settings.scheduler_params.pto_cnt_thr =
        NETINS_UDP_CLIENT_PATH_FAILOVER_PTO;
    settings.reinj_ctl_callback = xqc_deadline_reinj_ctl_cb;
    settings.adaptive_ack_frequency = 1;
    settings.mp_ack_on_any_path = 0;
    settings.enable_multipath = client->config.enable_multipath;
    settings.init_recv_window = 64 * 1024 * 1024;
    settings.multipath_version = XQC_MULTIPATH_10;
    settings.recv_rate_bytes_per_sec = 0;
    settings.mp_ping_on = 1;
    settings.mp_enable_reinjection = 1;

    xqc_conn_ssl_config_t conn_ssl_config;
    memset(&conn_ssl_config, 0, sizeof(conn_ssl_config));
    if (client->config.verify_certificate) {
        conn_ssl_config.cert_verify_flag = XQC_TLS_CERT_FLAG_NEED_VERIFY;
        if (client->ca_store != NULL) {
            conn_ssl_config.cert_verify_flag |=
                XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED;
        }
    }

    const char *server_name = client->config.server_name[0] != '\0'
        ? client->config.server_name : client->config.server_address;
    const xqc_cid_t *cid = xqc_h3_connect(client->engine, &settings,
        NULL, 0, server_name, 0, &conn_ssl_config,
        (const struct sockaddr *)&client->paths[0].peer_addr,
        client->paths[0].peer_addrlen, client);
    if (cid == NULL) {
        netins_udp_client_set_error(client, ECONNREFUSED,
            "xqc_h3_connect failed");
        return NETINS_UDP_ERROR;
    }
    memcpy(&client->cid, cid, sizeof(client->cid));
    client->cid_valid = 1;
    return NETINS_UDP_OK;
}

static int
netins_udp_client_bind_observer(netins_udp_client_t *client)
{
    if (netins_udp_backend_connection_bind(client->observer, client->engine,
            &client->cid,
            NETINS_UDP_ROLE_CLIENT, 1) != NETINS_UDP_OK)
    {
        netins_udp_client_set_error(client, EIO,
            "failed to bind observer to the QUIC connection");
        return NETINS_UDP_ERROR;
    }
    netins_udp_client_update_path_metadata(client, &client->paths[0]);
    (void)netins_udp_backend_connection_refresh(client->observer);
    return NETINS_UDP_OK;
}

static int
netins_udp_client_try_create_path(netins_udp_client_t *client)
{
    if (!client->config.enable_multipath
        || client->config.interface_count <= 1
        || client->path_count >= client->config.interface_count
        || client->path_count >= NETINS_UDP_MAX_PATHS)
    {
        return NETINS_UDP_OK;
    }

    uint32_t slot = NETINS_UDP_MAX_PATHS;
    for (uint32_t i = 1;
         i < client->config.interface_count && i < NETINS_UDP_MAX_PATHS;
         ++i)
    {
        if (!client->paths[i].active) {
            slot = i;
            break;
        }
    }
    if (slot == NETINS_UDP_MAX_PATHS) {
        return NETINS_UDP_OK;
    }
    netins_udp_client_path_t *path = &client->paths[slot];
    if (netins_udp_client_create_socket(client, path,
            client->config.interface_names[slot],
            &client->paths[0].peer_addr,
            client->paths[0].peer_addrlen) != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }

    uint64_t path_id = UINT64_MAX;
    int ret = xqc_conn_create_path(client->engine, &client->cid, &path_id, 0);
    if (ret != XQC_OK) {
        netins_udp_client_close_path(path);
        return ret;
    }

    path->path_id = path_id;
    path->active = 1;
    client->path_count++;
    netins_udp_client_update_path_metadata(client, path);
    netins_udp_client_notify(client, NETINS_UDP_V2_EVENT_PATH_CHANGED);
    return NETINS_UDP_OK;
}

static void
netins_udp_client_maybe_create_paths(netins_udp_client_t *client,
    uint64_t now_us)
{
    pthread_mutex_lock(&client->lock);
    int ready = client->path_creation_ready;
    pthread_mutex_unlock(&client->lock);
    if (!ready || !client->cid_valid
        || now_us - client->last_path_attempt_us
            < NETINS_UDP_CLIENT_PATH_RETRY_US)
    {
        return;
    }
    client->last_path_attempt_us = now_us;
    (void)netins_udp_client_try_create_path(client);
}

static int
netins_udp_client_recv_path(netins_udp_client_t *client,
    netins_udp_client_path_t *path)
{
    if (path == NULL || path->fd < 0 || !path->active
        || path->rx_batch == NULL)
    {
        return 0;
    }
    int processed = 0;
    uint32_t packet_limit = netins_udp_backend_rx_packet_limit(
        &client->rx_policy, client->path_count);
    if (packet_limit > NETINS_UDP_BACKEND_RX_BATCH_CAPACITY) {
        packet_limit = NETINS_UDP_BACKEND_RX_BATCH_CAPACITY;
    }
    netins_udp_client_rx_batch_t *batch = path->rx_batch;

#if defined(__linux__)
    for (uint32_t i = 0; i < packet_limit; ++i) {
        memset(&batch->messages[i].msg_hdr, 0,
            sizeof(batch->messages[i].msg_hdr));
        batch->messages[i].msg_hdr.msg_iov = &batch->iovecs[i];
        batch->messages[i].msg_hdr.msg_iovlen = 1;
        batch->messages[i].msg_len = 0;
    }
    int received;
    do {
        received = recvmmsg(path->fd, batch->messages, packet_limit,
            MSG_DONTWAIT, NULL);
    } while (received < 0 && errno == EINTR);
    if (received < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        int error_code = errno;
        return netins_udp_client_handle_path_error(client, path,
            error_code, "recvmmsg");
    }

    uint64_t receive_time_us = netins_udp_client_packet_time_us();
    for (int i = 0; i < received; ++i) {
        size_t size = batch->messages[i].msg_len;
        if (size == 0) {
            continue;
        }
        (void)xqc_engine_packet_process(client->engine,
            batch->storage + (size_t)i * batch->slot_capacity, size,
            (const struct sockaddr *)&path->local_addr,
            path->local_addrlen,
            (const struct sockaddr *)&path->peer_addr,
            path->peer_addrlen, receive_time_us, client);
        processed++;
        if (!path->active) {
            break;
        }
    }
#else
    for (uint32_t i = 0; i < packet_limit; ++i) {
        unsigned char *packet = batch->storage
            + (size_t)i * batch->slot_capacity;
        ssize_t size = recvfrom(path->fd, packet, batch->slot_capacity,
            0, NULL, NULL);
        if (size < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            int error_code = errno;
            return netins_udp_client_handle_path_error(client, path,
                error_code, "recvfrom");
        }
        if (size == 0) {
            break;
        }
        (void)xqc_engine_packet_process(client->engine,
            packet, (size_t)size,
            (const struct sockaddr *)&path->local_addr,
            path->local_addrlen,
            (const struct sockaddr *)&path->peer_addr,
            path->peer_addrlen,
            netins_udp_client_packet_time_us(), client);
        processed++;
        if (!path->active) {
            break;
        }
    }
#endif
    return processed;
}

static void
netins_udp_stream_set_header(xqc_http_header_t *header, const char *name,
    const char *value)
{
    memset(header, 0, sizeof(*header));
    header->name.iov_base = (void *)name;
    header->name.iov_len = strlen(name);
    header->value.iov_base = (void *)value;
    header->value.iov_len = strlen(value);
}

static int
netins_udp_stream_has_request_header(const netins_udp_stream_t *stream,
    const char *name)
{
    if (stream->h3_extension == NULL) {
        return 0;
    }
    for (uint32_t i = 0;
         i < stream->h3_extension->request_header_count; ++i)
    {
        if (strcasecmp(
                stream->h3_extension->request_headers[i].name,
                name) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int
netins_udp_stream_send_headers(netins_udp_stream_t *stream,
    xqc_h3_request_t *request)
{
    size_t extension_header_count = stream->h3_extension != NULL
        ? stream->h3_extension->request_header_count : 0;
    xqc_http_header_t fields[10 + extension_header_count];
    size_t count = 0;
    char content_length[32];
    char stream_index[8];
    char stream_count[8];
    char stream_offset[32];
    char total_length[32];

    netins_udp_stream_set_header(&fields[count++], ":method",
        stream->config.method);
    netins_udp_stream_set_header(&fields[count++], ":scheme",
        stream->config.scheme);
    netins_udp_stream_set_header(&fields[count++], ":authority",
        stream->config.authority);
    netins_udp_stream_set_header(&fields[count++], ":path",
        stream->config.path);
    netins_udp_stream_set_header(&fields[count++], "content-type",
        stream->config.content_type);
    int request_body_length_unknown =
        stream->h3_extension != NULL
        && stream->h3_extension->request_body_length_unknown;
    if (!request_body_length_unknown) {
        snprintf(content_length, sizeof(content_length), "%llu",
            (unsigned long long)stream->config.request_body_length);
        netins_udp_stream_set_header(&fields[count++], "content-length",
            content_length);
    }

    int is_post = strcasecmp(stream->config.method, "POST") == 0;
    /*
     * x-stream-* belongs to the historical mini transfer protocol, not to the
     * generic public HTTP/3 API. Public callers can still add those fields
     * explicitly when interoperating with a legacy peer.
     */
    if (is_post && stream->h3_extension == NULL) {
        snprintf(stream_index, sizeof(stream_index), "0");
        snprintf(stream_count, sizeof(stream_count), "1");
        snprintf(stream_offset, sizeof(stream_offset), "0");
        snprintf(total_length, sizeof(total_length), "%llu",
            (unsigned long long)stream->config.request_body_length);
        if (!netins_udp_stream_has_request_header(stream,
                "x-stream-index"))
        {
            netins_udp_stream_set_header(&fields[count++],
                "x-stream-index", stream_index);
        }
        if (!netins_udp_stream_has_request_header(stream,
                "x-stream-count"))
        {
            netins_udp_stream_set_header(&fields[count++],
                "x-stream-count", stream_count);
        }
        if (!netins_udp_stream_has_request_header(stream,
                "x-stream-offset"))
        {
            netins_udp_stream_set_header(&fields[count++],
                "x-stream-offset", stream_offset);
        }
        if (!request_body_length_unknown
            && !netins_udp_stream_has_request_header(stream,
                "x-total-length"))
        {
            netins_udp_stream_set_header(&fields[count++],
                "x-total-length", total_length);
        }
    }
    if (stream->h3_extension != NULL) {
        for (uint32_t i = 0;
             i < stream->h3_extension->request_header_count; ++i)
        {
            netins_udp_stream_set_header(&fields[count++],
                stream->h3_extension->request_headers[i].name,
                stream->h3_extension->request_headers[i].value);
        }
    }

    xqc_http_headers_t headers;
    memset(&headers, 0, sizeof(headers));
    headers.headers = fields;
    headers.count = count;
    headers.capacity = count;

    uint8_t fin = !is_post
        || (!request_body_length_unknown
            && stream->config.request_body_length == 0);
    ssize_t sent = xqc_h3_request_send_headers(request, &headers, fin);
    if (sent == -XQC_EAGAIN) {
        return 0;
    }
    if (sent < 0) {
        netins_udp_stream_mark_transport_error(stream, (int)-sent);
        return -1;
    }

    pthread_mutex_lock(&stream->lock);
    stream->request_headers_sent = 1;
    stream->state = NETINS_UDP_STREAM_STATE_OPEN;
    if (fin) {
        stream->send_finished = 1;
        stream->send_fin_requested = 1;
    }
    pthread_cond_broadcast(&stream->cond);
    pthread_mutex_unlock(&stream->lock);
    return 1;
}

static int
netins_udp_stream_drive_send(netins_udp_stream_t *stream,
    xqc_h3_request_t *request)
{
    int activity = 0;
    while (1) {
        pthread_mutex_lock(&stream->lock);
        if (stream->send_finished || stream->close_requested) {
            pthread_mutex_unlock(&stream->lock);
            return activity;
        }
        netins_udp_buffer_t *buffer = stream->send_head;
        int finish_only = buffer == NULL && stream->send_fin_requested;
        if (buffer == NULL && !finish_only) {
            pthread_mutex_unlock(&stream->lock);
            return activity;
        }
        size_t remaining = buffer != NULL
            ? buffer->length - buffer->offset : 0;
        int fin = buffer != NULL && stream->send_fin_requested
            && buffer == stream->send_tail;
        unsigned char *data = buffer != NULL
            ? buffer->data + buffer->offset : NULL;
        pthread_mutex_unlock(&stream->lock);

        ssize_t sent = finish_only
            ? xqc_h3_request_finish(request)
            : xqc_h3_request_send_body(request, data, remaining,
                (uint8_t)fin);
        if (sent == -XQC_EAGAIN) {
            return activity;
        }
        if (sent < 0) {
            netins_udp_stream_mark_transport_error(stream, (int)-sent);
            return activity;
        }

        pthread_mutex_lock(&stream->lock);
        if (finish_only) {
            stream->send_finished = 1;

        } else if (sent > 0) {
            buffer->offset += (size_t)sent;
            stream->send_queue_bytes -= (size_t)sent;
            stream->bytes_sent += (uint64_t)sent;
            if (buffer->offset == buffer->length) {
                stream->send_head = buffer->next;
                if (stream->send_head == NULL) {
                    stream->send_tail = NULL;
                }
                free(buffer);
            }
            if (fin && stream->send_head == NULL) {
                stream->send_finished = 1;
            }

        } else {
            stream->send_finished = 1;
        }
        int finished = stream->send_finished;
        pthread_cond_broadcast(&stream->cond);
        pthread_mutex_unlock(&stream->lock);
        netins_udp_stream_notify(stream, NETINS_UDP_V2_EVENT_WRITABLE);
        activity = 1;

        if (finish_only || sent == 0 || (fin && finished)) {
            return activity;
        }
    }
}

static void
netins_udp_stream_complete_without_request(netins_udp_stream_t *stream,
    int error_code)
{
    pthread_mutex_lock(&stream->lock);
    netins_udp_client_t *client = stream->client;
    stream->client = NULL;
    stream->close_notified = 1;
    if (error_code != 0 && stream->stream_error == 0) {
        stream->stream_error = error_code;
        stream->stream_system_error = error_code;
    }
    stream->state = error_code == 0
            && stream->stream_error == 0
        ? NETINS_UDP_STREAM_STATE_CLOSED : NETINS_UDP_STREAM_STATE_ERROR;
    pthread_cond_broadcast(&stream->cond);
    pthread_mutex_unlock(&stream->lock);
    netins_udp_stream_notify(stream, error_code == 0
        ? NETINS_UDP_V2_EVENT_CLOSED : NETINS_UDP_V2_EVENT_ERROR);
    netins_udp_client_detach_stream(client, stream);
}

static int
netins_udp_client_process_stream(netins_udp_client_t *client,
    netins_udp_stream_t *stream)
{
    pthread_mutex_lock(&stream->lock);
    int close_requested = stream->close_requested;
    int request_headers_sent = stream->request_headers_sent;
    int read_pending = stream->read_pending;
    xqc_h3_request_t *request = stream->request;
    int terminal = stream->state == NETINS_UDP_STREAM_STATE_CLOSED
        || stream->state == NETINS_UDP_STREAM_STATE_ERROR;
    pthread_mutex_unlock(&stream->lock);

    if (terminal && request == NULL) {
        return 0;
    }
    if (terminal && request != NULL) {
        pthread_mutex_lock(&stream->lock);
        int should_close = !stream->close_sent;
        stream->close_sent = 1;
        pthread_mutex_unlock(&stream->lock);
        if (should_close) {
            (void)xqc_h3_request_close(request);
            return 1;
        }
        return 0;
    }
    if (close_requested && request == NULL) {
        netins_udp_stream_complete_without_request(stream, 0);
        return 0;
    }

    int activity = 0;
    if (request == NULL) {
        pthread_mutex_lock(&stream->lock);
        if (!stream->request_ref) {
            stream->request_ref = 1;
            stream->ref_count++;
        }
        pthread_mutex_unlock(&stream->lock);

        xqc_stream_settings_t settings;
        memset(&settings, 0, sizeof(settings));
        settings.recv_rate_bytes_per_sec = 0;
        request = xqc_h3_request_create(client->engine, &client->cid,
            &settings, stream);
        if (request == NULL) {
            pthread_mutex_lock(&stream->lock);
            int release_request_ref = stream->request_ref;
            stream->request_ref = 0;
            pthread_mutex_unlock(&stream->lock);
            netins_udp_stream_mark_error(stream, EIO);
            netins_udp_stream_complete_without_request(stream, EIO);
            if (release_request_ref) {
                netins_udp_stream_release(stream);
            }
            return 0;
        }
        pthread_mutex_lock(&stream->lock);
        stream->request = request;
        stream->stream_id = (uint64_t)xqc_h3_stream_id(request);
        pthread_mutex_unlock(&stream->lock);
        activity = 1;
    }

    if (!request_headers_sent) {
        int header_result = netins_udp_stream_send_headers(stream, request);
        if (header_result < 0) {
            pthread_mutex_lock(&stream->lock);
            stream->close_sent = 1;
            pthread_mutex_unlock(&stream->lock);
            (void)xqc_h3_request_close(request);
            return 1;
        }
        if (header_result == 0) {
            return activity;
        }
        activity = 1;
    }

    pthread_mutex_lock(&stream->lock);
    read_pending = stream->read_pending;
    close_requested = stream->close_requested;
    int close_sent = stream->close_sent;
    pthread_mutex_unlock(&stream->lock);

    activity |= netins_udp_stream_drive_send(stream, request);
    if (read_pending) {
        netins_udp_stream_drain_response(stream, request);
    }

    if (close_requested && !close_sent) {
        pthread_mutex_lock(&stream->lock);
        stream->close_sent = 1;
        if (stream->state != NETINS_UDP_STREAM_STATE_ERROR) {
            stream->state = NETINS_UDP_STREAM_STATE_CLOSING;
        }
        pthread_mutex_unlock(&stream->lock);
        int ret = xqc_h3_request_close(request);
        if (ret != XQC_OK) {
            netins_udp_stream_mark_transport_error(stream,
                ret < 0 ? -ret : ret);
        }
        activity = 1;
    }
    return activity;
}

static void
netins_udp_client_process_streams(netins_udp_client_t *client)
{
    netins_udp_stream_t *streams[NETINS_UDP_MAX_STREAMS];
    pthread_mutex_lock(&client->lock);
    size_t copied = 0;
    for (netins_udp_stream_t *item = client->streams;
         item != NULL && copied < NETINS_UDP_MAX_STREAMS;
         item = item->next)
    {
        netins_udp_stream_retain(item);
        streams[copied++] = item;
    }
    pthread_mutex_unlock(&client->lock);

    int activity = 0;
    for (size_t i = 0; i < copied; ++i) {
        activity |= netins_udp_client_process_stream(client, streams[i]);
        netins_udp_stream_release(streams[i]);
    }
    if (activity) {
        xqc_engine_main_logic(client->engine);
    }
}

static int
netins_udp_client_poll_timeout_ms(netins_udp_client_t *client,
    uint64_t now_us)
{
    int timeout_ms = 500;
    pthread_mutex_lock(&client->lock);
    uint64_t wake_us = client->next_wake_us;
    uint64_t connect_deadline_us =
        client->state == NETINS_UDP_STATE_CONNECTING
            ? client->connect_deadline_us : 0;
    pthread_mutex_unlock(&client->lock);

    if (wake_us != 0) {
        if (wake_us <= now_us) {
            return 0;
        }
        uint64_t remaining_us = wake_us - now_us;
        int timer_ms = (int)((remaining_us + 999ULL) / 1000ULL);
        if (timer_ms < timeout_ms) {
            timeout_ms = timer_ms;
        }
    }
    if (connect_deadline_us != 0) {
        if (connect_deadline_us <= now_us) {
            return 0;
        }
        uint64_t remaining_us = connect_deadline_us - now_us;
        int timer_ms = (int)((remaining_us + 999ULL) / 1000ULL);
        if (timer_ms < timeout_ms) {
            timeout_ms = timer_ms;
        }
    }
    return timeout_ms;
}

static void
netins_udp_client_streams_send_drained_locked(
    netins_udp_client_t *client, int *out_drained)
{
    int drained = 1;
    for (netins_udp_stream_t *stream = client->streams;
         stream != NULL; stream = stream->next)
    {
        pthread_mutex_lock(&stream->lock);
        int terminal = stream->stream_error != 0
            || stream->state == NETINS_UDP_STREAM_STATE_ERROR
            || stream->state == NETINS_UDP_STREAM_STATE_CLOSED
            || stream->close_notified;
        int stream_drained = terminal
            || (stream->send_queue_bytes == 0
                && (!stream->send_fin_requested
                    || stream->send_finished));
        pthread_mutex_unlock(&stream->lock);
        if (!stream_drained) {
            drained = 0;
            break;
        }
    }
    *out_drained = drained;
}

static void
netins_udp_client_drive_close(netins_udp_client_t *client)
{
    pthread_mutex_lock(&client->lock);
    uint64_t application_error = client->close_application_error;
    int streams_drained = 1;
    if (client->close_requested && !client->close_sent
        && application_error == 0)
    {
        netins_udp_client_streams_send_drained_locked(client,
            &streams_drained);
    }
    int should_close = client->close_requested && !client->close_sent
        && (application_error != 0 || streams_drained);
    xqc_h3_conn_t *h3_conn = client->h3_conn;
    if (should_close) {
        client->close_sent = 1;
        if (client->state == NETINS_UDP_STATE_CONNECTED
            || client->state == NETINS_UDP_STATE_CONNECTING)
        {
            client->state = NETINS_UDP_STATE_CLOSING;
        }
        pthread_cond_broadcast(&client->state_cond);
    }
    pthread_mutex_unlock(&client->lock);

    if (should_close && client->engine != NULL && client->cid_valid) {
        if (client->observer != NULL) {
            (void)netins_udp_backend_connection_closing(client->observer);
        }
        if (application_error != 0 && h3_conn != NULL) {
            (void)xqc_conn_close_with_error(
                xqc_h3_conn_get_xqc_conn(h3_conn), application_error);
        } else {
            (void)xqc_h3_conn_close(client->engine, &client->cid);
        }
        xqc_engine_main_logic(client->engine);
    }
}

static void
netins_udp_client_run_engine_timer(netins_udp_client_t *client,
    uint64_t now_us)
{
    pthread_mutex_lock(&client->lock);
    int timer_due = client->next_wake_us != 0
        && client->next_wake_us <= now_us;
    if (timer_due) {
        client->next_wake_us = 0;
    }
    pthread_mutex_unlock(&client->lock);
    if (timer_due) {
        xqc_engine_main_logic(client->engine);
    }
}

static void
netins_udp_client_refresh_observer(netins_udp_client_t *client, uint64_t now_us)
{
    if (client->observer == NULL
        || now_us - client->last_refresh_us < NETINS_UDP_CLIENT_REFRESH_US)
    {
        return;
    }
    for (uint32_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        netins_udp_client_update_path_metadata(client, &client->paths[i]);
    }
    (void)netins_udp_backend_connection_refresh(client->observer);
    client->last_refresh_us = now_us;
}

static int
netins_udp_client_check_startup_abort(netins_udp_client_t *client)
{
    uint64_t now_us = netins_udp_client_now_us();
    pthread_mutex_lock(&client->lock);
    int canceled = client->stop_requested || client->close_requested;
    int timed_out = !canceled
        && client->state == NETINS_UDP_STATE_CONNECTING
        && client->connect_deadline_us != 0
        && now_us >= client->connect_deadline_us;
    if (canceled && client->state != NETINS_UDP_STATE_ERROR) {
        client->state = NETINS_UDP_STATE_CLOSED;
        pthread_cond_broadcast(&client->state_cond);
    }
    pthread_mutex_unlock(&client->lock);

    if (canceled) {
        netins_udp_client_notify(client, NETINS_UDP_V2_EVENT_CLOSED);
        return 1;
    }
    if (timed_out) {
        netins_udp_client_fail_runtime(client, ETIMEDOUT,
            "timed out establishing QUIC/H3 connection");
        return 1;
    }
    return 0;
}

static int
netins_udp_client_check_connect_timeout(netins_udp_client_t *client)
{
    uint64_t now_us = netins_udp_client_now_us();
    pthread_mutex_lock(&client->lock);
    int timed_out = client->state == NETINS_UDP_STATE_CONNECTING
        && client->connect_deadline_us != 0
        && now_us >= client->connect_deadline_us;
    pthread_mutex_unlock(&client->lock);
    if (!timed_out) {
        return 0;
    }
    netins_udp_client_fail_runtime(client, ETIMEDOUT,
        "timed out establishing QUIC/H3 connection");
    return 1;
}

static int
netins_udp_client_bootstrap(netins_udp_client_t *client)
{
    struct sockaddr_storage peer_addr;
    socklen_t peer_addrlen = 0;
    if (netins_udp_client_resolve_peer(client, &peer_addr, &peer_addrlen)
        != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }
    if (netins_udp_client_check_startup_abort(client)) {
        return NETINS_UDP_ERROR;
    }

    const char *initial_interface = client->config.interface_count > 0
        ? client->config.interface_names[0] : NULL;
    if (netins_udp_client_create_socket(client, &client->paths[0],
            initial_interface, &peer_addr, peer_addrlen) != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }
    client->paths[0].path_id = XQC_INITIAL_PATH_ID;
    client->paths[0].active = 1;
    client->path_count = 1;
    if (netins_udp_client_check_startup_abort(client)) {
        return NETINS_UDP_ERROR;
    }

    if (netins_udp_client_create_engine(client) != NETINS_UDP_OK
        || netins_udp_client_check_startup_abort(client)
        || netins_udp_client_start_connection(client) != NETINS_UDP_OK
        || netins_udp_client_check_startup_abort(client)
        || netins_udp_client_bind_observer(client) != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }
    return NETINS_UDP_OK;
}

static void *
netins_udp_client_thread_main(void *argument)
{
    netins_udp_client_t *client = argument;
    if (netins_udp_client_bootstrap(client) != NETINS_UDP_OK) {
        pthread_mutex_lock(&client->lock);
        int terminal_state = client->state == NETINS_UDP_STATE_CLOSED
            || client->state == NETINS_UDP_STATE_ERROR;
        int error_code = client->last_error_code != 0
            ? client->last_error_code : EIO;
        char message[NETINS_UDP_ERROR_MSG_LEN];
        snprintf(message, sizeof(message), "%s",
            client->last_error_msg[0] != '\0'
                ? client->last_error_msg
                : "failed to start QUIC/H3 connection");
        pthread_mutex_unlock(&client->lock);
        if (!terminal_state) {
            netins_udp_client_fail_runtime(client, error_code, message);
        }
        return NULL;
    }

    while (1) {
        if (netins_udp_client_check_connect_timeout(client)) {
            break;
        }
        pthread_mutex_lock(&client->lock);
        int stop_requested = client->stop_requested;
        int terminal_state = client->state == NETINS_UDP_STATE_CLOSED
            || client->state == NETINS_UDP_STATE_ERROR;
        pthread_mutex_unlock(&client->lock);
        if (stop_requested || terminal_state) {
            break;
        }

        netins_udp_client_drive_close(client);
        uint64_t now_us = netins_udp_client_now_us();
        int timeout_ms = netins_udp_client_poll_timeout_ms(client, now_us);

        struct pollfd poll_fds[NETINS_UDP_MAX_PATHS + 1];
        int path_indexes[NETINS_UDP_MAX_PATHS + 1];
        nfds_t poll_count = 0;
        poll_fds[poll_count].fd = client->wake_pipe[0];
        poll_fds[poll_count].events = POLLIN;
        poll_fds[poll_count].revents = 0;
        path_indexes[poll_count] = -1;
        poll_count++;
        size_t poll_path_start = client->rx_next_path_start;
        for (uint32_t offset = 0; offset < NETINS_UDP_MAX_PATHS; ++offset) {
            uint32_t i = (uint32_t)((poll_path_start + offset)
                % NETINS_UDP_MAX_PATHS);
            netins_udp_client_path_t *path = &client->paths[i];
            if (!path->active || path->fd < 0) {
                continue;
            }
            poll_fds[poll_count].fd = path->fd;
            poll_fds[poll_count].events = POLLIN
                | (path->write_blocked ? POLLOUT : 0);
            poll_fds[poll_count].revents = 0;
            path_indexes[poll_count] = (int)i;
            poll_count++;
        }

        int poll_ret;
        do {
            poll_ret = poll(poll_fds, poll_count, timeout_ms);
        } while (poll_ret < 0 && errno == EINTR);
        if (poll_ret < 0) {
            int error_code = errno;
            char message[NETINS_UDP_ERROR_MSG_LEN];
            snprintf(message, sizeof(message), "poll: %s",
                strerror(error_code));
            netins_udp_client_fail_runtime(client, error_code, message);
            break;
        }

        if (poll_fds[0].revents & POLLIN) {
            netins_udp_client_drain_wake_pipe(client);
        }

        int received_packets = 0;
        int continue_send = 0;
        int runtime_failed = 0;
        for (nfds_t i = 1; i < poll_count; ++i) {
            int path_index = path_indexes[i];
            netins_udp_client_path_t *path = &client->paths[path_index];
            if (poll_fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                int socket_error = 0;
                socklen_t error_len = sizeof(socket_error);
                if (getsockopt(path->fd, SOL_SOCKET, SO_ERROR,
                        &socket_error, &error_len) != 0
                    || socket_error == 0)
                {
                    socket_error = EIO;
                }
                if (netins_udp_client_handle_path_error(client, path,
                        socket_error, "UDP path poll") != NETINS_UDP_OK)
                {
                    runtime_failed = 1;
                    break;
                }
                continue;
            }
            if (poll_fds[i].revents & POLLIN) {
                int path_packets = netins_udp_client_recv_path(client, path);
                if (path_packets < 0) {
                    runtime_failed = 1;
                    break;
                }
                received_packets += path_packets;
            }
            if (poll_fds[i].revents & POLLOUT) {
                path->write_blocked = 0;
                continue_send = 1;
            }
        }
        if (runtime_failed) {
            break;
        }
        if (received_packets > 0) {
            xqc_engine_finish_recv(client->engine);
        }
        client->rx_next_path_start = netins_udp_backend_next_path_start(
            poll_path_start, NETINS_UDP_MAX_PATHS);
        if (continue_send && client->cid_valid) {
            (void)xqc_conn_continue_send(client->engine, &client->cid);
        }

        netins_udp_client_process_streams(client);

        now_us = netins_udp_client_now_us();
        netins_udp_client_run_engine_timer(client, now_us);
        netins_udp_client_maybe_create_paths(client, now_us);
        netins_udp_client_refresh_observer(client, now_us);
    }
    return NULL;
}

static int
netins_udp_client_make_deadline(uint32_t timeout_ms, struct timespec *deadline)
{
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        return NETINS_UDP_ERROR;
    }
    deadline->tv_sec += timeout_ms / 1000U;
    long extra_ns = (long)(timeout_ms % 1000U) * 1000000L;
    deadline->tv_nsec += extra_ns;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return NETINS_UDP_OK;
}

static void
netins_udp_client_detach_all_streams(netins_udp_client_t *client,
    int error_code)
{
    pthread_mutex_lock(&client->lock);
    netins_udp_stream_t *streams = client->streams;
    client->streams = NULL;
    client->stream_count = 0;
    client->reserved_send_queue_bytes = 0;
    client->reserved_recv_queue_bytes = 0;
    pthread_mutex_unlock(&client->lock);

    while (streams != NULL) {
        netins_udp_stream_t *stream = streams;
        streams = stream->next;
        stream->next = NULL;

        pthread_mutex_lock(&stream->lock);
        int release_request_ref = stream->request_ref;
        stream->request_ref = 0;
        stream->request = NULL;
        stream->client = NULL;
        stream->budget_reserved = 0;
        stream->close_notified = 1;
        if (stream->state != NETINS_UDP_STREAM_STATE_CLOSED) {
            if (stream->stream_error == 0) {
                stream->stream_error = error_code;
                stream->stream_system_error = error_code > 0
                        && error_code < H3_NO_ERROR
                    ? error_code
                    : (error_code != 0 ? ECONNABORTED : 0);
            }
            stream->state = error_code == 0
                ? NETINS_UDP_STREAM_STATE_CLOSED
                : NETINS_UDP_STREAM_STATE_ERROR;
        }
        uint32_t terminal_event =
            stream->state == NETINS_UDP_STREAM_STATE_ERROR
                ? NETINS_UDP_V2_EVENT_ERROR
                : NETINS_UDP_V2_EVENT_CLOSED;
        pthread_cond_broadcast(&stream->cond);
        pthread_mutex_unlock(&stream->lock);
        netins_udp_stream_notify(stream, terminal_event);

        if (release_request_ref) {
            netins_udp_stream_release(stream);
        }
        netins_udp_stream_release(stream);
    }
}

void
netins_udp_stream_config_init(netins_udp_stream_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->api_version = NETINS_UDP_STREAM_API_VERSION;
    snprintf(config->method, sizeof(config->method), "GET");
    snprintf(config->scheme, sizeof(config->scheme), "https");
    snprintf(config->path, sizeof(config->path), "/");
    snprintf(config->content_type, sizeof(config->content_type),
        "application/octet-stream");
    config->send_queue_limit_bytes = NETINS_UDP_STREAM_DEFAULT_SEND_QUEUE;
    config->recv_queue_limit_bytes = NETINS_UDP_STREAM_DEFAULT_RECV_QUEUE;
}

static int
netins_udp_client_format_api_authority(const netins_udp_client_t *client,
    char *authority, size_t capacity)
{
    const char *host = client->config.server_name[0] != '\0'
        ? client->config.server_name : client->config.server_address;
    size_t host_length = strlen(host);
    int bracketed = host_length >= 2 && host[0] == '['
        && host[host_length - 1] == ']';
    int ipv6_literal = !bracketed && strchr(host, ':') != NULL;
    int include_port = client->config.server_port != 443;

    int written;
    if (ipv6_literal && include_port) {
        written = snprintf(authority, capacity, "[%s]:%u", host,
            (unsigned)client->config.server_port);
    } else if (ipv6_literal) {
        written = snprintf(authority, capacity, "[%s]", host);
    } else if (include_port) {
        written = snprintf(authority, capacity, "%s:%u", host,
            (unsigned)client->config.server_port);
    } else {
        written = snprintf(authority, capacity, "%s", host);
    }
    if (written < 0 || (size_t)written >= capacity) {
        errno = ENAMETOOLONG;
        return NETINS_UDP_ERROR;
    }
    return NETINS_UDP_OK;
}

static int
netins_udp_stream_open_internal(netins_udp_client_t *client,
    const netins_udp_stream_config_t *config,
    const netins_udp_client_h3_config_t *extended_config,
    netins_udp_stream_t **out_stream)
{
    if (client == NULL || config == NULL || out_stream == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    *out_stream = NULL;
    if (config->struct_size != sizeof(*config)
        || config->api_version != NETINS_UDP_STREAM_API_VERSION)
    {
        errno = EPROTO;
        return NETINS_UDP_VERSION_MISMATCH;
    }
    if (memchr(config->method, '\0', sizeof(config->method)) == NULL
        || memchr(config->scheme, '\0', sizeof(config->scheme)) == NULL
        || memchr(config->path, '\0', sizeof(config->path)) == NULL
        || memchr(config->authority, '\0', sizeof(config->authority)) == NULL
        || memchr(config->content_type, '\0',
               sizeof(config->content_type)) == NULL
        || config->send_queue_limit_bytes > (size_t)INT64_MAX
        || config->recv_queue_limit_bytes > (size_t)INT64_MAX)
    {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    int is_get = strcasecmp(config->method, "GET") == 0;
    int is_post = strcasecmp(config->method, "POST") == 0;
    if ((!is_get && !is_post) || config->path[0] != '/'
        || (is_get && config->request_body_length != 0))
    {
        errno = !is_get && !is_post ? EOPNOTSUPP : EINVAL;
        return NETINS_UDP_ERROR;
    }

    netins_udp_stream_t *stream = calloc(1, sizeof(*stream));
    if (stream == NULL) {
        errno = ENOMEM;
        return NETINS_UDP_ERROR;
    }
    int init_error = pthread_mutex_init(&stream->lock, NULL);
    if (init_error != 0) {
        free(stream);
        errno = init_error;
        return NETINS_UDP_ERROR;
    }
    init_error = pthread_cond_init(&stream->cond, NULL);
    if (init_error != 0) {
        pthread_mutex_destroy(&stream->lock);
        free(stream);
        errno = init_error;
        return NETINS_UDP_ERROR;
    }

    stream->config = *config;
    stream->config.method[NETINS_UDP_METHOD_LEN - 1] = '\0';
    stream->config.scheme[NETINS_UDP_SCHEME_LEN - 1] = '\0';
    stream->config.path[NETINS_UDP_REQUEST_PATH_LEN - 1] = '\0';
    stream->config.authority[NETINS_UDP_SERVER_NAME_LEN - 1] = '\0';
    stream->config.content_type[NETINS_UDP_CONTENT_TYPE_LEN - 1] = '\0';
    snprintf(stream->config.method, sizeof(stream->config.method), "%s",
        is_get ? "GET" : "POST");
    if (stream->config.scheme[0] == '\0') {
        snprintf(stream->config.scheme, sizeof(stream->config.scheme),
            "https");
    }
    if (stream->config.content_type[0] == '\0') {
        snprintf(stream->config.content_type,
            sizeof(stream->config.content_type),
            "application/octet-stream");
    }
    if (stream->config.authority[0] == '\0' && extended_config != NULL) {
        if (netins_udp_client_format_api_authority(client,
                stream->config.authority,
                sizeof(stream->config.authority)) != NETINS_UDP_OK)
        {
            pthread_cond_destroy(&stream->cond);
            pthread_mutex_destroy(&stream->lock);
            free(stream);
            return NETINS_UDP_ERROR;
        }
    } else if (stream->config.authority[0] == '\0') {
        /*
         * Preserve the compatibility stream contract. Only the final public
         * API gets RFC authority formatting with a non-default port.
         */
        const char *authority = client->config.server_name[0] != '\0'
            ? client->config.server_name : client->config.server_address;
        snprintf(stream->config.authority,
            sizeof(stream->config.authority), "%s", authority);
    }
    if (stream->config.send_queue_limit_bytes == 0) {
        stream->config.send_queue_limit_bytes =
            NETINS_UDP_STREAM_DEFAULT_SEND_QUEUE;
    }
    if (stream->config.recv_queue_limit_bytes == 0) {
        stream->config.recv_queue_limit_bytes =
            NETINS_UDP_STREAM_DEFAULT_RECV_QUEUE;
    }
    if (extended_config != NULL) {
        stream->h3_extension =
            calloc(1, sizeof(*stream->h3_extension));
        if (stream->h3_extension == NULL) {
            pthread_cond_destroy(&stream->cond);
            pthread_mutex_destroy(&stream->lock);
            free(stream);
            errno = ENOMEM;
            return NETINS_UDP_ERROR;
        }
        stream->h3_extension->request_body_length_unknown =
            extended_config->content_length_unknown;
        stream->h3_extension->request_header_count =
            extended_config->header_count;
        memcpy(stream->h3_extension->request_headers,
            extended_config->headers,
            sizeof(stream->h3_extension->request_headers));
    }

    stream->ref_count = 2;
    stream->public_ref = 1;
    stream->client = client;
    stream->state = NETINS_UDP_STREAM_STATE_OPENING;
    stream->stream_id = NETINS_UDP_STREAM_INVALID_ID;

    pthread_mutex_lock(&client->lock);
    int budget_exhausted =
        stream->config.send_queue_limit_bytes
            > NETINS_UDP_CLIENT_SEND_QUEUE_BUDGET
        || stream->config.recv_queue_limit_bytes
            > NETINS_UDP_CLIENT_RECV_QUEUE_BUDGET
        || client->reserved_send_queue_bytes
            > NETINS_UDP_CLIENT_SEND_QUEUE_BUDGET
                - stream->config.send_queue_limit_bytes
        || client->reserved_recv_queue_bytes
            > NETINS_UDP_CLIENT_RECV_QUEUE_BUDGET
                - stream->config.recv_queue_limit_bytes;
    if (client->state != NETINS_UDP_STATE_CONNECTED
        || client->close_requested || client->stop_requested)
    {
        int unavailable_error =
            client->close_requested || client->stop_requested
                ? ECANCELED : ENOTCONN;
        pthread_mutex_unlock(&client->lock);
        stream->ref_count = 1;
        netins_udp_stream_release(stream);
        errno = unavailable_error;
        return NETINS_UDP_ERROR;
    }
    if (client->stream_count >= NETINS_UDP_MAX_STREAMS) {
        pthread_mutex_unlock(&client->lock);
        stream->ref_count = 1;
        netins_udp_stream_release(stream);
        errno = ENOSPC;
        return NETINS_UDP_ERROR;
    }
    if (budget_exhausted) {
        pthread_mutex_unlock(&client->lock);
        stream->ref_count = 1;
        netins_udp_stream_release(stream);
        errno = ENOBUFS;
        return NETINS_UDP_ERROR;
    }
    client->reserved_send_queue_bytes +=
        stream->config.send_queue_limit_bytes;
    client->reserved_recv_queue_bytes +=
        stream->config.recv_queue_limit_bytes;
    stream->budget_reserved = 1;
    stream->next = client->streams;
    client->streams = stream;
    client->stream_count++;
    pthread_mutex_unlock(&client->lock);

    *out_stream = stream;
    netins_udp_client_wake(client);
    return NETINS_UDP_OK;
}

int
netins_udp_stream_open(netins_udp_client_t *client,
    const netins_udp_stream_config_t *config,
    netins_udp_stream_t **out_stream)
{
    return netins_udp_stream_open_internal(client, config, NULL,
        out_stream);
}

int
netins_udp_client_h3_request_open(netins_udp_client_t *client,
    const netins_udp_client_h3_config_t *config,
    netins_udp_stream_t **out_stream)
{
    if (client == NULL || config == NULL || out_stream == NULL
        || config->header_count > NETINS_UDP_CLIENT_H3_MAX_HEADERS
        || (config->content_length_unknown != 0
            && config->content_length_unknown != 1)
        || memchr(config->method, '\0', sizeof(config->method)) == NULL
        || memchr(config->path, '\0', sizeof(config->path)) == NULL
        || memchr(config->authority, '\0',
               sizeof(config->authority)) == NULL
        || memchr(config->content_type, '\0',
               sizeof(config->content_type)) == NULL)
    {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    for (uint32_t i = 0; i < config->header_count; ++i) {
        if (config->headers[i].name[0] == '\0'
            || memchr(config->headers[i].name, '\0',
                   sizeof(config->headers[i].name)) == NULL
            || memchr(config->headers[i].value, '\0',
                   sizeof(config->headers[i].value)) == NULL)
        {
            errno = EINVAL;
            return NETINS_UDP_ERROR;
        }
    }

    netins_udp_stream_config_t legacy;
    netins_udp_stream_config_init(&legacy);
    snprintf(legacy.method, sizeof(legacy.method), "%s",
        config->method);
    snprintf(legacy.path, sizeof(legacy.path), "%s", config->path);
    snprintf(legacy.authority, sizeof(legacy.authority), "%s",
        config->authority);
    snprintf(legacy.content_type, sizeof(legacy.content_type), "%s",
        config->content_type);
    legacy.request_body_length = config->content_length_unknown
        ? 0 : config->content_length;
    return netins_udp_stream_open_internal(client, &legacy, config,
        out_stream);
}

int
netins_udp_stream_wait_open(netins_udp_stream_t *stream, uint32_t timeout_ms)
{
    if (stream == NULL) {
        return NETINS_UDP_ERROR;
    }
    struct timespec deadline;
    if (timeout_ms > 0
        && netins_udp_client_make_deadline(timeout_ms, &deadline)
            != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&stream->lock);
    int wait_ret = 0;
    while (!stream->request_headers_sent
        && stream->state != NETINS_UDP_STREAM_STATE_ERROR
        && stream->state != NETINS_UDP_STREAM_STATE_CLOSED)
    {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_WOULD_BLOCK;
        }
        wait_ret = pthread_cond_timedwait(&stream->cond, &stream->lock,
            &deadline);
        if (wait_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_WOULD_BLOCK;
        }
        if (wait_ret != 0) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_ERROR;
        }
    }
    int result = stream->request_headers_sent ? NETINS_UDP_OK
        : (stream->state == NETINS_UDP_STREAM_STATE_CLOSED
            ? NETINS_UDP_CLOSED : NETINS_UDP_ERROR);
    pthread_mutex_unlock(&stream->lock);
    return result;
}

int64_t
netins_udp_stream_send(netins_udp_stream_t *stream, const void *data,
    size_t size, int fin, uint32_t timeout_ms)
{
    if (stream == NULL || (size > 0 && data == NULL)) {
        return NETINS_UDP_ERROR;
    }
    struct timespec deadline;
    if (timeout_ms > 0
        && netins_udp_client_make_deadline(timeout_ms, &deadline)
            != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&stream->lock);
    if (strcasecmp(stream->config.method, "POST") != 0) {
        pthread_mutex_unlock(&stream->lock);
        return NETINS_UDP_ERROR;
    }
    if (stream->send_fin_requested || stream->send_finished) {
        pthread_mutex_unlock(&stream->lock);
        return NETINS_UDP_CLOSED;
    }
    if (stream->close_requested
        || stream->state == NETINS_UDP_STREAM_STATE_CLOSING)
    {
        pthread_mutex_unlock(&stream->lock);
        return NETINS_UDP_CLOSED;
    }
    int request_body_length_unknown =
        stream->h3_extension != NULL
        && stream->h3_extension->request_body_length_unknown;
    if (request_body_length_unknown) {
        if ((uint64_t)size > UINT64_MAX - stream->bytes_accepted) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_ERROR;
        }

    } else {
        uint64_t body_remaining = stream->config.request_body_length
            - stream->bytes_accepted;
        if ((uint64_t)size > body_remaining) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_ERROR;
        }
        if (fin && (uint64_t)size != body_remaining) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_ERROR;
        }
    }

    int wait_ret = 0;
    while (size > 0
        && stream->send_queue_bytes >= stream->config.send_queue_limit_bytes
        && stream->state != NETINS_UDP_STREAM_STATE_ERROR
        && stream->state != NETINS_UDP_STREAM_STATE_CLOSED
        && !stream->close_requested)
    {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_WOULD_BLOCK;
        }
        wait_ret = pthread_cond_timedwait(&stream->cond, &stream->lock,
            &deadline);
        if (wait_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_WOULD_BLOCK;
        }
        if (wait_ret != 0) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_ERROR;
        }
    }
    if (stream->state == NETINS_UDP_STREAM_STATE_ERROR) {
        pthread_mutex_unlock(&stream->lock);
        return NETINS_UDP_ERROR;
    }
    if (stream->state == NETINS_UDP_STREAM_STATE_CLOSED) {
        pthread_mutex_unlock(&stream->lock);
        return NETINS_UDP_CLOSED;
    }
    if (stream->close_requested) {
        pthread_mutex_unlock(&stream->lock);
        return NETINS_UDP_CLOSED;
    }

    size_t available = stream->config.send_queue_limit_bytes
        - stream->send_queue_bytes;
    size_t accepted = size < available ? size : available;
    netins_udp_buffer_t *buffer = NULL;
    if (accepted > 0) {
        buffer = netins_udp_stream_copy_buffer(data, accepted);
        if (buffer == NULL) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_ERROR;
        }
        if (stream->send_tail == NULL) {
            stream->send_head = buffer;
        } else {
            stream->send_tail->next = buffer;
        }
        stream->send_tail = buffer;
        stream->send_queue_bytes += accepted;
        stream->bytes_accepted += accepted;
    }
    if (request_body_length_unknown) {
        if (fin && accepted == size) {
            stream->send_fin_requested = 1;
        }

    } else if (stream->bytes_accepted
        == stream->config.request_body_length)
    {
        stream->send_fin_requested = 1;
    }
    pthread_cond_broadcast(&stream->cond);
    /*
     * Parent close detaches a stream under this same lock before freeing the
     * client.  Wake while the lock still pins stream->client so close cannot
     * free the wake pipe between pointer capture and use.
     */
    netins_udp_client_wake(stream->client);
    pthread_mutex_unlock(&stream->lock);
    return (int64_t)accepted;
}

static int64_t
netins_udp_stream_recv_internal(netins_udp_stream_t *stream, void *buffer,
    size_t capacity, int *fin, uint32_t timeout_ms, int peek)
{
    if (stream == NULL || fin == NULL
        || (capacity > 0 && buffer == NULL))
    {
        return NETINS_UDP_ERROR;
    }
    *fin = 0;
    struct timespec deadline;
    if (timeout_ms > 0
        && netins_udp_client_make_deadline(timeout_ms, &deadline)
            != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&stream->lock);
    int wait_ret = 0;
    while (stream->recv_head == NULL && !stream->recv_finished
        && stream->state != NETINS_UDP_STREAM_STATE_ERROR
        && stream->state != NETINS_UDP_STREAM_STATE_CLOSED)
    {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_WOULD_BLOCK;
        }
        wait_ret = pthread_cond_timedwait(&stream->cond, &stream->lock,
            &deadline);
        if (wait_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_WOULD_BLOCK;
        }
        if (wait_ret != 0) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_ERROR;
        }
    }

    if (stream->recv_head == NULL) {
        if (stream->recv_finished) {
            *fin = 1;
            if (!peek) {
                stream->recv_eof_delivered = 1;
            }
            pthread_mutex_unlock(&stream->lock);
            return 0;
        }
        int result = stream->state == NETINS_UDP_STREAM_STATE_ERROR
            ? NETINS_UDP_ERROR : NETINS_UDP_CLOSED;
        pthread_mutex_unlock(&stream->lock);
        return result;
    }
    if (capacity == 0) {
        pthread_mutex_unlock(&stream->lock);
        return NETINS_UDP_BUFFER_TOO_SMALL;
    }

    if (peek) {
        size_t copied = 0;
        netins_udp_buffer_t *item = stream->recv_head;
        size_t item_offset = item->offset;
        while (copied < capacity && item != NULL) {
            size_t available = item->length - item_offset;
            size_t amount = available < capacity - copied
                ? available : capacity - copied;
            memcpy((unsigned char *)buffer + copied,
                item->data + item_offset, amount);
            copied += amount;
            item_offset += amount;
            if (item_offset == item->length) {
                item = item->next;
                if (item != NULL) {
                    item_offset = item->offset;
                }
            }
        }
        *fin = item == NULL && stream->recv_finished;
        pthread_mutex_unlock(&stream->lock);
        return (int64_t)copied;
    }

    size_t copied = 0;
    while (copied < capacity && stream->recv_head != NULL) {
        netins_udp_buffer_t *item = stream->recv_head;
        size_t available = item->length - item->offset;
        size_t amount = available < capacity - copied
            ? available : capacity - copied;
        memcpy((unsigned char *)buffer + copied,
            item->data + item->offset, amount);
        item->offset += amount;
        copied += amount;
        stream->recv_queue_bytes -= amount;
        stream->bytes_delivered += amount;
        if (item->offset == item->length) {
            stream->recv_head = item->next;
            if (stream->recv_head == NULL) {
                stream->recv_tail = NULL;
            }
            free(item);
        }
    }
    *fin = stream->recv_head == NULL && stream->recv_finished;
    stream->read_pending = !stream->recv_finished;
    pthread_cond_broadcast(&stream->cond);
    netins_udp_client_wake(stream->client);
    pthread_mutex_unlock(&stream->lock);
    return (int64_t)copied;
}

int64_t
netins_udp_stream_recv(netins_udp_stream_t *stream, void *buffer,
    size_t capacity, int *fin, uint32_t timeout_ms)
{
    return netins_udp_stream_recv_internal(stream, buffer, capacity,
        fin, timeout_ms, 0);
}

int64_t
netins_udp_client_h3_request_recv(netins_udp_stream_t *stream,
    void *buffer, size_t capacity, int *fin, int peek,
    uint32_t timeout_ms)
{
    return netins_udp_stream_recv_internal(stream, buffer, capacity,
        fin, timeout_ms, peek != 0);
}

int
netins_udp_stream_get_info(netins_udp_stream_t *stream,
    netins_udp_stream_info_t *out)
{
    if (stream == NULL || out == NULL) {
        return NETINS_UDP_ERROR;
    }
    if (out->struct_size != sizeof(*out)
        || out->api_version != NETINS_UDP_STREAM_API_VERSION)
    {
        return NETINS_UDP_VERSION_MISMATCH;
    }

    pthread_mutex_lock(&stream->lock);
    netins_udp_stream_info_t info = NETINS_UDP_STREAM_INFO_INITIALIZER;
    info.state = stream->state;
    info.response_status = stream->response_status;
    info.request_headers_sent = stream->request_headers_sent;
    info.response_headers_received = stream->response_headers_received;
    info.send_finished = stream->send_finished;
    info.recv_finished = stream->recv_finished;
    info.stream_error = stream->stream_error;
    info.stream_id = stream->stream_id;
    info.request_body_length = stream->config.request_body_length;
    info.bytes_accepted = stream->bytes_accepted;
    info.bytes_sent = stream->bytes_sent;
    info.bytes_received = stream->bytes_received;
    info.bytes_delivered = stream->bytes_delivered;
    info.send_queue_bytes = stream->send_queue_bytes;
    info.recv_queue_bytes = stream->recv_queue_bytes;
    info.response_content_length = stream->response_content_length;
    snprintf(info.response_content_type,
        sizeof(info.response_content_type), "%s",
        stream->response_content_type);
    *out = info;
    pthread_mutex_unlock(&stream->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_client_h3_request_get_info(netins_udp_stream_t *stream,
    netins_udp_client_h3_info_t *out)
{
    if (stream == NULL || out == NULL) {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&stream->lock);
    memset(out, 0, sizeof(*out));
    out->stream_id = stream->stream_id;
    out->response_status = stream->response_status;
    out->request_headers_ready = stream->request_headers_sent;
    out->response_headers_ready = stream->response_headers_received;
    out->send_finished = stream->send_finished;
    out->receive_finished = stream->recv_finished;
    out->stream_error = stream->stream_error;
    out->stream_system_error = stream->stream_system_error;
    out->bytes_accepted = stream->bytes_accepted;
    out->bytes_delivered = stream->bytes_delivered;
    snprintf(out->request_authority, sizeof(out->request_authority),
        "%s", stream->config.authority);
    out->response_content_length_known =
        stream->h3_extension != NULL
        && stream->h3_extension->response_content_length_known;
    out->response_content_length = stream->response_content_length;
    snprintf(out->response_content_type,
        sizeof(out->response_content_type), "%s",
        stream->response_content_type);
    if (stream->h3_extension != NULL) {
        out->peer_header_count =
            stream->h3_extension->peer_header_count;
        memcpy(out->peer_headers,
            stream->h3_extension->peer_headers,
            sizeof(out->peer_headers));
    }
    pthread_mutex_unlock(&stream->lock);
    return NETINS_UDP_OK;
}

void
netins_udp_client_h3_request_close_async(netins_udp_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    pthread_mutex_lock(&stream->lock);
    stream->close_requested = 1;
    if (stream->state != NETINS_UDP_STREAM_STATE_CLOSED
        && stream->state != NETINS_UDP_STREAM_STATE_ERROR)
    {
        stream->state = NETINS_UDP_STREAM_STATE_CLOSING;
    }
    pthread_cond_broadcast(&stream->cond);
    netins_udp_client_wake(stream->client);
    pthread_mutex_unlock(&stream->lock);
}

void
netins_udp_client_h3_request_abandon(netins_udp_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    pthread_mutex_lock(&stream->lock);
    int release_public_ref = stream->public_ref;
    stream->public_ref = 0;
    pthread_mutex_unlock(&stream->lock);
    if (release_public_ref) {
        netins_udp_stream_release(stream);
    }
}

int
netins_udp_stream_close(netins_udp_stream_t *stream, uint32_t timeout_ms)
{
    if (stream == NULL) {
        return NETINS_UDP_OK;
    }
    struct timespec deadline;
    if (timeout_ms > 0
        && netins_udp_client_make_deadline(timeout_ms, &deadline)
            != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&stream->lock);
    stream->close_requested = 1;
    if (stream->state != NETINS_UDP_STREAM_STATE_CLOSED
        && stream->state != NETINS_UDP_STREAM_STATE_ERROR)
    {
        stream->state = NETINS_UDP_STREAM_STATE_CLOSING;
    }
    pthread_cond_broadcast(&stream->cond);
    netins_udp_client_wake(stream->client);
    pthread_mutex_unlock(&stream->lock);

    pthread_mutex_lock(&stream->lock);
    while (!stream->close_notified && stream->client != NULL) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_WOULD_BLOCK;
        }
        int wait_ret = pthread_cond_timedwait(&stream->cond, &stream->lock,
            &deadline);
        if (wait_ret == ETIMEDOUT) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_WOULD_BLOCK;
        }
        if (wait_ret != 0) {
            pthread_mutex_unlock(&stream->lock);
            return NETINS_UDP_ERROR;
        }
    }
    int release_public_ref = stream->public_ref;
    stream->public_ref = 0;
    pthread_mutex_unlock(&stream->lock);
    if (release_public_ref) {
        netins_udp_stream_release(stream);
    }
    return NETINS_UDP_OK;
}

void
netins_udp_client_config_init(netins_udp_client_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->api_version = NETINS_UDP_CLIENT_API_VERSION;
    config->connect_timeout_ms = NETINS_UDP_CLIENT_DEFAULT_TIMEOUT_MS;
    config->kernel_sndbuf = NETINS_UDP_CLIENT_DEFAULT_SOCKET_BUF;
    config->kernel_rcvbuf = NETINS_UDP_CLIENT_DEFAULT_SOCKET_BUF;
    config->initial_cwnd_packets = NETINS_UDP_CLIENT_DEFAULT_CWND;
}

int
netins_udp_client_create(const netins_udp_client_config_t *config,
    netins_udp_client_t **out_client)
{
    if (config == NULL || out_client == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    *out_client = NULL;
    if (config->struct_size != sizeof(*config)
        || config->api_version != NETINS_UDP_CLIENT_API_VERSION)
    {
        errno = EPROTO;
        return NETINS_UDP_VERSION_MISMATCH;
    }
    if (config->server_address[0] == '\0' || config->server_port == 0
        || config->interface_count > NETINS_UDP_MAX_PATHS)
    {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }

    netins_udp_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        errno = ENOMEM;
        return NETINS_UDP_ERROR;
    }
    client->config = *config;
    client->config.server_address[NETINS_UDP_SERVER_ADDRESS_LEN - 1] = '\0';
    client->config.server_name[NETINS_UDP_SERVER_NAME_LEN - 1] = '\0';
    client->config.ca_file[NETINS_UDP_CA_FILE_LEN - 1] = '\0';
    for (uint32_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        client->config.interface_names[i]
            [NETINS_UDP_INTERFACE_NAME_LEN - 1] = '\0';
        client->paths[i].fd = -1;
        client->paths[i].path_id = UINT64_MAX;
    }
    if (client->config.connect_timeout_ms == 0) {
        client->config.connect_timeout_ms =
            NETINS_UDP_CLIENT_DEFAULT_TIMEOUT_MS;
    }
    if (client->config.kernel_sndbuf <= 0) {
        client->config.kernel_sndbuf = NETINS_UDP_CLIENT_DEFAULT_SOCKET_BUF;
    }
    if (client->config.kernel_rcvbuf <= 0) {
        client->config.kernel_rcvbuf = NETINS_UDP_CLIENT_DEFAULT_SOCKET_BUF;
    }
    if (client->config.initial_cwnd_packets == 0) {
        client->config.initial_cwnd_packets = NETINS_UDP_CLIENT_DEFAULT_CWND;
    }
    if (client->config.interface_count < 2) {
        client->config.enable_multipath = 0;
    }
    if (client->config.verify_certificate
        && client->config.ca_file[0] != '\0')
    {
        client->ca_store = X509_STORE_new();
        if (client->ca_store == NULL
            || X509_STORE_load_locations(client->ca_store,
                client->config.ca_file, NULL) != 1)
        {
            X509_STORE_free(client->ca_store);
            free(client);
            errno = EINVAL;
            return NETINS_UDP_ERROR;
        }
    }

    client->wake_pipe[0] = -1;
    client->wake_pipe[1] = -1;
    netins_udp_backend_rx_policy_init(&client->rx_policy);
    client->state = NETINS_UDP_STATE_NEW;
    client->congestion_control = xqc_bbr_cb;
    client->scheduler = xqc_minrtt_scheduler_cb;
    client->close_timeout_ms = NETINS_UDP_CLIENT_CLOSE_WAIT_MS;
    snprintf(client->ciphers, sizeof(client->ciphers), "%s", XQC_TLS_CIPHERS);
    snprintf(client->groups, sizeof(client->groups), "%s", XQC_TLS_GROUPS);
    int init_error = pthread_mutex_init(&client->lock, NULL);
    if (init_error != 0) {
        X509_STORE_free(client->ca_store);
        free(client);
        errno = init_error;
        return NETINS_UDP_ERROR;
    }
    init_error = pthread_cond_init(&client->state_cond, NULL);
    if (init_error != 0) {
        pthread_mutex_destroy(&client->lock);
        X509_STORE_free(client->ca_store);
        free(client);
        errno = init_error;
        return NETINS_UDP_ERROR;
    }
    client->observer = netins_udp_observer_new();
    if (client->observer == NULL) {
        pthread_cond_destroy(&client->state_cond);
        pthread_mutex_destroy(&client->lock);
        X509_STORE_free(client->ca_store);
        free(client);
        errno = ENOMEM;
        return NETINS_UDP_ERROR;
    }
    *out_client = client;
    return NETINS_UDP_OK;
}

int
netins_udp_client_wait_connected(netins_udp_client_t *client,
    uint32_t timeout_ms)
{
    if (client == NULL) {
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&client->lock);
    if (client->state == NETINS_UDP_STATE_CONNECTED) {
        pthread_mutex_unlock(&client->lock);
        return NETINS_UDP_OK;
    }
    if (!client->connect_started) {
        netins_udp_client_set_error_locked(client, ENOTCONN,
            "connection has not been started");
        pthread_mutex_unlock(&client->lock);
        return NETINS_UDP_ERROR;
    }
    if (timeout_ms == 0) {
        netins_udp_client_set_error_locked(client, ETIMEDOUT,
            "QUIC/H3 handshake is not complete");
        pthread_mutex_unlock(&client->lock);
        return NETINS_UDP_ERROR;
    }

    struct timespec deadline;
    if (netins_udp_client_make_deadline(timeout_ms, &deadline)
        != NETINS_UDP_OK)
    {
        netins_udp_client_set_error_locked(client, errno,
            "clock_gettime failed");
        pthread_mutex_unlock(&client->lock);
        return NETINS_UDP_ERROR;
    }

    int wait_ret = 0;
    while (client->state == NETINS_UDP_STATE_CONNECTING && wait_ret == 0) {
        wait_ret = pthread_cond_timedwait(&client->state_cond, &client->lock,
            &deadline);
    }
    if (client->state == NETINS_UDP_STATE_CONNECTED) {
        pthread_mutex_unlock(&client->lock);
        return NETINS_UDP_OK;
    }
    if (wait_ret == ETIMEDOUT) {
        netins_udp_client_set_error_locked(client, ETIMEDOUT,
            "timed out waiting for QUIC/H3 handshake");
    } else if (client->last_error_code == 0) {
        netins_udp_client_set_error_locked(client, ECONNABORTED,
            "QUIC/H3 connection closed before handshake completion");
    }
    pthread_mutex_unlock(&client->lock);
    return NETINS_UDP_ERROR;
}

int
netins_udp_client_configure_transport(netins_udp_client_t *client,
    const xqc_cong_ctrl_callback_t *congestion_control,
    const xqc_scheduler_callback_t *scheduler,
    uint32_t idle_timeout_ms, uint32_t close_timeout_ms)
{
    if (client == NULL || congestion_control == NULL || scheduler == NULL) {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&client->lock);
    if (client->connect_started || client->state != NETINS_UDP_STATE_NEW) {
        netins_udp_client_set_error_locked(client, EBUSY,
            "transport options must be configured before connect");
        pthread_mutex_unlock(&client->lock);
        return NETINS_UDP_ERROR;
    }
    client->congestion_control = *congestion_control;
    client->scheduler = *scheduler;
    client->idle_timeout_ms = idle_timeout_ms;
    client->close_timeout_ms = close_timeout_ms != 0
        ? close_timeout_ms : NETINS_UDP_CLIENT_CLOSE_WAIT_MS;
    pthread_mutex_unlock(&client->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_client_start_async(netins_udp_client_t *client)
{
    if (client == NULL) {
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&client->lock);
    if (client->connect_started || client->state != NETINS_UDP_STATE_NEW) {
        netins_udp_client_set_error_locked(client, EALREADY,
            "connection can only be started once");
        pthread_mutex_unlock(&client->lock);
        return NETINS_UDP_ERROR;
    }
    client->connect_started = 1;
    client->state = NETINS_UDP_STATE_CONNECTING;
    uint64_t now_us = netins_udp_client_now_us();
    if (now_us != 0
        && client->config.connect_timeout_ms
            <= (UINT64_MAX - now_us) / 1000ULL)
    {
        client->connect_deadline_us = now_us
            + (uint64_t)client->config.connect_timeout_ms * 1000ULL;
    } else {
        client->connect_deadline_us = UINT64_MAX;
    }
    pthread_mutex_unlock(&client->lock);

    /*
     * Only the local wake descriptor is initialized in the caller. DNS,
     * socket binding, engine creation and QUIC startup run on the worker so a
     * NONBLOCK connect never performs potentially blocking network setup.
     */
    if (netins_udp_client_init_wake_pipe(client) != NETINS_UDP_OK) {
        goto fail;
    }

    int thread_ret = pthread_create(&client->thread, NULL,
        netins_udp_client_thread_main, client);
    if (thread_ret != 0) {
        netins_udp_client_set_error(client, thread_ret,
            "pthread_create failed");
        goto fail;
    }
    client->thread_started = 1;
    return NETINS_UDP_OK;

fail:
    pthread_mutex_lock(&client->lock);
    client->state = NETINS_UDP_STATE_ERROR;
    if (client->last_error_code == 0) {
        netins_udp_client_set_error_locked(client, EIO,
            "failed to start QUIC/H3 connection");
    }
    pthread_cond_broadcast(&client->state_cond);
    pthread_mutex_unlock(&client->lock);
    return NETINS_UDP_ERROR;
}

void
netins_udp_client_set_close_timeout(netins_udp_client_t *client,
    uint32_t close_timeout_ms)
{
    if (client == NULL || close_timeout_ms == 0) {
        return;
    }
    pthread_mutex_lock(&client->lock);
    client->close_timeout_ms = close_timeout_ms;
    pthread_mutex_unlock(&client->lock);
}

int
netins_udp_client_connect(netins_udp_client_t *client)
{
    if (netins_udp_client_start_async(client) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    int result = netins_udp_client_wait_connected(client,
        client->config.connect_timeout_ms);
    if (result != NETINS_UDP_OK) {
        netins_udp_client_request_close(client);
    }
    return result;
}

int
netins_udp_client_get_state(netins_udp_client_t *client)
{
    if (client == NULL) {
        return NETINS_UDP_STATE_ERROR;
    }
    pthread_mutex_lock(&client->lock);
    int state = client->state;
    pthread_mutex_unlock(&client->lock);
    return state;
}

uint64_t
netins_udp_client_get_connection_id(netins_udp_client_t *client)
{
    if (client == NULL || client->observer == NULL) {
        return NETINS_UDP_INVALID_CONNECTION_ID;
    }
    netins_udp_info_t info = NETINS_UDP_INFO_INITIALIZER;
    if (netins_udp_observer_get_info(client->observer, &info) != NETINS_UDP_OK) {
        return NETINS_UDP_INVALID_CONNECTION_ID;
    }
    return info.connection_id;
}

int
netins_udp_client_get_snapshot(netins_udp_client_t *client,
    netins_udp_snapshot_t *out)
{
    if (client == NULL || client->observer == NULL) {
        return NETINS_UDP_ERROR;
    }
    return netins_udp_observer_get_snapshot(client->observer, out);
}

int
netins_udp_client_get_private_snapshot(
    netins_udp_client_t *client,
    netins_udp_observer_private_snapshot_t *out)
{
    if (client == NULL || client->observer == NULL || out == NULL) {
        return NETINS_UDP_ERROR;
    }
    return netins_udp_observer_get_private_snapshot(client->observer, out);
}

int
netins_udp_client_get_events(netins_udp_client_t *client,
    uint32_t *out_events)
{
    if (client == NULL || out_events == NULL) {
        return NETINS_UDP_ERROR;
    }
    /*
     * Connection events are transitions only for a client HTTP/3 handle.
     * Request body readiness belongs to the independent request descriptor.
     */
    *out_events = 0;
    return NETINS_UDP_OK;
}

void
netins_udp_client_set_notify(netins_udp_client_t *client,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    if (client == NULL) {
        return;
    }
    pthread_mutex_lock(&client->lock);
    client->notify = notify;
    client->notify_user_data = user_data;
    int state = client->state;
    pthread_mutex_unlock(&client->lock);
    if (notify != NULL) {
        uint32_t events = state == NETINS_UDP_STATE_CONNECTED
            ? NETINS_UDP_V2_EVENT_CONNECTED : 0;
        if (state == NETINS_UDP_STATE_CLOSED) {
            events |= NETINS_UDP_V2_EVENT_CLOSED;
        } else if (state == NETINS_UDP_STATE_ERROR) {
            events |= NETINS_UDP_V2_EVENT_ERROR;
        }
        if (events != 0) {
            notify(user_data, events);
        }
    }
}

int
netins_udp_client_h3_request_get_events(netins_udp_stream_t *stream,
    uint32_t *out_events)
{
    if (stream == NULL || out_events == NULL) {
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&stream->lock);
    uint32_t events = 0;
    if (stream->recv_head != NULL
        || (stream->recv_finished && !stream->recv_eof_delivered))
    {
        events |= NETINS_UDP_V2_EVENT_READABLE;
    }
    if (!stream->send_fin_requested && !stream->send_finished
        && !stream->close_requested
        && stream->state != NETINS_UDP_STREAM_STATE_CLOSED
        && stream->state != NETINS_UDP_STREAM_STATE_ERROR
        && stream->send_queue_bytes < stream->config.send_queue_limit_bytes)
    {
        events |= NETINS_UDP_V2_EVENT_WRITABLE;
    }
    *out_events = events;
    pthread_mutex_unlock(&stream->lock);
    return NETINS_UDP_OK;
}

void
netins_udp_client_h3_request_set_notify(netins_udp_stream_t *stream,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    if (stream == NULL) {
        return;
    }
    pthread_mutex_lock(&stream->lock);
    stream->notify = notify;
    stream->notify_user_data = user_data;
    uint32_t events = 0;
    if (stream->recv_head != NULL
        || (stream->recv_finished && !stream->recv_eof_delivered))
    {
        events |= NETINS_UDP_V2_EVENT_READABLE;
    }
    if (!stream->send_fin_requested && !stream->send_finished
        && !stream->close_requested
        && stream->state != NETINS_UDP_STREAM_STATE_CLOSED
        && stream->state != NETINS_UDP_STREAM_STATE_ERROR
        && stream->send_queue_bytes < stream->config.send_queue_limit_bytes)
    {
        events |= NETINS_UDP_V2_EVENT_WRITABLE;
    }
    if (stream->state == NETINS_UDP_STREAM_STATE_CLOSED) {
        events |= NETINS_UDP_V2_EVENT_CLOSED;
    } else if (stream->state == NETINS_UDP_STREAM_STATE_ERROR) {
        events |= NETINS_UDP_V2_EVENT_ERROR;
    }
    pthread_mutex_unlock(&stream->lock);
    if (notify != NULL && events != 0) {
        notify(user_data, events);
    }
}

int
netins_udp_client_get_last_error_code(netins_udp_client_t *client)
{
    if (client == NULL) {
        return EINVAL;
    }
    pthread_mutex_lock(&client->lock);
    int error_code = client->last_error_code;
    pthread_mutex_unlock(&client->lock);
    return error_code;
}

int
netins_udp_client_get_last_error_msg(netins_udp_client_t *client,
    char *buffer, size_t buffer_len)
{
    if (client == NULL || buffer == NULL || buffer_len == 0) {
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&client->lock);
    snprintf(buffer, buffer_len, "%s", client->last_error_msg);
    pthread_mutex_unlock(&client->lock);
    return NETINS_UDP_OK;
}

void
netins_udp_client_request_close(netins_udp_client_t *client)
{
    netins_udp_client_request_shutdown(client, 0);
}

void
netins_udp_client_request_shutdown(netins_udp_client_t *client,
    uint64_t application_error)
{
    if (client == NULL) {
        return;
    }

    pthread_mutex_lock(&client->lock);
    if (client->state != NETINS_UDP_STATE_CLOSED) {
        if (!client->close_requested || application_error != 0) {
            client->close_application_error = application_error;
        }
        client->close_requested = 1;
    }
    pthread_cond_broadcast(&client->state_cond);
    pthread_mutex_unlock(&client->lock);
    netins_udp_client_wake(client);
}

void
netins_udp_client_close(netins_udp_client_t *client)
{
    if (client == NULL) {
        return;
    }

    if (client->thread_started) {
        /*
         * A normal owner close must preserve the same drain semantics as
         * request_shutdown(0). Do not mark individual streams for immediate
         * close here: that used to race queued body data and FIN delivery.
         * The bounded wait below remains the forced-cleanup limit.
         */
        netins_udp_client_request_shutdown(client, 0);

        struct timespec deadline;
        if (netins_udp_client_make_deadline(client->close_timeout_ms,
                &deadline) == NETINS_UDP_OK)
        {
            pthread_mutex_lock(&client->lock);
            while (client->state != NETINS_UDP_STATE_CLOSED
                && client->state != NETINS_UDP_STATE_ERROR)
            {
                int ret = pthread_cond_timedwait(&client->state_cond,
                    &client->lock, &deadline);
                if (ret == ETIMEDOUT) {
                    break;
                }
            }
            pthread_mutex_unlock(&client->lock);
        }

        pthread_mutex_lock(&client->lock);
        client->stop_requested = 1;
        pthread_mutex_unlock(&client->lock);
        netins_udp_client_wake(client);
        pthread_join(client->thread, NULL);
        client->thread_started = 0;
    }

    if (client->observer != NULL) {
        (void)netins_udp_backend_connection_refresh(client->observer);
        (void)netins_udp_backend_connection_closed(client->observer,
            client->last_error_code);
    }
    if (client->engine != NULL) {
        (void)xqc_h3_ctx_destroy(client->engine);
        xqc_engine_destroy(client->engine);
        client->engine = NULL;
    }
    netins_udp_client_detach_all_streams(client,
        client->last_error_code != 0
            ? client->last_error_code : ECONNABORTED);
    for (uint32_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        netins_udp_client_close_path(&client->paths[i]);
    }
    if (client->wake_pipe[0] >= 0) {
        close(client->wake_pipe[0]);
    }
    if (client->wake_pipe[1] >= 0) {
        close(client->wake_pipe[1]);
    }
    if (client->observer != NULL) {
        netins_udp_observer_close(client->observer);
        client->observer = NULL;
    }
    pthread_cond_destroy(&client->state_cond);
    pthread_mutex_destroy(&client->lock);
    X509_STORE_free(client->ca_store);
    free(client);
}
