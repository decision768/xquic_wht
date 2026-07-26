#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "netins_udp_v2_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <xquic/netins_udp.h>
#include <xquic/xqc_errno.h>
#include <xquic/xqc_http3.h>
#include <xquic/xquic.h>

#include "netins_udp_backend.h"

#define NETINS_UDP_SERVER_RX_BATCH_PACKETS 256U
#define NETINS_UDP_SERVER_ACCEPT_QUEUE_MAX 256U
#define NETINS_UDP_SERVER_REQUEST_MAX      256U
#define NETINS_UDP_SERVER_REQUEST_SEND_MAX (4U * 1024U * 1024U)
#define NETINS_UDP_SERVER_REQUEST_RECV_MAX (4U * 1024U * 1024U)
#define NETINS_UDP_SERVER_REQUEST_RECV_CHUNK (64U * 1024U)
#define NETINS_UDP_SERVER_REQUEST_DISCARD_CHUNK (16U * 1024U)
#define NETINS_UDP_SERVER_REQUEST_DISCARD_BATCH (256U * 1024U)
#define NETINS_UDP_SERVER_CONNECTION_SEND_BUDGET (64U * 1024U * 1024U)
#define NETINS_UDP_SERVER_CONNECTION_RECV_BUDGET (64U * 1024U * 1024U)
#define NETINS_UDP_SERVER_SEND_BUDGET (256U * 1024U * 1024U)
#define NETINS_UDP_SERVER_RECV_BUDGET (256U * 1024U * 1024U)
#define NETINS_UDP_SERVER_REFRESH_US       500000ULL
#define NETINS_UDP_SERVER_POLL_MAX_MS      500

typedef struct netins_udp_server_buffer_s {
    struct netins_udp_server_buffer_s *next;
    size_t length;
    size_t offset;
    unsigned char data[];
} netins_udp_server_buffer_t;

struct netins_udp_server_request_s {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned int references;

    struct netins_udp_server_connection_s *connection;
    xqc_h3_request_t *xqc_request;
    uint64_t stream_id;
    int xqc_reference;
    int connection_reference;
    int queued;
    int public_reference;
    int headers_ready;
    int response_configured;
    int response_headers_sent;
    int send_fin_requested;
    int send_finished;
    int recv_finished;
    int recv_eof_delivered;
    int public_close_requested;
    int close_requested;
    int close_sent;
    int close_notified;
    int read_pending;
    int stream_error;
    int budget_reserved;

    char method[NETINS_UDP_V2_TRANSPORT_METHOD_LEN];
    char path[NETINS_UDP_V2_TRANSPORT_REQUEST_PATH_LEN];
    char authority[NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN];
    char request_content_type[NETINS_UDP_V2_TRANSPORT_CONTENT_TYPE_LEN];
    int request_content_length_known;
    uint64_t request_content_length;
    netins_udp_v2_transport_h3_response_config_t response;
    uint32_t peer_header_count;
    netins_udp_v2_transport_h3_header_t
        peer_headers[NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS];

    uint64_t bytes_accepted;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t bytes_delivered;
    size_t send_queue_bytes;
    size_t recv_queue_bytes;
    netins_udp_server_buffer_t *send_head;
    netins_udp_server_buffer_t *send_tail;
    netins_udp_server_buffer_t *recv_head;
    netins_udp_server_buffer_t *recv_tail;
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;

    struct netins_udp_server_request_s *all_next;
    struct netins_udp_server_request_s *accept_next;
};

struct netins_udp_server_connection_s {
    pthread_mutex_t lock;
    pthread_cond_t state_cond;
    unsigned int references;

    struct netins_udp_server_s *server;
    xqc_connection_t *xqc_connection;
    xqc_h3_conn_t *h3_connection;
    xqc_cid_t cid;
    struct sockaddr_storage local_address;
    socklen_t local_address_len;
    struct sockaddr_storage peer_address;
    socklen_t peer_address_len;
    netins_udp_observer_t *observer;

    int state;
    int last_error_code;
    char last_error_msg[NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN];
    int queued;
    int delivered;
    int close_requested;
    int close_sent;
    uint64_t application_error;

    netins_udp_v2_transport_server_request_t *requests;
    netins_udp_v2_transport_server_request_t *request_accept_head;
    netins_udp_v2_transport_server_request_t *request_accept_tail;
    size_t request_count;
    size_t request_accept_count;
    size_t reserved_send_queue_bytes;
    size_t reserved_recv_queue_bytes;
    netins_udp_v2_transport_notify_cb notify;
    void *notify_user_data;

    struct netins_udp_server_connection_s *all_next;
    struct netins_udp_server_connection_s *accept_next;
};

struct netins_udp_server_s {
    pthread_mutex_t lock;
    pthread_cond_t accept_cond;
    unsigned int handle_references;

    netins_udp_v2_transport_server_config_t config;
    int state;
    int accepting;
    int stop_requested;
    int thread_started;
    int socket_write_blocked;
    int last_error_code;
    char last_error_msg[NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN];

    int socket_fd;
    int wake_pipe[2];
    pthread_t thread;
    xqc_engine_t *engine;
    struct sockaddr_storage local_address;
    socklen_t local_address_len;
    uint64_t next_wake_us;
    uint64_t last_refresh_us;

    netins_udp_v2_transport_server_connection_t *connections;
    netins_udp_v2_transport_server_connection_t *accept_head;
    netins_udp_v2_transport_server_connection_t *accept_tail;
    size_t accept_count;
    size_t reserved_send_queue_bytes;
    size_t reserved_recv_queue_bytes;
};

static void
netins_udp_server_notify(netins_udp_v2_transport_server_t *server,
    uint32_t events)
{
    if (server == NULL || events == 0) {
        return;
    }
    pthread_mutex_lock(&server->lock);
    if (server->config.notify != NULL) {
        server->config.notify(server->config.notify_user_data, events);
    }
    pthread_mutex_unlock(&server->lock);
}

static void
netins_udp_server_connection_notify(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t events)
{
    if (connection == NULL || events == 0) {
        return;
    }
    pthread_mutex_lock(&connection->lock);
    if (connection->notify != NULL) {
        connection->notify(connection->notify_user_data, events);
    }
    pthread_mutex_unlock(&connection->lock);
}

static void
netins_udp_server_request_notify(
    netins_udp_v2_transport_server_request_t *request,
    uint32_t events)
{
    if (request == NULL || events == 0) {
        return;
    }
    pthread_mutex_lock(&request->lock);
    if (request->notify != NULL) {
        request->notify(request->notify_user_data, events);
    }
    pthread_mutex_unlock(&request->lock);
}

static uint64_t
netins_udp_server_monotonic_us(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000ULL
        + (uint64_t)now.tv_nsec / 1000ULL;
}

static uint64_t
netins_udp_server_packet_time_us(void)
{
    struct timeval now;
    if (gettimeofday(&now, NULL) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000ULL
        + (uint64_t)now.tv_usec;
}

static int
netins_udp_server_make_deadline(uint32_t timeout_ms,
    struct timespec *deadline)
{
    if (clock_gettime(CLOCK_REALTIME, deadline) != 0) {
        return -1;
    }
    deadline->tv_sec += timeout_ms / 1000U;
    deadline->tv_nsec +=
        (long)(timeout_ms % 1000U) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
    return 0;
}

static int
netins_udp_server_make_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

static void
netins_udp_server_set_error_locked(netins_udp_v2_transport_server_t *server,
    int error_code, const char *message)
{
    server->last_error_code = error_code;
    snprintf(server->last_error_msg, sizeof(server->last_error_msg), "%s",
        message != NULL ? message : "");
}

static void
netins_udp_server_connection_set_error_locked(
    netins_udp_v2_transport_server_connection_t *connection,
    int error_code, const char *message)
{
    connection->last_error_code = error_code;
    snprintf(connection->last_error_msg,
        sizeof(connection->last_error_msg), "%s",
        message != NULL ? message : "");
}

static void
netins_udp_server_wake(netins_udp_v2_transport_server_t *server)
{
    if (server == NULL || server->wake_pipe[1] < 0) {
        return;
    }
    unsigned char byte = 1;
    ssize_t written = write(server->wake_pipe[1], &byte, sizeof(byte));
    (void)written;
}

static void
netins_udp_server_drain_wake_pipe(
    netins_udp_v2_transport_server_t *server)
{
    unsigned char bytes[64];
    while (server->wake_pipe[0] >= 0) {
        ssize_t received = read(server->wake_pipe[0], bytes, sizeof(bytes));
        if (received > 0) {
            continue;
        }
        if (received < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

static void
netins_udp_server_connection_retain(
    netins_udp_v2_transport_server_connection_t *connection)
{
    pthread_mutex_lock(&connection->lock);
    connection->references++;
    pthread_mutex_unlock(&connection->lock);
}

static void
netins_udp_server_connection_release(
    netins_udp_v2_transport_server_connection_t *connection)
{
    int destroy = 0;
    pthread_mutex_lock(&connection->lock);
    if (connection->references > 0) {
        connection->references--;
        destroy = connection->references == 0;
    }
    pthread_mutex_unlock(&connection->lock);
    if (!destroy) {
        return;
    }

    if (connection->observer != NULL) {
        netins_udp_observer_close(connection->observer);
    }
    pthread_cond_destroy(&connection->state_cond);
    pthread_mutex_destroy(&connection->lock);
    free(connection);
}

static void
netins_udp_server_request_free_buffers(netins_udp_server_buffer_t *head)
{
    while (head != NULL) {
        netins_udp_server_buffer_t *next = head->next;
        free(head);
        head = next;
    }
}

static netins_udp_server_buffer_t *
netins_udp_server_request_copy_buffer(const void *data, size_t length)
{
    if (length > SIZE_MAX - sizeof(netins_udp_server_buffer_t)) {
        return NULL;
    }
    netins_udp_server_buffer_t *buffer =
        malloc(sizeof(*buffer) + length);
    if (buffer == NULL) {
        return NULL;
    }
    buffer->next = NULL;
    buffer->length = length;
    buffer->offset = 0;
    if (length > 0 && data != NULL) {
        memcpy(buffer->data, data, length);
    }
    return buffer;
}

static void
netins_udp_server_request_destroy(
    netins_udp_v2_transport_server_request_t *request)
{
    netins_udp_server_request_free_buffers(request->send_head);
    netins_udp_server_request_free_buffers(request->recv_head);
    pthread_cond_destroy(&request->cond);
    pthread_mutex_destroy(&request->lock);
    free(request);
}

static void
netins_udp_server_request_retain(
    netins_udp_v2_transport_server_request_t *request)
{
    pthread_mutex_lock(&request->lock);
    request->references++;
    pthread_mutex_unlock(&request->lock);
}

static void
netins_udp_server_request_release(
    netins_udp_v2_transport_server_request_t *request)
{
    int destroy = 0;
    pthread_mutex_lock(&request->lock);
    if (request->references > 0) {
        request->references--;
        destroy = request->references == 0;
    }
    pthread_mutex_unlock(&request->lock);
    if (destroy) {
        netins_udp_server_request_destroy(request);
    }
}

static void netins_udp_server_release(
    netins_udp_v2_transport_server_t *server);

static void
netins_udp_server_remove_connection_locked(
    netins_udp_v2_transport_server_t *server,
    netins_udp_v2_transport_server_connection_t *connection)
{
    netins_udp_v2_transport_server_connection_t **cursor =
        &server->connections;
    while (*cursor != NULL) {
        if (*cursor == connection) {
            *cursor = connection->all_next;
            connection->all_next = NULL;
            return;
        }
        cursor = &(*cursor)->all_next;
    }
}

static int
netins_udp_server_remove_queued_locked(
    netins_udp_v2_transport_server_t *server,
    netins_udp_v2_transport_server_connection_t *connection)
{
    netins_udp_v2_transport_server_connection_t *previous = NULL;
    netins_udp_v2_transport_server_connection_t *item =
        server->accept_head;
    while (item != NULL) {
        if (item == connection) {
            if (previous == NULL) {
                server->accept_head = item->accept_next;
            } else {
                previous->accept_next = item->accept_next;
            }
            if (server->accept_tail == item) {
                server->accept_tail = previous;
            }
            item->accept_next = NULL;
            item->queued = 0;
            if (server->accept_count > 0) {
                server->accept_count--;
            }
            return 1;
        }
        previous = item;
        item = item->accept_next;
    }
    return 0;
}

static void
netins_udp_server_remove_request_locked(
    netins_udp_v2_transport_server_connection_t *connection,
    netins_udp_v2_transport_server_request_t *request)
{
    netins_udp_v2_transport_server_request_t **cursor =
        &connection->requests;
    while (*cursor != NULL) {
        if (*cursor == request) {
            *cursor = request->all_next;
            request->all_next = NULL;
            if (connection->request_count > 0) {
                connection->request_count--;
            }
            return;
        }
        cursor = &(*cursor)->all_next;
    }
}

/*
 * Each admitted request reserves its complete per-request queue allowance.
 * This intentionally trades a little concurrency for a hard, simple memory
 * bound that never depends on application read/write timing.
 *
 * Caller holds server->lock, connection->lock and request->lock.
 */
static void
netins_udp_server_release_request_budget_locked(
    netins_udp_v2_transport_server_t *server,
    netins_udp_v2_transport_server_connection_t *connection,
    netins_udp_v2_transport_server_request_t *request)
{
    if (!request->budget_reserved) {
        return;
    }
    request->budget_reserved = 0;
    if (connection->reserved_send_queue_bytes
        >= NETINS_UDP_SERVER_REQUEST_SEND_MAX)
    {
        connection->reserved_send_queue_bytes -=
            NETINS_UDP_SERVER_REQUEST_SEND_MAX;
    } else {
        connection->reserved_send_queue_bytes = 0;
    }
    if (connection->reserved_recv_queue_bytes
        >= NETINS_UDP_SERVER_REQUEST_RECV_MAX)
    {
        connection->reserved_recv_queue_bytes -=
            NETINS_UDP_SERVER_REQUEST_RECV_MAX;
    } else {
        connection->reserved_recv_queue_bytes = 0;
    }
    if (server->reserved_send_queue_bytes
        >= NETINS_UDP_SERVER_REQUEST_SEND_MAX)
    {
        server->reserved_send_queue_bytes -=
            NETINS_UDP_SERVER_REQUEST_SEND_MAX;
    } else {
        server->reserved_send_queue_bytes = 0;
    }
    if (server->reserved_recv_queue_bytes
        >= NETINS_UDP_SERVER_REQUEST_RECV_MAX)
    {
        server->reserved_recv_queue_bytes -=
            NETINS_UDP_SERVER_REQUEST_RECV_MAX;
    } else {
        server->reserved_recv_queue_bytes = 0;
    }
}

static int
netins_udp_server_remove_request_queued_locked(
    netins_udp_v2_transport_server_connection_t *connection,
    netins_udp_v2_transport_server_request_t *request)
{
    netins_udp_v2_transport_server_request_t *previous = NULL;
    netins_udp_v2_transport_server_request_t *item =
        connection->request_accept_head;
    while (item != NULL) {
        if (item == request) {
            if (previous == NULL) {
                connection->request_accept_head = item->accept_next;
            } else {
                previous->accept_next = item->accept_next;
            }
            if (connection->request_accept_tail == item) {
                connection->request_accept_tail = previous;
            }
            item->accept_next = NULL;
            item->queued = 0;
            if (connection->request_accept_count > 0) {
                connection->request_accept_count--;
            }
            return 1;
        }
        previous = item;
        item = item->accept_next;
    }
    return 0;
}

static const xqc_scheduler_callback_t *
netins_udp_server_scheduler(int scheduler)
{
    switch (scheduler) {
    case 0:
    case 1:
        return &xqc_minrtt_scheduler_cb;
    case 2:
        return &xqc_backup_scheduler_cb;
    case 3:
        return &xqc_balanced_scheduler_cb;
    case 4:
        return &xqc_rap_scheduler_cb;
    case 5:
        return &xqc_act_scheduler_cb;
    case 6:
        return &xqc_bandwidth_scheduler_cb;
    case 7:
        return &xqc_fastprobe_scheduler_cb;
    case 8:
        return &xqc_adaptive_scheduler_cb;
    default:
        return NULL;
    }
}

static const xqc_cong_ctrl_callback_t *
netins_udp_server_congestion_control(int congestion_control)
{
    switch (congestion_control) {
    case 0:
    case 2:
        return &xqc_bbr_cb;
    case 1:
        return &xqc_cubic_cb;
    case 3:
        return &xqc_bbr2_cb;
    case 4:
        return &xqc_reno_cb;
    default:
        return NULL;
    }
}

static void
netins_udp_server_set_event_timer(xqc_usec_t wake_after,
    void *engine_user_data)
{
    netins_udp_v2_transport_server_t *server = engine_user_data;
    pthread_mutex_lock(&server->lock);
    server->next_wake_us = netins_udp_server_monotonic_us()
        + (wake_after > 0 ? wake_after : 1);
    pthread_mutex_unlock(&server->lock);
    netins_udp_server_wake(server);
}

static ssize_t
netins_udp_server_send_packet(netins_udp_v2_transport_server_t *server,
    const unsigned char *buffer, size_t length,
    const struct sockaddr *peer_address, socklen_t peer_address_len)
{
    if (server == NULL || server->socket_fd < 0) {
        return XQC_SOCKET_ERROR;
    }

    ssize_t sent;
    do {
        sent = sendto(server->socket_fd, buffer, length, 0,
            peer_address, peer_address_len);
    } while (sent < 0 && errno == EINTR);

    pthread_mutex_lock(&server->lock);
    if (sent >= 0) {
        server->socket_write_blocked = 0;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
        server->socket_write_blocked = 1;
    }
    pthread_mutex_unlock(&server->lock);
    if (sent >= 0) {
        return sent;
    }
    return errno == EAGAIN || errno == EWOULDBLOCK
        ? XQC_SOCKET_EAGAIN : XQC_SOCKET_ERROR;
}

static ssize_t
netins_udp_server_write_socket(const unsigned char *buffer, size_t length,
    const struct sockaddr *peer_address, socklen_t peer_address_len,
    void *connection_user_data)
{
    netins_udp_v2_transport_server_connection_t *connection =
        connection_user_data;
    return connection != NULL
        ? netins_udp_server_send_packet(connection->server, buffer, length,
            peer_address, peer_address_len)
        : XQC_SOCKET_ERROR;
}

static ssize_t
netins_udp_server_write_socket_ex(uint64_t path_id,
    const unsigned char *buffer, size_t length,
    const struct sockaddr *peer_address, socklen_t peer_address_len,
    void *connection_user_data)
{
    (void)path_id;
    return netins_udp_server_write_socket(buffer, length, peer_address,
        peer_address_len, connection_user_data);
}

static ssize_t
netins_udp_server_stateless_reset(const unsigned char *buffer, size_t length,
    const struct sockaddr *peer_address, socklen_t peer_address_len,
    const struct sockaddr *local_address, socklen_t local_address_len,
    void *user_data)
{
    (void)local_address;
    (void)local_address_len;
    return netins_udp_server_send_packet(user_data, buffer, length,
        peer_address, peer_address_len);
}

static void
netins_udp_server_update_connection_addresses(
    netins_udp_v2_transport_server_connection_t *connection,
    xqc_connection_t *xqc_connection)
{
    struct sockaddr_storage peer;
    struct sockaddr_storage local;
    socklen_t peer_len = sizeof(peer);
    socklen_t local_len = sizeof(local);
    int have_peer = xqc_conn_get_peer_addr(xqc_connection,
        (struct sockaddr *)&peer, sizeof(peer), &peer_len) == XQC_OK;
    int have_local = xqc_conn_get_local_addr(xqc_connection,
        (struct sockaddr *)&local, sizeof(local), &local_len) == XQC_OK;

    pthread_mutex_lock(&connection->lock);
    if (have_peer) {
        connection->peer_address = peer;
        connection->peer_address_len = peer_len;
    }
    if (have_local) {
        connection->local_address = local;
        connection->local_address_len = local_len;
    }
    pthread_mutex_unlock(&connection->lock);

    if (connection->observer != NULL) {
        (void)netins_udp_backend_set_path_metadata(connection->observer,
            XQC_INITIAL_PATH_ID, NULL,
            have_local ? (const struct sockaddr *)&local : NULL,
            have_local ? local_len : 0,
            have_peer ? (const struct sockaddr *)&peer : NULL,
            have_peer ? peer_len : 0);
    }
}

static int
netins_udp_server_accept_callback(xqc_engine_t *engine,
    xqc_connection_t *xqc_connection, const xqc_cid_t *cid,
    void *user_data)
{
    netins_udp_v2_transport_server_t *server = user_data;
    if (server == NULL || xqc_connection == NULL || cid == NULL) {
        return -1;
    }

    pthread_mutex_lock(&server->lock);
    int accepting = server->accepting
        && server->state == NETINS_UDP_V2_TRANSPORT_LISTENING;
    pthread_mutex_unlock(&server->lock);
    if (!accepting) {
        return -1;
    }

    netins_udp_v2_transport_server_connection_t *connection =
        calloc(1, sizeof(*connection));
    if (connection == NULL) {
        return -1;
    }
    if (pthread_mutex_init(&connection->lock, NULL) != 0) {
        free(connection);
        return -1;
    }
    if (pthread_cond_init(&connection->state_cond, NULL) != 0) {
        pthread_mutex_destroy(&connection->lock);
        free(connection);
        return -1;
    }
    connection->references = 1; /* XQUIC callback ownership. */
    connection->server = server;
    connection->xqc_connection = xqc_connection;
    connection->state = NETINS_UDP_V2_TRANSPORT_CONNECTING;
    memcpy(&connection->cid, cid, sizeof(*cid));
    connection->observer = netins_udp_observer_new();
    if (connection->observer == NULL) {
        pthread_cond_destroy(&connection->state_cond);
        pthread_mutex_destroy(&connection->lock);
        free(connection);
        return -1;
    }

    xqc_conn_set_transport_user_data(xqc_connection, connection);
    netins_udp_server_update_connection_addresses(connection, xqc_connection);
    if (netins_udp_backend_connection_bind(connection->observer, engine, cid,
            NETINS_UDP_ROLE_SERVER) != NETINS_UDP_OK)
    {
        xqc_conn_set_transport_user_data(xqc_connection, NULL);
        netins_udp_observer_close(connection->observer);
        pthread_cond_destroy(&connection->state_cond);
        pthread_mutex_destroy(&connection->lock);
        free(connection);
        return -1;
    }
    (void)netins_udp_backend_connection_refresh(connection->observer);

    pthread_mutex_lock(&server->lock);
    if (!server->accepting) {
        pthread_mutex_unlock(&server->lock);
        xqc_conn_set_transport_user_data(xqc_connection, NULL);
        netins_udp_observer_close(connection->observer);
        pthread_cond_destroy(&connection->state_cond);
        pthread_mutex_destroy(&connection->lock);
        free(connection);
        return -1;
    }
    connection->all_next = server->connections;
    server->connections = connection;
    pthread_mutex_unlock(&server->lock);
    return 0;
}

static void
netins_udp_server_refuse_callback(xqc_engine_t *engine,
    xqc_connection_t *xqc_connection, const xqc_cid_t *cid,
    void *user_data)
{
    (void)engine;
    (void)xqc_connection;
    (void)cid;
    netins_udp_v2_transport_server_connection_t *connection = user_data;
    if (connection == NULL) {
        return;
    }

    netins_udp_v2_transport_server_t *server = connection->server;
    int release_queue = 0;
    pthread_mutex_lock(&server->lock);
    release_queue = netins_udp_server_remove_queued_locked(server, connection);
    netins_udp_server_remove_connection_locked(server, connection);
    pthread_mutex_unlock(&server->lock);
    if (release_queue) {
        netins_udp_server_connection_release(connection);
    }
    netins_udp_server_connection_release(connection);
}

static void
netins_udp_server_update_cid(xqc_connection_t *xqc_connection,
    const xqc_cid_t *retired_cid, const xqc_cid_t *new_cid,
    void *connection_user_data)
{
    (void)xqc_connection;
    (void)retired_cid;
    netins_udp_v2_transport_server_connection_t *connection =
        connection_user_data;
    if (connection == NULL || new_cid == NULL) {
        return;
    }
    pthread_mutex_lock(&connection->lock);
    memcpy(&connection->cid, new_cid, sizeof(*new_cid));
    pthread_mutex_unlock(&connection->lock);
    if (connection->observer != NULL) {
        (void)netins_udp_backend_connection_update_cid(connection->observer,
            new_cid);
    }
}

static int
netins_udp_server_path_created(xqc_connection_t *xqc_connection,
    const xqc_cid_t *cid, uint64_t path_id, void *connection_user_data)
{
    (void)cid;
    netins_udp_v2_transport_server_connection_t *connection =
        connection_user_data;
    if (connection != NULL && connection->observer != NULL) {
        (void)netins_udp_backend_sync_xquic_path(connection->observer,
            xqc_connection, path_id, NULL);
    }
    netins_udp_server_connection_notify(connection,
        NETINS_UDP_V2_EVENT_PATH_CHANGED);
    return 0;
}

static void
netins_udp_server_path_removed(const xqc_cid_t *cid, uint64_t path_id,
    void *connection_user_data)
{
    (void)cid;
    (void)path_id;
    netins_udp_v2_transport_server_connection_t *connection =
        connection_user_data;
    if (connection != NULL && connection->observer != NULL) {
        (void)netins_udp_backend_connection_refresh(connection->observer);
    }
    netins_udp_server_connection_notify(connection,
        NETINS_UDP_V2_EVENT_PATH_CHANGED);
}

static void
netins_udp_server_peer_address_changed(xqc_connection_t *xqc_connection,
    void *connection_user_data)
{
    netins_udp_v2_transport_server_connection_t *connection =
        connection_user_data;
    if (connection != NULL) {
        netins_udp_server_update_connection_addresses(connection,
            xqc_connection);
    }
}

static void
netins_udp_server_path_peer_address_changed(
    xqc_connection_t *xqc_connection, uint64_t path_id,
    void *connection_user_data)
{
    netins_udp_v2_transport_server_connection_t *connection =
        connection_user_data;
    if (connection != NULL && connection->observer != NULL) {
        (void)netins_udp_backend_sync_xquic_path(connection->observer,
            xqc_connection, path_id, NULL);
    }
}

static int
netins_udp_server_h3_connection_create(xqc_h3_conn_t *h3_connection,
    const xqc_cid_t *cid, void *connection_user_data)
{
    netins_udp_v2_transport_server_connection_t *connection =
        connection_user_data;
    if (connection == NULL) {
        return -1;
    }
    pthread_mutex_lock(&connection->lock);
    connection->h3_connection = h3_connection;
    if (cid != NULL) {
        memcpy(&connection->cid, cid, sizeof(*cid));
    }
    pthread_mutex_unlock(&connection->lock);
    xqc_h3_conn_set_user_data(h3_connection, connection);
    return 0;
}

static void
netins_udp_server_h3_handshake_finished(xqc_h3_conn_t *h3_connection,
    void *connection_user_data)
{
    (void)h3_connection;
    netins_udp_v2_transport_server_connection_t *connection =
        connection_user_data;
    if (connection == NULL) {
        return;
    }
    netins_udp_v2_transport_server_t *server = connection->server;
    int queued = 0;

    pthread_mutex_lock(&server->lock);
    pthread_mutex_lock(&connection->lock);
    if (server->accepting
        && server->accept_count < NETINS_UDP_SERVER_ACCEPT_QUEUE_MAX
        && !connection->queued && !connection->delivered
        && !connection->close_requested)
    {
        connection->state = NETINS_UDP_V2_TRANSPORT_CONNECTED;
        connection->queued = 1;
        connection->references++;
        if (server->accept_tail == NULL) {
            server->accept_head = connection;
        } else {
            server->accept_tail->accept_next = connection;
        }
        server->accept_tail = connection;
        server->accept_count++;
        queued = 1;
        pthread_cond_broadcast(&connection->state_cond);
        pthread_cond_broadcast(&server->accept_cond);
    } else {
        connection->close_requested = 1;
    }
    pthread_mutex_unlock(&connection->lock);
    pthread_mutex_unlock(&server->lock);

    if (connection->observer != NULL) {
        (void)netins_udp_backend_connection_handshake_finished(
            connection->observer);
    }
    if (queued) {
        netins_udp_server_notify(server, NETINS_UDP_V2_EVENT_ACCEPTABLE);
    }
    netins_udp_server_wake(server);
}

static int
netins_udp_server_h3_connection_close(xqc_h3_conn_t *h3_connection,
    const xqc_cid_t *cid, void *connection_user_data)
{
    (void)cid;
    netins_udp_v2_transport_server_connection_t *connection =
        connection_user_data;
    if (connection == NULL) {
        return 0;
    }
    netins_udp_v2_transport_server_t *server = connection->server;
    int connection_error = h3_connection != NULL
        ? xqc_h3_conn_get_errno(h3_connection) : 0;
    if (connection_error == H3_NO_ERROR) {
        connection_error = 0;
    }
    int release_queue = 0;

    pthread_mutex_lock(&server->lock);
    release_queue = netins_udp_server_remove_queued_locked(server, connection);
    netins_udp_server_remove_connection_locked(server, connection);
    pthread_mutex_lock(&connection->lock);
    connection->xqc_connection = NULL;
    connection->h3_connection = NULL;
    if (connection_error != 0) {
        connection->state = NETINS_UDP_V2_TRANSPORT_ERROR;
        netins_udp_server_connection_set_error_locked(connection, EPROTO,
            "XQUIC server connection closed with a transport error");
    } else {
        connection->state = NETINS_UDP_V2_TRANSPORT_CLOSED;
    }
    pthread_cond_broadcast(&connection->state_cond);
    pthread_mutex_unlock(&connection->lock);
    pthread_cond_broadcast(&server->accept_cond);
    pthread_mutex_unlock(&server->lock);

    if (connection->observer != NULL) {
        (void)netins_udp_backend_connection_refresh(connection->observer);
        (void)netins_udp_backend_connection_closed(connection->observer,
            connection_error);
    }
    netins_udp_server_connection_notify(connection, connection_error != 0
        ? NETINS_UDP_V2_EVENT_ERROR : NETINS_UDP_V2_EVENT_CLOSED);
    if (release_queue) {
        netins_udp_server_connection_release(connection);
    }
    netins_udp_server_connection_release(connection);
    return 0;
}

static int
netins_udp_server_h3_header_name_equals(const xqc_http_header_t *header,
    const char *name)
{
    size_t name_len = strlen(name);
    return header->name.iov_len == name_len
        && strncasecmp((const char *)header->name.iov_base, name,
            name_len) == 0;
}

static int
netins_udp_server_h3_copy_header_value(char *destination,
    size_t destination_size, const xqc_http_header_t *header)
{
    if (destination == NULL || destination_size == 0 || header == NULL
        || header->value.iov_len >= destination_size)
    {
        return -1;
    }
    memcpy(destination, header->value.iov_base, header->value.iov_len);
    destination[header->value.iov_len] = '\0';
    return 0;
}

static int
netins_udp_server_h3_parse_content_length(
    const xqc_http_header_t *header, uint64_t *out)
{
    char value[32];
    if (netins_udp_server_h3_copy_header_value(value, sizeof(value),
            header) != 0
        || value[0] == '\0')
    {
        return -1;
    }
    uint64_t parsed = 0;
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor)
    {
        if (*cursor < '0' || *cursor > '9'
            || parsed > (UINT64_MAX - (uint64_t)(*cursor - '0')) / 10U)
        {
            return -1;
        }
        parsed = parsed * 10U + (uint64_t)(*cursor - '0');
    }
    *out = parsed;
    return 0;
}

static void
netins_udp_server_request_mark_error(
    netins_udp_v2_transport_server_request_t *request, int error_code)
{
    pthread_mutex_lock(&request->lock);
    if (request->stream_error == 0) {
        request->stream_error = error_code != 0 ? error_code : EIO;
    }
    request->close_requested = 1;
    netins_udp_v2_transport_server_connection_t *connection =
        request->connection;
    if (connection != NULL) {
        netins_udp_server_wake(connection->server);
    }
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);
    netins_udp_server_request_notify(request, NETINS_UDP_V2_EVENT_ERROR);
}

static int
netins_udp_server_h3_parse_request_headers(
    netins_udp_v2_transport_server_request_t *request,
    const xqc_http_headers_t *headers)
{
    int parse_error = 0;
    pthread_mutex_lock(&request->lock);
    request->peer_header_count = 0;
    for (size_t i = 0; i < headers->count; ++i) {
        const xqc_http_header_t *header = &headers->headers[i];
        if (netins_udp_server_h3_header_name_equals(header, ":method")) {
            parse_error |= netins_udp_server_h3_copy_header_value(
                request->method, sizeof(request->method), header) != 0;

        } else if (netins_udp_server_h3_header_name_equals(header,
                       ":path"))
        {
            parse_error |= netins_udp_server_h3_copy_header_value(
                request->path, sizeof(request->path), header) != 0;

        } else if (netins_udp_server_h3_header_name_equals(header,
                       ":authority")
            || netins_udp_server_h3_header_name_equals(header, "host"))
        {
            parse_error |= netins_udp_server_h3_copy_header_value(
                request->authority, sizeof(request->authority), header) != 0;

        } else if (netins_udp_server_h3_header_name_equals(header,
                       "content-type"))
        {
            parse_error |= netins_udp_server_h3_copy_header_value(
                request->request_content_type,
                sizeof(request->request_content_type), header) != 0;

        } else if (netins_udp_server_h3_header_name_equals(header,
                       "content-length"))
        {
            uint64_t content_length = 0;
            if (request->request_content_length_known
                || netins_udp_server_h3_parse_content_length(header,
                    &content_length) != 0)
            {
                parse_error = 1;
            } else {
                request->request_content_length_known = 1;
                request->request_content_length = content_length;
            }

        } else if (header->name.iov_len > 0
            && ((const unsigned char *)header->name.iov_base)[0] != ':'
            && request->peer_header_count
                < NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS)
        {
            netins_udp_v2_transport_h3_header_t *destination =
                &request->peer_headers[request->peer_header_count];
            if (header->name.iov_len < sizeof(destination->name)
                && header->value.iov_len < sizeof(destination->value))
            {
                memcpy(destination->name, header->name.iov_base,
                    header->name.iov_len);
                destination->name[header->name.iov_len] = '\0';
                memcpy(destination->value, header->value.iov_base,
                    header->value.iov_len);
                destination->value[header->value.iov_len] = '\0';
                request->peer_header_count++;
            }
        }
    }
    if (request->method[0] == '\0' || request->path[0] == '\0') {
        parse_error = 1;
    }
    if (request->request_content_type[0] == '\0') {
        snprintf(request->request_content_type,
            sizeof(request->request_content_type),
            "application/octet-stream");
    }
    if (parse_error && request->stream_error == 0) {
        request->stream_error = EPROTO;
        request->close_requested = 1;
    }
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);
    return parse_error ? -1 : 0;
}

static void
netins_udp_server_queue_request(
    netins_udp_v2_transport_server_request_t *request)
{
    netins_udp_v2_transport_server_connection_t *connection =
        request->connection;
    if (connection == NULL) {
        return;
    }

    int queued = 0;
    pthread_mutex_lock(&connection->lock);
    pthread_mutex_lock(&request->lock);
    if (connection->state == NETINS_UDP_V2_TRANSPORT_CONNECTED
        && !connection->close_requested
        && !request->queued && !request->public_reference
        && !request->close_requested
        && connection->request_accept_count
            < NETINS_UDP_SERVER_REQUEST_MAX)
    {
        request->queued = 1;
        request->references++;
        if (connection->request_accept_tail == NULL) {
            connection->request_accept_head = request;
        } else {
            connection->request_accept_tail->accept_next = request;
        }
        connection->request_accept_tail = request;
        connection->request_accept_count++;
        queued = 1;
    } else {
        request->close_requested = 1;
    }
    pthread_cond_broadcast(&request->cond);
    pthread_cond_broadcast(&connection->state_cond);
    pthread_mutex_unlock(&request->lock);
    pthread_mutex_unlock(&connection->lock);
    if (queued) {
        netins_udp_server_connection_notify(connection,
            NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE);
    }
    if (!queued) {
        netins_udp_server_wake(connection->server);
    }
}

static void
netins_udp_server_request_drain_body(
    netins_udp_v2_transport_server_request_t *request,
    xqc_h3_request_t *xqc_request)
{
    unsigned char discard_buffer[NETINS_UDP_SERVER_REQUEST_DISCARD_CHUNK];
    size_t discarded_this_batch = 0;

    for (;;) {
        netins_udp_server_buffer_t *discarded_queue = NULL;
        pthread_mutex_lock(&request->lock);
        int discard = request->public_close_requested;
        if (discard && request->recv_head != NULL) {
            discarded_queue = request->recv_head;
            request->recv_head = NULL;
            request->recv_tail = NULL;
            request->recv_queue_bytes = 0;
        }
        size_t available = discard
            ? sizeof(discard_buffer)
            : NETINS_UDP_SERVER_REQUEST_RECV_MAX
                - request->recv_queue_bytes;
        if (available == 0 || request->recv_finished
            || request->close_requested)
        {
            request->read_pending =
                available == 0 && !request->recv_finished;
            pthread_mutex_unlock(&request->lock);
            netins_udp_server_request_free_buffers(discarded_queue);
            return;
        }
        size_t capacity = discard ? available
            : (available < NETINS_UDP_SERVER_REQUEST_RECV_CHUNK
                ? available : NETINS_UDP_SERVER_REQUEST_RECV_CHUNK);
        pthread_mutex_unlock(&request->lock);
        netins_udp_server_request_free_buffers(discarded_queue);

        netins_udp_server_buffer_t *buffer = discard ? NULL
            : netins_udp_server_request_copy_buffer(NULL, capacity);
        if (!discard && buffer == NULL) {
            netins_udp_server_request_mark_error(request, ENOMEM);
            return;
        }
        uint8_t fin = 0;
        ssize_t received = xqc_h3_request_recv_body(xqc_request,
            discard ? discard_buffer : buffer->data, capacity, &fin);
        if (received == -XQC_EAGAIN) {
            free(buffer);
            pthread_mutex_lock(&request->lock);
            request->read_pending = 0;
            pthread_mutex_unlock(&request->lock);
            return;
        }
        if (received < 0) {
            free(buffer);
            netins_udp_server_request_mark_error(request, (int)-received);
            return;
        }

        pthread_mutex_lock(&request->lock);
        if (received > 0) {
            if (request->public_close_requested) {
                free(buffer);
                buffer = NULL;
            } else {
                buffer->length = (size_t)received;
                if (request->recv_tail == NULL) {
                    request->recv_head = buffer;
                } else {
                    request->recv_tail->next = buffer;
                }
                request->recv_tail = buffer;
                request->recv_queue_bytes += (size_t)received;
            }
            request->bytes_received += (uint64_t)received;
            if (request->request_content_length_known
                && request->bytes_received
                    > request->request_content_length)
            {
                request->stream_error = EPROTO;
                request->close_requested = 1;
            }
        } else {
            free(buffer);
        }
        if (fin) {
            request->recv_finished = 1;
            request->read_pending = 0;
            if (request->request_content_length_known
                && request->bytes_received
                    != request->request_content_length)
            {
                request->stream_error = EPROTO;
                request->close_requested = 1;
            }
        }
        pthread_cond_broadcast(&request->cond);
        pthread_mutex_unlock(&request->lock);
        if (received > 0 || fin) {
            netins_udp_server_request_notify(request,
                NETINS_UDP_V2_EVENT_READABLE);
        }
        if (fin || received == 0) {
            return;
        }
        if (discard) {
            discarded_this_batch += (size_t)received;
            if (discarded_this_batch
                >= NETINS_UDP_SERVER_REQUEST_DISCARD_BATCH)
            {
                pthread_mutex_lock(&request->lock);
                request->read_pending = !request->recv_finished
                    && !request->close_requested;
                netins_udp_v2_transport_server_connection_t *connection =
                    request->connection;
                pthread_mutex_unlock(&request->lock);
                if (connection != NULL) {
                    netins_udp_server_wake(connection->server);
                }
                return;
            }
        }
    }
}

static int
netins_udp_server_h3_request_create(xqc_h3_request_t *xqc_request,
    void *request_user_data)
{
    (void)request_user_data;
    netins_udp_v2_transport_server_connection_t *connection =
        xqc_h3_get_conn_user_data_by_request(xqc_request);
    if (connection == NULL) {
        return XQC_ERROR;
    }

    netins_udp_v2_transport_server_request_t *request =
        calloc(1, sizeof(*request));
    if (request == NULL) {
        (void)xqc_h3_request_close(xqc_request);
        return XQC_OK;
    }
    int init_error = pthread_mutex_init(&request->lock, NULL);
    if (init_error == 0) {
        init_error = pthread_cond_init(&request->cond, NULL);
        if (init_error != 0) {
            pthread_mutex_destroy(&request->lock);
        }
    }
    if (init_error != 0) {
        free(request);
        (void)xqc_h3_request_close(xqc_request);
        return XQC_OK;
    }

    request->references = 1;
    request->xqc_reference = 1;
    request->connection_reference = 1;
    request->connection = connection;
    request->xqc_request = xqc_request;
    request->stream_id = (uint64_t)xqc_h3_stream_id(xqc_request);
    netins_udp_server_connection_retain(connection);

    netins_udp_v2_transport_server_t *server = connection->server;
    pthread_mutex_lock(&server->lock);
    pthread_mutex_lock(&connection->lock);
    pthread_mutex_lock(&request->lock);
    int budget_exhausted =
        connection->reserved_send_queue_bytes
            > NETINS_UDP_SERVER_CONNECTION_SEND_BUDGET
                - NETINS_UDP_SERVER_REQUEST_SEND_MAX
        || connection->reserved_recv_queue_bytes
            > NETINS_UDP_SERVER_CONNECTION_RECV_BUDGET
                - NETINS_UDP_SERVER_REQUEST_RECV_MAX
        || server->reserved_send_queue_bytes
            > NETINS_UDP_SERVER_SEND_BUDGET
                - NETINS_UDP_SERVER_REQUEST_SEND_MAX
        || server->reserved_recv_queue_bytes
            > NETINS_UDP_SERVER_RECV_BUDGET
                - NETINS_UDP_SERVER_REQUEST_RECV_MAX;
    if (!server->accepting
        || connection->close_requested
        || connection->state == NETINS_UDP_V2_TRANSPORT_CLOSING
        || connection->state == NETINS_UDP_V2_TRANSPORT_CLOSED
        || connection->state == NETINS_UDP_V2_TRANSPORT_ERROR
        || connection->request_count >= NETINS_UDP_SERVER_REQUEST_MAX
        || budget_exhausted)
    {
        pthread_mutex_unlock(&request->lock);
        pthread_mutex_unlock(&connection->lock);
        pthread_mutex_unlock(&server->lock);
        request->connection_reference = 0;
        netins_udp_server_connection_release(connection);
        netins_udp_server_request_release(request);
        (void)xqc_h3_request_close(xqc_request);
        return XQC_OK;
    }
    connection->reserved_send_queue_bytes +=
        NETINS_UDP_SERVER_REQUEST_SEND_MAX;
    connection->reserved_recv_queue_bytes +=
        NETINS_UDP_SERVER_REQUEST_RECV_MAX;
    server->reserved_send_queue_bytes +=
        NETINS_UDP_SERVER_REQUEST_SEND_MAX;
    server->reserved_recv_queue_bytes +=
        NETINS_UDP_SERVER_REQUEST_RECV_MAX;
    request->budget_reserved = 1;
    request->all_next = connection->requests;
    connection->requests = request;
    connection->request_count++;
    pthread_mutex_unlock(&request->lock);
    pthread_mutex_unlock(&connection->lock);
    pthread_mutex_unlock(&server->lock);

    xqc_h3_request_set_user_data(xqc_request, request);
    return XQC_OK;
}

static int
netins_udp_server_h3_request_read(xqc_h3_request_t *xqc_request,
    xqc_request_notify_flag_t flags, void *user_data)
{
    netins_udp_v2_transport_server_request_t *request = user_data;
    if (request == NULL) {
        return XQC_ERROR;
    }

    if (flags & XQC_REQ_NOTIFY_READ_HEADER) {
        uint8_t fin = 0;
        xqc_http_headers_t *headers =
            xqc_h3_request_recv_headers(xqc_request, &fin);
        if (headers == NULL
            || netins_udp_server_h3_parse_request_headers(request,
                headers) != 0)
        {
            netins_udp_server_request_mark_error(request, EPROTO);
            return XQC_OK;
        }
        pthread_mutex_lock(&request->lock);
        request->headers_ready = 1;
        if (fin) {
            request->recv_finished = 1;
            if (request->request_content_length_known
                && request->request_content_length != 0)
            {
                request->stream_error = EPROTO;
                request->close_requested = 1;
            }
        }
        pthread_cond_broadcast(&request->cond);
        pthread_mutex_unlock(&request->lock);
        netins_udp_server_request_notify(request,
            NETINS_UDP_V2_EVENT_READABLE);
        netins_udp_server_queue_request(request);
    }

    if (flags & XQC_REQ_NOTIFY_READ_TRAILER) {
        uint8_t fin = 0;
        if (xqc_h3_request_recv_headers(xqc_request, &fin) == NULL) {
            netins_udp_server_request_mark_error(request, EPROTO);
            return XQC_OK;
        }
        if (fin) {
            pthread_mutex_lock(&request->lock);
            request->recv_finished = 1;
            if (request->request_content_length_known
                && request->bytes_received
                    != request->request_content_length)
            {
                request->stream_error = EPROTO;
                request->close_requested = 1;
            }
            pthread_cond_broadcast(&request->cond);
            pthread_mutex_unlock(&request->lock);
            netins_udp_server_request_notify(request,
                NETINS_UDP_V2_EVENT_READABLE);
        }
    }

    if (flags & XQC_REQ_NOTIFY_READ_BODY) {
        netins_udp_server_request_drain_body(request, xqc_request);
    }
    if (flags & XQC_REQ_NOTIFY_READ_EMPTY_FIN) {
        pthread_mutex_lock(&request->lock);
        request->recv_finished = 1;
        if (request->request_content_length_known
            && request->bytes_received
                != request->request_content_length)
        {
            request->stream_error = EPROTO;
            request->close_requested = 1;
        }
        pthread_cond_broadcast(&request->cond);
        pthread_mutex_unlock(&request->lock);
        netins_udp_server_request_notify(request,
            NETINS_UDP_V2_EVENT_READABLE);
    }
    return XQC_OK;
}

static int
netins_udp_server_h3_request_write(xqc_h3_request_t *xqc_request,
    void *user_data)
{
    (void)xqc_request;
    netins_udp_v2_transport_server_request_t *request = user_data;
    if (request == NULL) {
        return XQC_ERROR;
    }
    pthread_mutex_lock(&request->lock);
    netins_udp_v2_transport_server_connection_t *connection =
        request->connection;
    if (connection != NULL) {
        netins_udp_server_wake(connection->server);
    }
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);
    netins_udp_server_request_notify(request,
        NETINS_UDP_V2_EVENT_WRITABLE);
    return XQC_OK;
}

static void
netins_udp_server_h3_request_closing(xqc_h3_request_t *xqc_request,
    xqc_int_t error, void *user_data)
{
    (void)xqc_request;
    netins_udp_v2_transport_server_request_t *request = user_data;
    if (request == NULL) {
        return;
    }
    pthread_mutex_lock(&request->lock);
    if (error != 0 && request->stream_error == 0) {
        request->stream_error = (int)error;
    }
    request->close_requested = 1;
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);
    if (error != 0) {
        netins_udp_server_request_notify(request,
            NETINS_UDP_V2_EVENT_ERROR);
    }
}

static int
netins_udp_server_h3_request_close(xqc_h3_request_t *xqc_request,
    void *user_data)
{
    (void)xqc_request;
    netins_udp_v2_transport_server_request_t *request = user_data;
    if (request == NULL) {
        return XQC_OK;
    }

    pthread_mutex_lock(&request->lock);
    netins_udp_v2_transport_server_connection_t *connection =
        request->connection;
    pthread_mutex_unlock(&request->lock);

    int release_queue = 0;
    int release_xqc = 0;
    int release_connection = 0;
    uint32_t terminal_event = NETINS_UDP_V2_EVENT_CLOSED;
    if (connection != NULL) {
        netins_udp_v2_transport_server_t *server = connection->server;
        pthread_mutex_lock(&server->lock);
        pthread_mutex_lock(&connection->lock);
        pthread_mutex_lock(&request->lock);
        release_queue = netins_udp_server_remove_request_queued_locked(
            connection, request);
        netins_udp_server_remove_request_locked(connection, request);
        netins_udp_server_release_request_budget_locked(server, connection,
            request);
        request->connection = NULL;
        release_xqc = request->xqc_reference;
        release_connection = request->connection_reference;
        request->xqc_reference = 0;
        request->connection_reference = 0;
        request->xqc_request = NULL;
        request->close_notified = 1;
        request->close_requested = 1;
        if (request->stream_error == 0 && !request->recv_finished) {
            request->stream_error = ECANCELED;
        }
        terminal_event = request->stream_error != 0
            ? NETINS_UDP_V2_EVENT_ERROR : NETINS_UDP_V2_EVENT_CLOSED;
        pthread_cond_broadcast(&request->cond);
        pthread_cond_broadcast(&connection->state_cond);
        pthread_mutex_unlock(&request->lock);
        pthread_mutex_unlock(&connection->lock);
        pthread_mutex_unlock(&server->lock);
    } else {
        pthread_mutex_lock(&request->lock);
        release_xqc = request->xqc_reference;
        release_connection = request->connection_reference;
        request->xqc_reference = 0;
        request->connection_reference = 0;
        request->xqc_request = NULL;
        request->close_notified = 1;
        request->close_requested = 1;
        if (request->stream_error == 0 && !request->recv_finished) {
            request->stream_error = ECANCELED;
        }
        terminal_event = request->stream_error != 0
            ? NETINS_UDP_V2_EVENT_ERROR : NETINS_UDP_V2_EVENT_CLOSED;
        pthread_cond_broadcast(&request->cond);
        pthread_mutex_unlock(&request->lock);
    }

    netins_udp_server_request_notify(request, terminal_event);
    if (release_queue) {
        netins_udp_server_request_release(request);
    }
    if (release_xqc) {
        netins_udp_server_request_release(request);
    }
    if (release_connection && connection != NULL) {
        netins_udp_server_connection_release(connection);
    }
    return XQC_OK;
}

static void
netins_udp_server_h3_set_header(xqc_http_header_t *header,
    const char *name, const char *value)
{
    memset(header, 0, sizeof(*header));
    header->name.iov_base = (void *)name;
    header->name.iov_len = strlen(name);
    header->value.iov_base = (void *)value;
    header->value.iov_len = strlen(value);
}

static int
netins_udp_server_request_send_headers(
    netins_udp_v2_transport_server_request_t *request,
    xqc_h3_request_t *xqc_request)
{
    pthread_mutex_lock(&request->lock);
    netins_udp_v2_transport_h3_response_config_t response =
        request->response;
    pthread_mutex_unlock(&request->lock);

    xqc_http_header_t fields[
        3 + NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS];
    size_t count = 0;
    char status[4];
    char content_length[32];
    snprintf(status, sizeof(status), "%d", response.status);
    netins_udp_server_h3_set_header(&fields[count++], ":status", status);
    netins_udp_server_h3_set_header(&fields[count++], "content-type",
        response.content_type);
    if (!response.content_length_unknown) {
        snprintf(content_length, sizeof(content_length), "%llu",
            (unsigned long long)response.content_length);
        netins_udp_server_h3_set_header(&fields[count++],
            "content-length", content_length);
    }
    for (uint32_t i = 0; i < response.header_count; ++i) {
        netins_udp_server_h3_set_header(&fields[count++],
            response.headers[i].name, response.headers[i].value);
    }

    xqc_http_headers_t headers;
    memset(&headers, 0, sizeof(headers));
    headers.headers = fields;
    headers.count = count;
    headers.capacity = count;
    uint8_t fin = !response.content_length_unknown
        && response.content_length == 0;
    ssize_t sent = xqc_h3_request_send_headers(xqc_request, &headers, fin);
    if (sent == -XQC_EAGAIN) {
        return 0;
    }
    if (sent < 0) {
        netins_udp_server_request_mark_error(request, (int)-sent);
        return -1;
    }

    pthread_mutex_lock(&request->lock);
    request->response_headers_sent = 1;
    if (fin) {
        request->send_fin_requested = 1;
        request->send_finished = 1;
    }
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);
    return 1;
}

static int
netins_udp_server_request_drive_send(
    netins_udp_v2_transport_server_request_t *request,
    xqc_h3_request_t *xqc_request)
{
    int activity = 0;
    for (;;) {
        pthread_mutex_lock(&request->lock);
        if (request->send_finished || request->close_requested
            || !request->response_headers_sent)
        {
            pthread_mutex_unlock(&request->lock);
            return activity;
        }
        netins_udp_server_buffer_t *buffer = request->send_head;
        int finish_only =
            buffer == NULL && request->send_fin_requested;
        if (buffer == NULL && !finish_only) {
            pthread_mutex_unlock(&request->lock);
            return activity;
        }
        size_t remaining = buffer != NULL
            ? buffer->length - buffer->offset : 0;
        int fin = buffer != NULL && request->send_fin_requested
            && buffer == request->send_tail;
        unsigned char *data = buffer != NULL
            ? buffer->data + buffer->offset : NULL;
        pthread_mutex_unlock(&request->lock);

        ssize_t sent = finish_only
            ? xqc_h3_request_finish(xqc_request)
            : xqc_h3_request_send_body(xqc_request, data, remaining,
                (uint8_t)fin);
        if (sent == -XQC_EAGAIN) {
            return activity;
        }
        if (sent < 0) {
            netins_udp_server_request_mark_error(request, (int)-sent);
            return activity;
        }

        pthread_mutex_lock(&request->lock);
        if (finish_only) {
            request->send_finished = 1;

        } else if (sent > 0) {
            buffer->offset += (size_t)sent;
            request->send_queue_bytes -= (size_t)sent;
            request->bytes_sent += (uint64_t)sent;
            if (buffer->offset == buffer->length) {
                request->send_head = buffer->next;
                if (request->send_head == NULL) {
                    request->send_tail = NULL;
                }
                free(buffer);
            }
            if (fin && request->send_head == NULL) {
                request->send_finished = 1;
            }

        } else {
            request->send_finished = 1;
        }
        int finished = request->send_finished;
        pthread_cond_broadcast(&request->cond);
        pthread_mutex_unlock(&request->lock);
        netins_udp_server_request_notify(request,
            NETINS_UDP_V2_EVENT_WRITABLE);
        activity = 1;
        if (finish_only || sent == 0 || (fin && finished)) {
            return activity;
        }
    }
}

static int
netins_udp_server_request_should_close_locked(
    const netins_udp_v2_transport_server_request_t *request)
{
    if (request->close_requested) {
        return 1;
    }
    if (!request->public_close_requested) {
        return 0;
    }
    /*
     * A public close relinquishes delivery of the request body, but it must
     * not immediately reset a peer that is still uploading. Keep the XQUIC
     * request alive until the bounded discard drain observes FIN (or an
     * actual stream/connection error). This prevents large early-response
     * POSTs from stalling behind unread flow-controlled data.
     */
    if (!request->recv_finished && request->stream_error == 0) {
        return 0;
    }
    int response_complete_queued = request->response_configured
        && (request->response.content_length_unknown
            ? request->send_fin_requested
            : request->bytes_accepted
                == request->response.content_length);
    /*
     * xqc_h3_request_close() actively resets the QUIC stream. Once a complete
     * response is queued, leave the stream to finish naturally so the queued
     * FIN/data are not replaced by RESET_STREAM.
     */
    return !response_complete_queued || request->stream_error != 0;
}

static int
netins_udp_server_process_request(
    netins_udp_v2_transport_server_request_t *request)
{
    pthread_mutex_lock(&request->lock);
    xqc_h3_request_t *xqc_request = request->xqc_request;
    int terminal = request->close_notified || xqc_request == NULL;
    int close_requested =
        netins_udp_server_request_should_close_locked(request);
    int close_sent = request->close_sent;
    int response_configured = request->response_configured;
    int response_headers_sent = request->response_headers_sent;
    int read_pending = request->read_pending;
    pthread_mutex_unlock(&request->lock);
    if (terminal) {
        return 0;
    }

    if (close_requested && !close_sent) {
        pthread_mutex_lock(&request->lock);
        request->close_sent = 1;
        pthread_mutex_unlock(&request->lock);
        int result = xqc_h3_request_close(xqc_request);
        if (result != XQC_OK) {
            netins_udp_server_request_mark_error(request,
                result < 0 ? -result : result);
        }
        return 1;
    }

    int activity = 0;
    if (read_pending) {
        netins_udp_server_request_drain_body(request, xqc_request);
        pthread_mutex_lock(&request->lock);
        close_requested =
            netins_udp_server_request_should_close_locked(request);
        close_sent = request->close_sent;
        pthread_mutex_unlock(&request->lock);
        if (close_requested && !close_sent) {
            pthread_mutex_lock(&request->lock);
            request->close_sent = 1;
            pthread_mutex_unlock(&request->lock);
            int result = xqc_h3_request_close(xqc_request);
            if (result != XQC_OK) {
                netins_udp_server_request_mark_error(request,
                    result < 0 ? -result : result);
            }
            return 1;
        }
    }
    if (response_configured && !response_headers_sent) {
        int header_result =
            netins_udp_server_request_send_headers(request, xqc_request);
        if (header_result < 0) {
            return 1;
        }
        if (header_result == 0) {
            return activity;
        }
        activity = 1;
    }
    if (response_configured) {
        activity |= netins_udp_server_request_drive_send(request,
            xqc_request);
    }

    pthread_mutex_lock(&request->lock);
    close_requested =
        netins_udp_server_request_should_close_locked(request);
    close_sent = request->close_sent;
    pthread_mutex_unlock(&request->lock);
    if (close_requested && !close_sent) {
        pthread_mutex_lock(&request->lock);
        request->close_sent = 1;
        pthread_mutex_unlock(&request->lock);
        int result = xqc_h3_request_close(xqc_request);
        if (result != XQC_OK) {
            netins_udp_server_request_mark_error(request,
                result < 0 ? -result : result);
        }
        activity = 1;
    }
    return activity;
}

static void
netins_udp_server_release_request_snapshot(
    netins_udp_v2_transport_server_request_t **requests, size_t count)
{
    if (requests == NULL) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        netins_udp_server_request_release(requests[i]);
    }
    free(requests);
}

static netins_udp_v2_transport_server_request_t **
netins_udp_server_request_snapshot(
    netins_udp_v2_transport_server_t *server, size_t *out_count)
{
    *out_count = 0;
    pthread_mutex_lock(&server->lock);
    size_t capacity = 0;
    for (netins_udp_v2_transport_server_connection_t *connection =
             server->connections;
         connection != NULL; connection = connection->all_next)
    {
        pthread_mutex_lock(&connection->lock);
        capacity += connection->request_count;
        pthread_mutex_unlock(&connection->lock);
    }
    netins_udp_v2_transport_server_request_t **requests =
        capacity > 0 ? calloc(capacity, sizeof(*requests)) : NULL;
    if (capacity > 0 && requests == NULL) {
        pthread_mutex_unlock(&server->lock);
        return NULL;
    }
    for (netins_udp_v2_transport_server_connection_t *connection =
             server->connections;
         connection != NULL && *out_count < capacity;
         connection = connection->all_next)
    {
        pthread_mutex_lock(&connection->lock);
        for (netins_udp_v2_transport_server_request_t *request =
                 connection->requests;
             request != NULL && *out_count < capacity;
             request = request->all_next)
        {
            netins_udp_server_request_retain(request);
            requests[(*out_count)++] = request;
        }
        pthread_mutex_unlock(&connection->lock);
    }
    pthread_mutex_unlock(&server->lock);
    return requests;
}

static void
netins_udp_server_process_requests(
    netins_udp_v2_transport_server_t *server)
{
    size_t count = 0;
    netins_udp_v2_transport_server_request_t **requests =
        netins_udp_server_request_snapshot(server, &count);
    int activity = 0;
    for (size_t i = 0; i < count; ++i) {
        activity |= netins_udp_server_process_request(requests[i]);
    }
    netins_udp_server_release_request_snapshot(requests, count);
    if (activity) {
        xqc_engine_main_logic(server->engine);
    }
}

static int
netins_udp_server_create_socket(netins_udp_v2_transport_server_t *server)
{
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    char port_text[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_PASSIVE;
    snprintf(port_text, sizeof(port_text), "%u",
        (unsigned)server->config.port);

    const char *host = server->config.host[0] != '\0'
        ? server->config.host : NULL;
    int resolve_result = getaddrinfo(host, port_text, &hints, &addresses);
    if (resolve_result != 0) {
        pthread_mutex_lock(&server->lock);
        netins_udp_server_set_error_locked(server, EADDRNOTAVAIL,
            gai_strerror(resolve_result));
        pthread_mutex_unlock(&server->lock);
        errno = EADDRNOTAVAIL;
        return -1;
    }

    int saved_error = EADDRNOTAVAIL;
    for (struct addrinfo *item = addresses; item != NULL;
         item = item->ai_next)
    {
        if (item->ai_family != AF_INET && item->ai_family != AF_INET6) {
            continue;
        }
        int fd = socket(item->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0) {
            saved_error = errno;
            continue;
        }
        int enabled = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
            &enabled, sizeof(enabled));
        if (item->ai_family == AF_INET6 && host == NULL) {
            int dual_stack = 0;
            (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                &dual_stack, sizeof(dual_stack));
        }
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF,
                &server->config.kernel_sndbuf,
                sizeof(server->config.kernel_sndbuf)) != 0
            || setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                &server->config.kernel_rcvbuf,
                sizeof(server->config.kernel_rcvbuf)) != 0
            || netins_udp_server_make_nonblocking(fd) != 0
            || bind(fd, item->ai_addr, item->ai_addrlen) != 0)
        {
            saved_error = errno;
            close(fd);
            continue;
        }

        server->local_address_len = sizeof(server->local_address);
        if (getsockname(fd, (struct sockaddr *)&server->local_address,
                &server->local_address_len) != 0)
        {
            saved_error = errno;
            close(fd);
            continue;
        }
        server->socket_fd = fd;
        freeaddrinfo(addresses);
        return 0;
    }

    freeaddrinfo(addresses);
    pthread_mutex_lock(&server->lock);
    char message[NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN];
    snprintf(message, sizeof(message), "bind UDP listener: %s",
        strerror(saved_error));
    netins_udp_server_set_error_locked(server, saved_error, message);
    pthread_mutex_unlock(&server->lock);
    errno = saved_error;
    return -1;
}

static int
netins_udp_server_create_engine(netins_udp_v2_transport_server_t *server)
{
    const xqc_scheduler_callback_t *scheduler =
        netins_udp_server_scheduler(server->config.scheduler);
    const xqc_cong_ctrl_callback_t *congestion_control =
        netins_udp_server_congestion_control(
            server->config.congestion_control);
    if (scheduler == NULL || congestion_control == NULL) {
        errno = EOPNOTSUPP;
        return -1;
    }

    xqc_config_t engine_config;
    if (xqc_engine_get_default_config(&engine_config, XQC_ENGINE_SERVER)
        != XQC_OK)
    {
        errno = EIO;
        return -1;
    }

    xqc_engine_callback_t engine_callbacks;
    memset(&engine_callbacks, 0, sizeof(engine_callbacks));
    engine_callbacks.set_event_timer = netins_udp_server_set_event_timer;

    xqc_transport_callbacks_t transport_callbacks;
    memset(&transport_callbacks, 0, sizeof(transport_callbacks));
    transport_callbacks.server_accept = netins_udp_server_accept_callback;
    transport_callbacks.server_refuse = netins_udp_server_refuse_callback;
    transport_callbacks.write_socket = netins_udp_server_write_socket;
    transport_callbacks.write_socket_ex = netins_udp_server_write_socket_ex;
    transport_callbacks.stateless_reset = netins_udp_server_stateless_reset;
    transport_callbacks.conn_update_cid_notify =
        netins_udp_server_update_cid;
    transport_callbacks.path_created_notify =
        netins_udp_server_path_created;
    transport_callbacks.path_removed_notify =
        netins_udp_server_path_removed;
    transport_callbacks.conn_peer_addr_changed_notify =
        netins_udp_server_peer_address_changed;
    transport_callbacks.path_peer_addr_changed_notify =
        netins_udp_server_path_peer_address_changed;

    xqc_engine_ssl_config_t ssl_config;
    memset(&ssl_config, 0, sizeof(ssl_config));
    ssl_config.private_key_file = server->config.private_key_file;
    ssl_config.cert_file = server->config.certificate_file;
    ssl_config.ciphers = XQC_TLS_CIPHERS;
    ssl_config.groups = XQC_TLS_GROUPS;

    server->engine = xqc_engine_create(XQC_ENGINE_SERVER, &engine_config,
        &ssl_config, &engine_callbacks, &transport_callbacks, server);
    if (server->engine == NULL) {
        errno = EIO;
        return -1;
    }

    xqc_conn_settings_t settings;
    memset(&settings, 0, sizeof(settings));
    settings.pacing_on = 1;
    settings.cong_ctrl_callback = *congestion_control;
    settings.cc_params.customize_on = 1;
    settings.cc_params.init_cwnd = server->config.initial_cwnd_packets;
    settings.proto_version = XQC_VERSION_V1;
    settings.idle_time_out = server->config.idle_timeout_ms;
    settings.spurious_loss_detect_on = 1;
    settings.scheduler_callback = *scheduler;
    settings.reinj_ctl_callback = xqc_deadline_reinj_ctl_cb;
    settings.adaptive_ack_frequency = 1;
    settings.mp_ack_on_any_path = 0;
    settings.enable_multipath = server->config.enable_multipath;
    settings.multipath_version = XQC_MULTIPATH_10;
    settings.init_recv_window = 64 * 1024 * 1024;
    xqc_server_set_conn_settings(server->engine, &settings);

    xqc_h3_callbacks_t h3_callbacks;
    memset(&h3_callbacks, 0, sizeof(h3_callbacks));
    h3_callbacks.h3c_cbs.h3_conn_create_notify =
        netins_udp_server_h3_connection_create;
    h3_callbacks.h3c_cbs.h3_conn_close_notify =
        netins_udp_server_h3_connection_close;
    h3_callbacks.h3c_cbs.h3_conn_handshake_finished =
        netins_udp_server_h3_handshake_finished;
    h3_callbacks.h3r_cbs.h3_request_create_notify =
        netins_udp_server_h3_request_create;
    h3_callbacks.h3r_cbs.h3_request_close_notify =
        netins_udp_server_h3_request_close;
    h3_callbacks.h3r_cbs.h3_request_read_notify =
        netins_udp_server_h3_request_read;
    h3_callbacks.h3r_cbs.h3_request_write_notify =
        netins_udp_server_h3_request_write;
    h3_callbacks.h3r_cbs.h3_request_closing_notify =
        netins_udp_server_h3_request_closing;
    if (xqc_h3_ctx_init(server->engine, &h3_callbacks) != XQC_OK) {
        xqc_engine_destroy(server->engine);
        server->engine = NULL;
        errno = EIO;
        return -1;
    }
    return 0;
}

static void
netins_udp_server_fail_runtime(netins_udp_v2_transport_server_t *server,
    int error_code, const char *operation)
{
    char message[NETINS_UDP_V2_TRANSPORT_ERROR_MSG_LEN];
    snprintf(message, sizeof(message), "%s: %s", operation,
        strerror(error_code));
    pthread_mutex_lock(&server->lock);
    server->state = NETINS_UDP_V2_TRANSPORT_ERROR;
    server->accepting = 0;
    netins_udp_server_set_error_locked(server, error_code, message);
    for (netins_udp_v2_transport_server_connection_t *connection =
             server->connections;
         connection != NULL; connection = connection->all_next)
    {
        pthread_mutex_lock(&connection->lock);
        connection->state = NETINS_UDP_V2_TRANSPORT_ERROR;
        netins_udp_server_connection_set_error_locked(connection, error_code,
            message);
        for (netins_udp_v2_transport_server_request_t *request =
                 connection->requests;
             request != NULL; request = request->all_next)
        {
            pthread_mutex_lock(&request->lock);
            if (request->stream_error == 0) {
                request->stream_error = error_code;
            }
            request->close_requested = 1;
            if (request->notify != NULL) {
                request->notify(request->notify_user_data,
                    NETINS_UDP_V2_EVENT_ERROR);
            }
            pthread_cond_broadcast(&request->cond);
            pthread_mutex_unlock(&request->lock);
        }
        if (connection->notify != NULL) {
            connection->notify(connection->notify_user_data,
                NETINS_UDP_V2_EVENT_ERROR);
        }
        pthread_cond_broadcast(&connection->state_cond);
        pthread_mutex_unlock(&connection->lock);
        if (connection->observer != NULL) {
            (void)netins_udp_backend_connection_error(connection->observer,
                error_code, message);
        }
    }
    if (server->config.notify != NULL) {
        server->config.notify(server->config.notify_user_data,
            NETINS_UDP_V2_EVENT_ERROR);
    }
    pthread_cond_broadcast(&server->accept_cond);
    pthread_mutex_unlock(&server->lock);
}

static int
netins_udp_server_receive_packets(
    netins_udp_v2_transport_server_t *server)
{
    unsigned char packet[65536];
    int processed = 0;
    while ((uint32_t)processed < NETINS_UDP_SERVER_RX_BATCH_PACKETS) {
        struct sockaddr_storage peer_address;
        socklen_t peer_address_len = sizeof(peer_address);
        ssize_t received = recvfrom(server->socket_fd, packet,
            sizeof(packet), 0, (struct sockaddr *)&peer_address,
            &peer_address_len);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            return -1;
        }
        if (received == 0) {
            break;
        }
        (void)xqc_engine_packet_process(server->engine, packet,
            (size_t)received,
            (const struct sockaddr *)&server->local_address,
            server->local_address_len,
            (const struct sockaddr *)&peer_address, peer_address_len,
            netins_udp_server_packet_time_us(), server);
        processed++;
    }
    return processed;
}

static void
netins_udp_server_drive_connection_closes(
    netins_udp_v2_transport_server_t *server)
{
    for (;;) {
        netins_udp_v2_transport_server_connection_t *target = NULL;
        xqc_connection_t *xqc_connection = NULL;
        xqc_cid_t cid;
        uint64_t application_error = 0;

        pthread_mutex_lock(&server->lock);
        for (netins_udp_v2_transport_server_connection_t *connection =
                 server->connections;
             connection != NULL; connection = connection->all_next)
        {
            pthread_mutex_lock(&connection->lock);
            if (connection->close_requested && !connection->close_sent) {
                connection->close_sent = 1;
                if (connection->state != NETINS_UDP_V2_TRANSPORT_ERROR
                    && connection->state != NETINS_UDP_V2_TRANSPORT_CLOSED)
                {
                    connection->state =
                        NETINS_UDP_V2_TRANSPORT_CLOSING;
                }
                xqc_connection = connection->xqc_connection;
                cid = connection->cid;
                application_error = connection->application_error;
                connection->references++;
                target = connection;
                pthread_cond_broadcast(&connection->state_cond);
                pthread_mutex_unlock(&connection->lock);
                break;
            }
            pthread_mutex_unlock(&connection->lock);
        }
        pthread_mutex_unlock(&server->lock);

        if (target == NULL) {
            break;
        }
        if (target->observer != NULL) {
            (void)netins_udp_backend_connection_closing(target->observer);
        }
        if (application_error != 0 && xqc_connection != NULL) {
            (void)xqc_conn_close_with_error(xqc_connection,
                application_error);
        } else {
            (void)xqc_h3_conn_close(server->engine, &cid);
        }
        xqc_engine_main_logic(server->engine);
        netins_udp_server_connection_release(target);
    }
}

static void
netins_udp_server_release_connection_snapshot(
    netins_udp_v2_transport_server_connection_t **connections,
    size_t count)
{
    if (connections == NULL) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        netins_udp_server_connection_release(connections[i]);
    }
    free(connections);
}

static netins_udp_v2_transport_server_connection_t **
netins_udp_server_connection_snapshot(
    netins_udp_v2_transport_server_t *server, size_t *out_count)
{
    *out_count = 0;
    pthread_mutex_lock(&server->lock);
    size_t capacity = 0;
    for (netins_udp_v2_transport_server_connection_t *connection =
             server->connections;
         connection != NULL;
         connection = connection->all_next)
    {
        capacity++;
    }
    netins_udp_v2_transport_server_connection_t **connections =
        capacity > 0 ? calloc(capacity, sizeof(*connections)) : NULL;
    if (capacity > 0 && connections == NULL) {
        pthread_mutex_unlock(&server->lock);
        return NULL;
    }
    for (netins_udp_v2_transport_server_connection_t *connection =
             server->connections;
         connection != NULL && *out_count < capacity;
         connection = connection->all_next)
    {
        netins_udp_server_connection_retain(connection);
        connections[(*out_count)++] = connection;
    }
    pthread_mutex_unlock(&server->lock);
    return connections;
}

static void
netins_udp_server_continue_blocked_writes(
    netins_udp_v2_transport_server_t *server)
{
    pthread_mutex_lock(&server->lock);
    server->socket_write_blocked = 0;
    pthread_mutex_unlock(&server->lock);

    size_t count = 0;
    netins_udp_v2_transport_server_connection_t **connections =
        netins_udp_server_connection_snapshot(server, &count);
    for (size_t i = 0; i < count; ++i) {
        pthread_mutex_lock(&connections[i]->lock);
        xqc_connection_t *xqc_connection = connections[i]->xqc_connection;
        pthread_mutex_unlock(&connections[i]->lock);
        if (xqc_connection != NULL) {
            xqc_conn_continue_send_by_conn(xqc_connection);
        }
    }
    netins_udp_server_release_connection_snapshot(connections, count);
}

static void
netins_udp_server_refresh_connections(
    netins_udp_v2_transport_server_t *server, uint64_t now_us)
{
    if (now_us - server->last_refresh_us < NETINS_UDP_SERVER_REFRESH_US) {
        return;
    }
    size_t count = 0;
    netins_udp_v2_transport_server_connection_t **connections =
        netins_udp_server_connection_snapshot(server, &count);

    for (size_t i = 0; i < count; ++i) {
        if (connections[i]->observer != NULL) {
            (void)netins_udp_backend_connection_refresh(
                connections[i]->observer);
        }
    }
    netins_udp_server_release_connection_snapshot(connections, count);
    server->last_refresh_us = now_us;
}

static int
netins_udp_server_poll_timeout_ms(
    netins_udp_v2_transport_server_t *server, uint64_t now_us)
{
    int timeout_ms = NETINS_UDP_SERVER_POLL_MAX_MS;
    pthread_mutex_lock(&server->lock);
    uint64_t wake_us = server->next_wake_us;
    for (netins_udp_v2_transport_server_connection_t *connection =
             server->connections;
         connection != NULL; connection = connection->all_next)
    {
        pthread_mutex_lock(&connection->lock);
        int close_pending =
            connection->close_requested && !connection->close_sent;
        pthread_mutex_unlock(&connection->lock);
        if (close_pending) {
            timeout_ms = 0;
            break;
        }
    }
    pthread_mutex_unlock(&server->lock);
    if (timeout_ms != 0 && wake_us != 0) {
        if (wake_us <= now_us) {
            timeout_ms = 0;
        } else {
            uint64_t remaining_us = wake_us - now_us;
            int timer_ms = (int)((remaining_us + 999ULL) / 1000ULL);
            if (timer_ms < timeout_ms) {
                timeout_ms = timer_ms;
            }
        }
    }
    return timeout_ms;
}

static void *
netins_udp_server_thread_main(void *argument)
{
    netins_udp_v2_transport_server_t *server = argument;
    for (;;) {
        pthread_mutex_lock(&server->lock);
        int stop_requested = server->stop_requested;
        int write_blocked = server->socket_write_blocked;
        pthread_mutex_unlock(&server->lock);
        if (stop_requested) {
            break;
        }

        netins_udp_server_drive_connection_closes(server);
        netins_udp_server_process_requests(server);
        uint64_t now_us = netins_udp_server_monotonic_us();
        int timeout_ms = netins_udp_server_poll_timeout_ms(server, now_us);
        struct pollfd descriptors[2] = {
            { server->wake_pipe[0], POLLIN, 0 },
            { server->socket_fd, POLLIN | (write_blocked ? POLLOUT : 0), 0 }
        };
        int poll_result;
        do {
            poll_result = poll(descriptors, 2, timeout_ms);
        } while (poll_result < 0 && errno == EINTR);
        if (poll_result < 0) {
            netins_udp_server_fail_runtime(server, errno, "poll");
            break;
        }
        if (descriptors[0].revents & POLLIN) {
            netins_udp_server_drain_wake_pipe(server);
        }
        if (descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            int socket_error = 0;
            socklen_t error_len = sizeof(socket_error);
            if (getsockopt(server->socket_fd, SOL_SOCKET, SO_ERROR,
                    &socket_error, &error_len) != 0
                || socket_error == 0)
            {
                socket_error = EIO;
            }
            netins_udp_server_fail_runtime(server, socket_error,
                "UDP listener");
            break;
        }
        if (descriptors[1].revents & POLLIN) {
            int received = netins_udp_server_receive_packets(server);
            if (received < 0) {
                netins_udp_server_fail_runtime(server, errno, "recvfrom");
                break;
            }
            if (received > 0) {
                xqc_engine_finish_recv(server->engine);
            }
        }
        if (descriptors[1].revents & POLLOUT) {
            netins_udp_server_continue_blocked_writes(server);
        }

        now_us = netins_udp_server_monotonic_us();
        pthread_mutex_lock(&server->lock);
        int timer_due = server->next_wake_us != 0
            && server->next_wake_us <= now_us;
        if (timer_due) {
            server->next_wake_us = 0;
        }
        pthread_mutex_unlock(&server->lock);
        if (timer_due) {
            xqc_engine_main_logic(server->engine);
        }
        netins_udp_server_refresh_connections(server, now_us);
    }
    return NULL;
}

static void
netins_udp_server_force_detach_requests(
    netins_udp_v2_transport_server_connection_t *connection)
{
    netins_udp_v2_transport_server_t *server = connection->server;
    pthread_mutex_lock(&server->lock);
    pthread_mutex_lock(&connection->lock);
    netins_udp_v2_transport_server_request_t *request =
        connection->requests;
    connection->requests = NULL;
    connection->request_accept_head = NULL;
    connection->request_accept_tail = NULL;
    connection->request_count = 0;
    connection->request_accept_count = 0;
    pthread_cond_broadcast(&connection->state_cond);

    for (netins_udp_v2_transport_server_request_t *item = request;
         item != NULL; item = item->all_next)
    {
        pthread_mutex_lock(&item->lock);
        netins_udp_server_release_request_budget_locked(server, connection,
            item);
        pthread_mutex_unlock(&item->lock);
    }
    pthread_mutex_unlock(&connection->lock);
    pthread_mutex_unlock(&server->lock);

    while (request != NULL) {
        netins_udp_v2_transport_server_request_t *next =
            request->all_next;
        pthread_mutex_lock(&request->lock);
        int release_queue = request->queued;
        int release_xqc = request->xqc_reference;
        int release_connection = request->connection_reference;
        request->queued = 0;
        request->xqc_reference = 0;
        request->connection_reference = 0;
        request->all_next = NULL;
        request->accept_next = NULL;
        request->connection = NULL;
        request->xqc_request = NULL;
        request->close_requested = 1;
        request->close_notified = 1;
        if (request->stream_error == 0
            && (!request->send_finished || !request->recv_finished))
        {
            request->stream_error = ECANCELED;
        }
        uint32_t terminal_event = request->stream_error != 0
            ? NETINS_UDP_V2_EVENT_ERROR
            : NETINS_UDP_V2_EVENT_CLOSED;
        pthread_cond_broadcast(&request->cond);
        pthread_mutex_unlock(&request->lock);
        netins_udp_server_request_notify(request, terminal_event);
        if (release_queue) {
            netins_udp_server_request_release(request);
        }
        if (release_xqc) {
            netins_udp_server_request_release(request);
        }
        if (release_connection) {
            netins_udp_server_connection_release(connection);
        }
        request = next;
    }
}

static void
netins_udp_server_destroy(netins_udp_v2_transport_server_t *server)
{
    pthread_mutex_lock(&server->lock);
    server->stop_requested = 1;
    pthread_mutex_unlock(&server->lock);
    netins_udp_server_wake(server);
    if (server->thread_started) {
        pthread_join(server->thread, NULL);
        server->thread_started = 0;
    }

    if (server->engine != NULL) {
        (void)xqc_h3_ctx_destroy(server->engine);
        xqc_engine_destroy(server->engine);
        server->engine = NULL;
    }

    /*
     * Engine destruction normally runs every H3/refuse callback. Keep a
     * defensive sweep for objects that never reached an XQUIC close callback.
     * Public request handles may outlive their accepted connection handle, so
     * detach them before the shared engine/server is released.
     */
    netins_udp_v2_transport_server_connection_t *connection =
        server->connections;
    server->connections = NULL;
    server->accept_head = NULL;
    server->accept_tail = NULL;
    server->accept_count = 0;
    while (connection != NULL) {
        netins_udp_v2_transport_server_connection_t *next =
            connection->all_next;
        connection->all_next = NULL;
        netins_udp_server_force_detach_requests(connection);
        pthread_mutex_lock(&connection->lock);
        int release_queue = connection->queued;
        connection->queued = 0;
        connection->xqc_connection = NULL;
        connection->h3_connection = NULL;
        connection->state = NETINS_UDP_V2_TRANSPORT_CLOSED;
        pthread_cond_broadcast(&connection->state_cond);
        pthread_mutex_unlock(&connection->lock);
        if (release_queue) {
            netins_udp_server_connection_release(connection);
        }
        netins_udp_server_connection_release(connection);
        connection = next;
    }
    if (server->socket_fd >= 0) {
        close(server->socket_fd);
    }
    if (server->wake_pipe[0] >= 0) {
        close(server->wake_pipe[0]);
    }
    if (server->wake_pipe[1] >= 0) {
        close(server->wake_pipe[1]);
    }
    pthread_cond_destroy(&server->accept_cond);
    pthread_mutex_destroy(&server->lock);
    free(server);
}

static void
netins_udp_server_release(netins_udp_v2_transport_server_t *server)
{
    int destroy = 0;
    pthread_mutex_lock(&server->lock);
    if (server->handle_references > 0) {
        server->handle_references--;
        destroy = server->handle_references == 0;
    }
    pthread_mutex_unlock(&server->lock);
    if (destroy) {
        netins_udp_server_destroy(server);
    }
}

int
netins_udp_v2_transport_server_create(
    const netins_udp_v2_transport_server_config_t *config,
    netins_udp_v2_transport_server_t **out_server)
{
    if (config == NULL || out_server == NULL
        || config->port == 0
        || config->certificate_file[0] == '\0'
        || config->private_key_file[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }
    *out_server = NULL;

    netins_udp_v2_transport_server_t *server =
        calloc(1, sizeof(*server));
    if (server == NULL) {
        errno = ENOMEM;
        return -1;
    }
    server->socket_fd = -1;
    server->wake_pipe[0] = -1;
    server->wake_pipe[1] = -1;
    server->config = *config;
    server->handle_references = 1;
    server->state = NETINS_UDP_V2_TRANSPORT_NEW;

    int init_error = pthread_mutex_init(&server->lock, NULL);
    if (init_error != 0) {
        free(server);
        errno = init_error;
        return -1;
    }
    init_error = pthread_cond_init(&server->accept_cond, NULL);
    if (init_error != 0) {
        pthread_mutex_destroy(&server->lock);
        free(server);
        errno = init_error;
        return -1;
    }
    if (pipe(server->wake_pipe) != 0
        || netins_udp_server_make_nonblocking(server->wake_pipe[0]) != 0
        || netins_udp_server_make_nonblocking(server->wake_pipe[1]) != 0
        || netins_udp_server_create_socket(server) != 0
        || netins_udp_server_create_engine(server) != 0)
    {
        int error_code = errno != 0 ? errno : EIO;
        netins_udp_server_destroy(server);
        errno = error_code;
        return -1;
    }

    pthread_mutex_lock(&server->lock);
    server->accepting = 1;
    server->state = NETINS_UDP_V2_TRANSPORT_LISTENING;
    pthread_mutex_unlock(&server->lock);
    int thread_result = pthread_create(&server->thread, NULL,
        netins_udp_server_thread_main, server);
    if (thread_result != 0) {
        pthread_mutex_lock(&server->lock);
        server->accepting = 0;
        pthread_mutex_unlock(&server->lock);
        netins_udp_server_destroy(server);
        errno = thread_result;
        return -1;
    }
    server->thread_started = 1;
    *out_server = server;
    return 0;
}

int
netins_udp_v2_transport_server_get_state(
    netins_udp_v2_transport_server_t *server)
{
    if (server == NULL) {
        return NETINS_UDP_V2_TRANSPORT_ERROR;
    }
    pthread_mutex_lock(&server->lock);
    int state = server->state;
    pthread_mutex_unlock(&server->lock);
    return state;
}

int
netins_udp_v2_transport_server_get_last_error_code(
    netins_udp_v2_transport_server_t *server)
{
    if (server == NULL) {
        return EINVAL;
    }
    pthread_mutex_lock(&server->lock);
    int error_code = server->last_error_code;
    pthread_mutex_unlock(&server->lock);
    return error_code;
}

int
netins_udp_v2_transport_server_get_last_error_msg(
    netins_udp_v2_transport_server_t *server,
    char *buffer, size_t buffer_len)
{
    if (server == NULL || buffer == NULL || buffer_len == 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&server->lock);
    snprintf(buffer, buffer_len, "%s", server->last_error_msg);
    pthread_mutex_unlock(&server->lock);
    return 0;
}

int
netins_udp_v2_transport_server_get_events(
    netins_udp_v2_transport_server_t *server, uint32_t *out_events)
{
    if (server == NULL || out_events == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&server->lock);
    *out_events = server->accept_head != NULL && server->accepting
        ? NETINS_UDP_V2_EVENT_ACCEPTABLE : 0;
    pthread_mutex_unlock(&server->lock);
    return 0;
}

void
netins_udp_v2_transport_server_set_notify(
    netins_udp_v2_transport_server_t *server,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    if (server == NULL) {
        return;
    }
    pthread_mutex_lock(&server->lock);
    server->config.notify = notify;
    server->config.notify_user_data = user_data;
    uint32_t events = server->accept_head != NULL && server->accepting
        ? NETINS_UDP_V2_EVENT_ACCEPTABLE : 0;
    if (server->state == NETINS_UDP_V2_TRANSPORT_CLOSED) {
        events |= NETINS_UDP_V2_EVENT_CLOSED;
    } else if (server->state == NETINS_UDP_V2_TRANSPORT_ERROR) {
        events |= NETINS_UDP_V2_EVENT_ERROR;
    }
    pthread_mutex_unlock(&server->lock);
    if (notify != NULL && events != 0) {
        notify(user_data, events);
    }
}

int
netins_udp_v2_transport_server_accept(
    netins_udp_v2_transport_server_t *server,
    uint32_t timeout_ms, int wait_forever,
    netins_udp_v2_transport_server_connection_t **out_connection)
{
    if (server == NULL || out_connection == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_connection = NULL;
    struct timespec deadline;
    if (!wait_forever && timeout_ms > 0
        && netins_udp_server_make_deadline(timeout_ms, &deadline) != 0)
    {
        return -1;
    }

    pthread_mutex_lock(&server->lock);
    int wait_result = 0;
    while (server->accept_head == NULL && server->accepting
        && server->state == NETINS_UDP_V2_TRANSPORT_LISTENING)
    {
        if (!wait_forever && timeout_ms == 0) {
            break;
        }
        wait_result = wait_forever
            ? pthread_cond_wait(&server->accept_cond, &server->lock)
            : pthread_cond_timedwait(&server->accept_cond, &server->lock,
                &deadline);
        if (wait_result != 0) {
            break;
        }
    }

    netins_udp_v2_transport_server_connection_t *connection =
        server->accepting
            && server->state == NETINS_UDP_V2_TRANSPORT_LISTENING
        ? server->accept_head : NULL;
    if (connection != NULL) {
        server->accept_head = connection->accept_next;
        if (server->accept_head == NULL) {
            server->accept_tail = NULL;
        }
        connection->accept_next = NULL;
        if (server->accept_count > 0) {
            server->accept_count--;
        }
        pthread_mutex_lock(&connection->lock);
        connection->queued = 0;
        connection->delivered = 1;
        pthread_mutex_unlock(&connection->lock);
        server->handle_references++;
        pthread_mutex_unlock(&server->lock);
        *out_connection = connection; /* Queue reference becomes handle ref. */
        return 0;
    }

    int error_code;
    if (!server->accepting
        || server->state == NETINS_UDP_V2_TRANSPORT_CLOSING
        || server->state == NETINS_UDP_V2_TRANSPORT_CLOSED)
    {
        error_code = ECANCELED;
    } else if (server->state == NETINS_UDP_V2_TRANSPORT_ERROR) {
        error_code = server->last_error_code != 0
            ? server->last_error_code : EIO;
    } else if (wait_result == ETIMEDOUT) {
        error_code = ETIMEDOUT;
    } else if (wait_result != 0) {
        error_code = wait_result;
    } else if (timeout_ms > 0) {
        error_code = ETIMEDOUT;
    } else {
        error_code = EAGAIN;
    }
    pthread_mutex_unlock(&server->lock);
    errno = error_code;
    return -1;
}

void
netins_udp_v2_transport_server_begin_close(
    netins_udp_v2_transport_server_t *server)
{
    if (server == NULL) {
        return;
    }
    pthread_mutex_lock(&server->lock);
    server->accepting = 0;
    if (server->state != NETINS_UDP_V2_TRANSPORT_ERROR
        && server->state != NETINS_UDP_V2_TRANSPORT_CLOSED)
    {
        server->state = NETINS_UDP_V2_TRANSPORT_CLOSING;
    }
    for (netins_udp_v2_transport_server_connection_t *connection =
             server->connections;
         connection != NULL; connection = connection->all_next)
    {
        pthread_mutex_lock(&connection->lock);
        if (!connection->delivered) {
            connection->close_requested = 1;
        }
        pthread_mutex_unlock(&connection->lock);
    }
    pthread_cond_broadcast(&server->accept_cond);
    pthread_mutex_unlock(&server->lock);
    netins_udp_server_wake(server);
}

void
netins_udp_v2_transport_server_close(
    netins_udp_v2_transport_server_t *server)
{
    if (server == NULL) {
        return;
    }
    netins_udp_v2_transport_server_begin_close(server);
    netins_udp_server_release(server);
}

int
netins_udp_v2_transport_server_connection_get_state(
    netins_udp_v2_transport_server_connection_t *connection)
{
    if (connection == NULL) {
        return NETINS_UDP_V2_TRANSPORT_ERROR;
    }
    pthread_mutex_lock(&connection->lock);
    int state = connection->state;
    pthread_mutex_unlock(&connection->lock);
    return state;
}

int
netins_udp_v2_transport_server_connection_get_last_error_code(
    netins_udp_v2_transport_server_connection_t *connection)
{
    if (connection == NULL) {
        return EINVAL;
    }
    pthread_mutex_lock(&connection->lock);
    int error_code = connection->last_error_code;
    pthread_mutex_unlock(&connection->lock);
    return error_code;
}

int
netins_udp_v2_transport_server_connection_get_last_error_msg(
    netins_udp_v2_transport_server_connection_t *connection,
    char *buffer, size_t buffer_len)
{
    if (connection == NULL || buffer == NULL || buffer_len == 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&connection->lock);
    snprintf(buffer, buffer_len, "%s", connection->last_error_msg);
    pthread_mutex_unlock(&connection->lock);
    return 0;
}

int
netins_udp_v2_transport_server_connection_get_peer_address(
    netins_udp_v2_transport_server_connection_t *connection,
    struct sockaddr_storage *address, socklen_t *address_len)
{
    if (connection == NULL || address == NULL || address_len == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&connection->lock);
    *address = connection->peer_address;
    *address_len = connection->peer_address_len;
    pthread_mutex_unlock(&connection->lock);
    return 0;
}

int
netins_udp_v2_transport_server_connection_get_snapshot(
    netins_udp_v2_transport_server_connection_t *connection,
    netins_udp_v2_transport_snapshot_t *out)
{
    if (connection == NULL || out == NULL || connection->observer == NULL) {
        errno = EINVAL;
        return -1;
    }
    return netins_udp_v2_transport_snapshot_from_observer(
        connection->observer, out);
}

int
netins_udp_v2_transport_server_connection_get_events(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t *out_events)
{
    if (connection == NULL || out_events == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&connection->lock);
    *out_events = connection->request_accept_head != NULL
        && connection->state == NETINS_UDP_V2_TRANSPORT_CONNECTED
        && !connection->close_requested
        ? NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE : 0;
    pthread_mutex_unlock(&connection->lock);
    return 0;
}

void
netins_udp_v2_transport_server_connection_set_notify(
    netins_udp_v2_transport_server_connection_t *connection,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    if (connection == NULL) {
        return;
    }
    pthread_mutex_lock(&connection->lock);
    connection->notify = notify;
    connection->notify_user_data = user_data;
    uint32_t events = connection->request_accept_head != NULL
        && connection->state == NETINS_UDP_V2_TRANSPORT_CONNECTED
        && !connection->close_requested
        ? NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE : 0;
    if (connection->state == NETINS_UDP_V2_TRANSPORT_CONNECTED) {
        events |= NETINS_UDP_V2_EVENT_CONNECTED;
    } else if (connection->state == NETINS_UDP_V2_TRANSPORT_CLOSED) {
        events |= NETINS_UDP_V2_EVENT_CLOSED;
    } else if (connection->state == NETINS_UDP_V2_TRANSPORT_ERROR) {
        events |= NETINS_UDP_V2_EVENT_ERROR;
    }
    pthread_mutex_unlock(&connection->lock);
    if (notify != NULL && events != 0) {
        notify(user_data, events);
    }
}

void
netins_udp_v2_transport_server_connection_request_shutdown(
    netins_udp_v2_transport_server_connection_t *connection,
    uint64_t application_error)
{
    if (connection == NULL) {
        return;
    }
    pthread_mutex_lock(&connection->lock);
    if (connection->state != NETINS_UDP_V2_TRANSPORT_CLOSED
        && connection->state != NETINS_UDP_V2_TRANSPORT_ERROR)
    {
        if (!connection->close_requested || application_error != 0) {
            connection->application_error = application_error;
        }
        connection->close_requested = 1;
    }
    for (netins_udp_v2_transport_server_request_t *request =
             connection->requests;
         request != NULL; request = request->all_next)
    {
        pthread_mutex_lock(&request->lock);
        request->close_requested = 1;
        pthread_cond_broadcast(&request->cond);
        pthread_mutex_unlock(&request->lock);
    }
    pthread_cond_broadcast(&connection->state_cond);
    pthread_mutex_unlock(&connection->lock);
    netins_udp_server_wake(connection->server);
}

void
netins_udp_v2_transport_server_connection_close(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t timeout_ms)
{
    if (connection == NULL) {
        return;
    }
    netins_udp_v2_transport_server_t *server = connection->server;
    netins_udp_v2_transport_server_connection_request_shutdown(connection, 0);

    struct timespec deadline;
    if (timeout_ms > 0
        && netins_udp_server_make_deadline(timeout_ms, &deadline) == 0)
    {
        pthread_mutex_lock(&connection->lock);
        while (connection->state != NETINS_UDP_V2_TRANSPORT_CLOSED
            && connection->state != NETINS_UDP_V2_TRANSPORT_ERROR)
        {
            int wait_result = pthread_cond_timedwait(&connection->state_cond,
                &connection->lock, &deadline);
            if (wait_result == ETIMEDOUT) {
                break;
            }
        }
        pthread_mutex_unlock(&connection->lock);
    }

    netins_udp_server_connection_release(connection);
    netins_udp_server_release(server);
}

int
netins_udp_v2_transport_server_request_accept(
    netins_udp_v2_transport_server_connection_t *connection,
    uint32_t timeout_ms, int wait_forever,
    netins_udp_v2_transport_server_request_t **out_request)
{
    if (connection == NULL || out_request == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_request = NULL;
    struct timespec deadline;
    if (!wait_forever && timeout_ms > 0
        && netins_udp_server_make_deadline(timeout_ms, &deadline) != 0)
    {
        return -1;
    }

    pthread_mutex_lock(&connection->lock);
    int wait_result = 0;
    while (connection->request_accept_head == NULL
        && connection->state == NETINS_UDP_V2_TRANSPORT_CONNECTED
        && !connection->close_requested)
    {
        if (!wait_forever && timeout_ms == 0) {
            break;
        }
        wait_result = wait_forever
            ? pthread_cond_wait(&connection->state_cond, &connection->lock)
            : pthread_cond_timedwait(&connection->state_cond,
                &connection->lock, &deadline);
        if (wait_result != 0) {
            break;
        }
    }

    netins_udp_v2_transport_server_request_t *request =
        connection->state == NETINS_UDP_V2_TRANSPORT_CONNECTED
            && !connection->close_requested
        ? connection->request_accept_head : NULL;
    if (request != NULL) {
        connection->request_accept_head = request->accept_next;
        if (connection->request_accept_head == NULL) {
            connection->request_accept_tail = NULL;
        }
        request->accept_next = NULL;
        if (connection->request_accept_count > 0) {
            connection->request_accept_count--;
        }
        pthread_mutex_lock(&request->lock);
        request->queued = 0;
        request->public_reference = 1;
        pthread_mutex_unlock(&request->lock);
        pthread_mutex_unlock(&connection->lock);
        *out_request = request; /* Queue reference becomes public ref. */
        return 0;
    }

    int error_code;
    if (connection->state == NETINS_UDP_V2_TRANSPORT_ERROR) {
        error_code = connection->last_error_code != 0
            ? connection->last_error_code : EIO;
    } else if (connection->state == NETINS_UDP_V2_TRANSPORT_CLOSING
        || connection->state == NETINS_UDP_V2_TRANSPORT_CLOSED
        || connection->close_requested)
    {
        error_code = ECANCELED;
    } else if (wait_result == ETIMEDOUT) {
        error_code = ETIMEDOUT;
    } else if (wait_result != 0) {
        error_code = wait_result;
    } else if (timeout_ms > 0) {
        error_code = ETIMEDOUT;
    } else {
        error_code = EAGAIN;
    }
    pthread_mutex_unlock(&connection->lock);
    errno = error_code;
    return -1;
}

int
netins_udp_v2_transport_server_request_set_response(
    netins_udp_v2_transport_server_request_t *request,
    const netins_udp_v2_transport_h3_response_config_t *config)
{
    if (request == NULL || config == NULL
        || config->status < 200 || config->status > 599
        || config->header_count > NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS
        || memchr(config->content_type, '\0',
            sizeof(config->content_type)) == NULL
        || config->content_type[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }
    for (uint32_t i = 0; i < config->header_count; ++i) {
        if (memchr(config->headers[i].name, '\0',
                sizeof(config->headers[i].name)) == NULL
            || config->headers[i].name[0] == '\0'
            || memchr(config->headers[i].value, '\0',
                sizeof(config->headers[i].value)) == NULL)
        {
            errno = EINVAL;
            return -1;
        }
    }
    pthread_mutex_lock(&request->lock);
    int error_code = 0;
    if (request->response_configured) {
        error_code = EALREADY;
    } else if (request->public_close_requested
        || request->close_requested || request->close_notified
        || request->xqc_request == NULL || request->connection == NULL)
    {
        error_code = ECANCELED;
    } else {
        request->response = *config;
        request->response_configured = 1;
    }
    if (error_code == 0 && request->connection != NULL) {
        netins_udp_server_wake(request->connection->server);
    }
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);
    if (error_code == 0) {
        netins_udp_server_request_notify(request,
            NETINS_UDP_V2_EVENT_WRITABLE);
    }
    if (error_code != 0) {
        errno = error_code;
        return -1;
    }
    return 0;
}

int64_t
netins_udp_v2_transport_server_request_send(
    netins_udp_v2_transport_server_request_t *request,
    const void *data, size_t length, int fin, uint32_t timeout_ms,
    int wait_forever)
{
    if (request == NULL || (length > 0 && data == NULL)
        || (fin != 0 && fin != 1))
    {
        errno = EINVAL;
        return -1;
    }
    struct timespec deadline;
    if (!wait_forever && timeout_ms > 0
        && netins_udp_server_make_deadline(timeout_ms, &deadline) != 0)
    {
        return -1;
    }

    pthread_mutex_lock(&request->lock);
    int error_code = 0;
    if (!request->response_configured) {
        error_code = EINVAL;
    } else if (request->send_fin_requested || request->send_finished) {
        error_code = EPIPE;
    } else if (request->public_close_requested
        || request->close_requested || request->close_notified
        || request->xqc_request == NULL || request->connection == NULL)
    {
        error_code = ECANCELED;
    } else if (request->stream_error != 0) {
        error_code = EIO;
    }
    if (error_code != 0) {
        pthread_mutex_unlock(&request->lock);
        errno = error_code;
        return -1;
    }

    int length_unknown = request->response.content_length_unknown;
    if (length_unknown) {
        if ((uint64_t)length > UINT64_MAX - request->bytes_accepted) {
            pthread_mutex_unlock(&request->lock);
            errno = EOVERFLOW;
            return -1;
        }
    } else {
        if (request->bytes_accepted > request->response.content_length) {
            pthread_mutex_unlock(&request->lock);
            errno = EIO;
            return -1;
        }
        uint64_t remaining = request->response.content_length
            - request->bytes_accepted;
        if ((uint64_t)length > remaining
            || (fin && (uint64_t)length != remaining)
            || (!fin && remaining != 0
                && (uint64_t)length == remaining))
        {
            pthread_mutex_unlock(&request->lock);
            errno = EINVAL;
            return -1;
        }
    }

    int wait_result = 0;
    while (length > 0
        && request->send_queue_bytes >= NETINS_UDP_SERVER_REQUEST_SEND_MAX
        && request->stream_error == 0
        && !request->public_close_requested
        && !request->close_requested && !request->close_notified)
    {
        if (!wait_forever && timeout_ms == 0) {
            break;
        }
        wait_result = wait_forever
            ? pthread_cond_wait(&request->cond, &request->lock)
            : pthread_cond_timedwait(&request->cond, &request->lock,
                &deadline);
        if (wait_result != 0) {
            break;
        }
    }
    if (request->stream_error != 0) {
        error_code = EIO;
    } else if (request->public_close_requested
        || request->close_requested || request->close_notified
        || request->xqc_request == NULL || request->connection == NULL)
    {
        error_code = ECANCELED;
    } else if (wait_result == ETIMEDOUT) {
        error_code = ETIMEDOUT;
    } else if (wait_result != 0) {
        error_code = wait_result;
    } else if (length > 0
        && request->send_queue_bytes >= NETINS_UDP_SERVER_REQUEST_SEND_MAX)
    {
        error_code = timeout_ms > 0 ? ETIMEDOUT : EAGAIN;
    }
    if (error_code != 0) {
        pthread_mutex_unlock(&request->lock);
        errno = error_code;
        return -1;
    }

    size_t available = NETINS_UDP_SERVER_REQUEST_SEND_MAX
        - request->send_queue_bytes;
    size_t accepted = length < available ? length : available;
    if (accepted > 0) {
        netins_udp_server_buffer_t *buffer =
            netins_udp_server_request_copy_buffer(data, accepted);
        if (buffer == NULL) {
            pthread_mutex_unlock(&request->lock);
            errno = ENOMEM;
            return -1;
        }
        if (request->send_tail == NULL) {
            request->send_head = buffer;
        } else {
            request->send_tail->next = buffer;
        }
        request->send_tail = buffer;
        request->send_queue_bytes += accepted;
        request->bytes_accepted += (uint64_t)accepted;
    }
    if (length_unknown) {
        if (fin && accepted == length) {
            request->send_fin_requested = 1;
        }
    } else if (request->bytes_accepted
        == request->response.content_length)
    {
        request->send_fin_requested = 1;
    }
    if (request->connection != NULL) {
        netins_udp_server_wake(request->connection->server);
    }
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);
    return (int64_t)accepted;
}

int64_t
netins_udp_v2_transport_server_request_recv(
    netins_udp_v2_transport_server_request_t *request,
    void *data, size_t length, int *fin, int peek,
    uint32_t timeout_ms, int wait_forever)
{
    if (request == NULL || fin == NULL
        || (length > 0 && data == NULL))
    {
        errno = EINVAL;
        return -1;
    }
    *fin = 0;
    struct timespec deadline;
    if (!wait_forever && timeout_ms > 0
        && netins_udp_server_make_deadline(timeout_ms, &deadline) != 0)
    {
        return -1;
    }

    pthread_mutex_lock(&request->lock);
    int wait_result = 0;
    while (request->recv_head == NULL && !request->recv_finished
        && request->stream_error == 0
        && !request->public_close_requested && !request->close_requested
        && !request->close_notified)
    {
        if (!wait_forever && timeout_ms == 0) {
            break;
        }
        wait_result = wait_forever
            ? pthread_cond_wait(&request->cond, &request->lock)
            : pthread_cond_timedwait(&request->cond, &request->lock,
                &deadline);
        if (wait_result != 0) {
            break;
        }
    }

    if (request->recv_head == NULL) {
        if (request->recv_finished) {
            *fin = 1;
            if (!peek) {
                request->recv_eof_delivered = 1;
            }
            pthread_mutex_unlock(&request->lock);
            return 0;
        }
        int error_code;
        if (request->stream_error != 0) {
            error_code = EIO;
        } else if (request->public_close_requested
            || request->close_requested || request->close_notified)
        {
            error_code = ECANCELED;
        } else if (wait_result == ETIMEDOUT) {
            error_code = ETIMEDOUT;
        } else if (wait_result != 0) {
            error_code = wait_result;
        } else {
            error_code = timeout_ms > 0 ? ETIMEDOUT : EAGAIN;
        }
        pthread_mutex_unlock(&request->lock);
        errno = error_code;
        return -1;
    }
    if (length == 0) {
        pthread_mutex_unlock(&request->lock);
        errno = ENOBUFS;
        return -1;
    }

    if (peek) {
        size_t copied = 0;
        netins_udp_server_buffer_t *item = request->recv_head;
        size_t item_offset = item->offset;
        while (copied < length && item != NULL) {
            size_t available = item->length - item_offset;
            size_t amount = available < length - copied
                ? available : length - copied;
            memcpy((unsigned char *)data + copied,
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
        *fin = item == NULL && request->recv_finished;
        pthread_mutex_unlock(&request->lock);
        return (int64_t)copied;
    }

    size_t copied = 0;
    while (copied < length && request->recv_head != NULL) {
        netins_udp_server_buffer_t *item = request->recv_head;
        size_t available = item->length - item->offset;
        size_t amount = available < length - copied
            ? available : length - copied;
        memcpy((unsigned char *)data + copied,
            item->data + item->offset, amount);
        item->offset += amount;
        copied += amount;
        request->recv_queue_bytes -= amount;
        request->bytes_delivered += (uint64_t)amount;
        if (item->offset == item->length) {
            request->recv_head = item->next;
            if (request->recv_head == NULL) {
                request->recv_tail = NULL;
            }
            free(item);
        }
    }
    *fin = request->recv_head == NULL && request->recv_finished;
    request->read_pending = !request->recv_finished;
    if (request->connection != NULL) {
        netins_udp_server_wake(request->connection->server);
    }
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);
    return (int64_t)copied;
}

int
netins_udp_v2_transport_server_request_get_info(
    netins_udp_v2_transport_server_request_t *request,
    netins_udp_v2_transport_h3_info_t *out)
{
    if (request == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&request->lock);
    memset(out, 0, sizeof(*out));
    out->stream_id = request->stream_id;
    out->response_status = request->response_configured
        ? request->response.status : 0;
    out->request_headers_ready = request->headers_ready;
    out->response_headers_ready = request->response_headers_sent;
    out->send_finished = request->send_finished;
    out->receive_finished = request->recv_finished;
    out->stream_error = request->stream_error;
    out->bytes_accepted = request->bytes_accepted;
    out->bytes_delivered = request->bytes_delivered;
    snprintf(out->request_method, sizeof(out->request_method), "%s",
        request->method);
    snprintf(out->request_path, sizeof(out->request_path), "%s",
        request->path);
    snprintf(out->request_authority, sizeof(out->request_authority), "%s",
        request->authority);
    snprintf(out->request_content_type,
        sizeof(out->request_content_type), "%s",
        request->request_content_type);
    out->request_content_length_known =
        request->request_content_length_known;
    out->request_content_length = request->request_content_length;
    if (request->response_configured) {
        out->response_content_length_known =
            !request->response.content_length_unknown;
        out->response_content_length = request->response.content_length;
        snprintf(out->response_content_type,
            sizeof(out->response_content_type), "%s",
            request->response.content_type);
    }
    out->peer_header_count = request->peer_header_count;
    memcpy(out->peer_headers, request->peer_headers,
        sizeof(out->peer_headers));
    pthread_mutex_unlock(&request->lock);
    return 0;
}

int
netins_udp_v2_transport_server_request_get_events(
    netins_udp_v2_transport_server_request_t *request,
    uint32_t *out_events)
{
    if (request == NULL || out_events == NULL) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&request->lock);
    uint32_t events = 0;
    if (request->recv_head != NULL
        || (request->recv_finished && !request->recv_eof_delivered))
    {
        events |= NETINS_UDP_V2_EVENT_READABLE;
    }
    if (request->response_configured
        && !request->send_fin_requested && !request->send_finished
        && !request->public_close_requested && !request->close_requested
        && !request->close_notified
        && request->send_queue_bytes < NETINS_UDP_SERVER_REQUEST_SEND_MAX)
    {
        events |= NETINS_UDP_V2_EVENT_WRITABLE;
    }
    *out_events = events;
    pthread_mutex_unlock(&request->lock);
    return 0;
}

void
netins_udp_v2_transport_server_request_set_notify(
    netins_udp_v2_transport_server_request_t *request,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    if (request == NULL) {
        return;
    }
    pthread_mutex_lock(&request->lock);
    request->notify = notify;
    request->notify_user_data = user_data;
    uint32_t events = 0;
    if (request->recv_head != NULL
        || (request->recv_finished && !request->recv_eof_delivered))
    {
        events |= NETINS_UDP_V2_EVENT_READABLE;
    }
    if (request->response_configured
        && !request->send_fin_requested && !request->send_finished
        && !request->public_close_requested && !request->close_requested
        && !request->close_notified
        && request->send_queue_bytes < NETINS_UDP_SERVER_REQUEST_SEND_MAX)
    {
        events |= NETINS_UDP_V2_EVENT_WRITABLE;
    }
    if (request->close_notified) {
        events |= request->stream_error != 0
            ? NETINS_UDP_V2_EVENT_ERROR : NETINS_UDP_V2_EVENT_CLOSED;
    } else if (request->stream_error != 0) {
        events |= NETINS_UDP_V2_EVENT_ERROR;
    }
    pthread_mutex_unlock(&request->lock);
    if (notify != NULL && events != 0) {
        notify(user_data, events);
    }
}

void
netins_udp_v2_transport_server_request_close_async(
    netins_udp_v2_transport_server_request_t *request)
{
    if (request == NULL) {
        return;
    }
    pthread_mutex_lock(&request->lock);
    request->public_close_requested = 1;
    request->read_pending = !request->recv_finished;
    if (request->connection != NULL) {
        netins_udp_server_wake(request->connection->server);
    }
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);
}

void
netins_udp_v2_transport_server_request_close(
    netins_udp_v2_transport_server_request_t *request,
    uint32_t timeout_ms)
{
    if (request == NULL) {
        return;
    }
    struct timespec deadline;
    int have_deadline = timeout_ms > 0
        && netins_udp_server_make_deadline(timeout_ms, &deadline) == 0;

    pthread_mutex_lock(&request->lock);
    request->public_close_requested = 1;
    request->read_pending = !request->recv_finished;
    if (request->connection != NULL) {
        netins_udp_server_wake(request->connection->server);
    }
    pthread_cond_broadcast(&request->cond);
    pthread_mutex_unlock(&request->lock);

    if (have_deadline) {
        pthread_mutex_lock(&request->lock);
        while (!request->close_notified && request->connection != NULL) {
            int wait_result = pthread_cond_timedwait(&request->cond,
                &request->lock, &deadline);
            if (wait_result != 0) {
                break;
            }
        }
        pthread_mutex_unlock(&request->lock);
    }

    pthread_mutex_lock(&request->lock);
    int release_public = request->public_reference;
    request->public_reference = 0;
    pthread_mutex_unlock(&request->lock);
    if (release_public) {
        netins_udp_server_request_release(request);
    }
}
