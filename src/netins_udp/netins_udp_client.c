#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L

#include "netins_udp_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <xquic/xqc_http3.h>

#define NETINS_UDP_CLIENT_DEFAULT_TIMEOUT_MS 20000U
#define NETINS_UDP_CLIENT_DEFAULT_SOCKET_BUF (16 * 1024 * 1024)
#define NETINS_UDP_CLIENT_DEFAULT_CWND       100U
#define NETINS_UDP_CLIENT_RX_BATCH           64
#define NETINS_UDP_CLIENT_REFRESH_US         500000ULL
#define NETINS_UDP_CLIENT_PATH_RETRY_US      200000ULL
#define NETINS_UDP_CLIENT_CLOSE_WAIT_MS      500U

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
    int state;
    int last_error_code;
    char last_error_msg[NETINS_UDP_ERROR_MSG_LEN];

    int wake_pipe[2];
    xqc_engine_t *engine;
    xqc_h3_conn_t *h3_conn;
    xqc_cid_t cid;
    int cid_valid;
    netins_udp_t *status;

    netins_udp_client_path_t paths[NETINS_UDP_MAX_PATHS];
    uint32_t path_count;
    int path_creation_ready;
    uint64_t next_wake_us;
    uint64_t last_refresh_us;
    uint64_t last_path_attempt_us;

    char ciphers[256];
    char groups[128];
};

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

    if (client->status != NULL) {
        (void)netins_udp_mark_error(client->status, error_code, message);
    }
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
    ssize_t written = write(client->wake_pipe[1], &byte, sizeof(byte));
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

static int
netins_udp_client_resolve_peer(netins_udp_client_t *client,
    struct sockaddr_storage *peer_addr, socklen_t *peer_addrlen)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *item = NULL;
    char port_text[16];
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    snprintf(port_text, sizeof(port_text), "%u",
        (unsigned)client->config.server_port);

    int ret = getaddrinfo(client->config.server_address, port_text,
        &hints, &result);
    if (ret != 0) {
        netins_udp_client_set_error(client, EHOSTUNREACH,
            gai_strerror(ret));
        return NETINS_UDP_ERROR;
    }

    for (item = result; item != NULL; item = item->ai_next) {
        if ((item->ai_family == AF_INET || item->ai_family == AF_INET6)
            && item->ai_addrlen <= sizeof(*peer_addr))
        {
            memcpy(peer_addr, item->ai_addr, item->ai_addrlen);
            *peer_addrlen = (socklen_t)item->ai_addrlen;
            freeaddrinfo(result);
            return NETINS_UDP_OK;
        }
    }

    freeaddrinfo(result);
    netins_udp_client_set_error(client, EAFNOSUPPORT,
        "server address has no supported UDP address");
    return NETINS_UDP_ERROR;
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
    if (client->status == NULL || path == NULL || !path->active) {
        return;
    }
    (void)netins_udp_set_path_metadata(client->status, path->path_id,
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
    (void)certs;
    (void)cert_len;
    (void)certs_len;
    (void)conn_user_data;
    return XQC_ERROR;
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
    if (client->status != NULL) {
        (void)netins_udp_update_cid(client->status, new_cid);
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

    if (client->status != NULL) {
        (void)netins_udp_refresh(client->status);
        (void)netins_udp_mark_closed(client->status, conn_error);
    }
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

    if (client->status != NULL) {
        (void)netins_udp_mark_handshake_finished(client->status);
        (void)netins_udp_refresh(client->status);
    }
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
    settings.cong_ctrl_callback = xqc_bbr_cb;
    settings.cc_params.customize_on = 1;
    settings.cc_params.init_cwnd = client->config.initial_cwnd_packets;
    settings.proto_version = XQC_VERSION_V1;
    settings.spurious_loss_detect_on = 1;
    settings.scheduler_callback = xqc_minrtt_scheduler_cb;
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
netins_udp_client_bind_status(netins_udp_client_t *client)
{
    if (netins_udp_bind(client->status, client->engine, &client->cid,
            NETINS_UDP_ROLE_CLIENT) != NETINS_UDP_OK)
    {
        netins_udp_client_set_error(client, EIO,
            "failed to bind status adapter to the QUIC connection");
        return NETINS_UDP_ERROR;
    }
    netins_udp_client_update_path_metadata(client, &client->paths[0]);
    (void)netins_udp_refresh(client->status);
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
    unsigned char packet[65536];
    int processed = 0;
    for (int i = 0; i < NETINS_UDP_CLIENT_RX_BATCH; ++i) {
        struct sockaddr_storage peer_addr;
        socklen_t peer_addrlen = sizeof(peer_addr);
        ssize_t size = recvfrom(path->fd, packet, sizeof(packet), 0,
            (struct sockaddr *)&peer_addr, &peer_addrlen);
        if (size < 0) {
            if (errno == EINTR) {
                --i;
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
            (const struct sockaddr *)&peer_addr, peer_addrlen,
            netins_udp_client_packet_time_us(), client);
        processed++;
        if (!path->active) {
            break;
        }
    }
    return processed;
}

static int
netins_udp_client_poll_timeout_ms(netins_udp_client_t *client,
    uint64_t now_us)
{
    int timeout_ms = 500;
    pthread_mutex_lock(&client->lock);
    uint64_t wake_us = client->next_wake_us;
    int close_requested = client->close_requested && !client->close_sent;
    pthread_mutex_unlock(&client->lock);

    if (close_requested) {
        return 0;
    }
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
    return timeout_ms;
}

static void
netins_udp_client_drive_close(netins_udp_client_t *client)
{
    pthread_mutex_lock(&client->lock);
    int should_close = client->close_requested && !client->close_sent;
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
        if (client->status != NULL) {
            (void)netins_udp_mark_closing(client->status);
        }
        (void)xqc_h3_conn_close(client->engine, &client->cid);
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
netins_udp_client_refresh_status(netins_udp_client_t *client, uint64_t now_us)
{
    if (client->status == NULL
        || now_us - client->last_refresh_us < NETINS_UDP_CLIENT_REFRESH_US)
    {
        return;
    }
    for (uint32_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        netins_udp_client_update_path_metadata(client, &client->paths[i]);
    }
    (void)netins_udp_refresh(client->status);
    client->last_refresh_us = now_us;
}

static void *
netins_udp_client_thread_main(void *argument)
{
    netins_udp_client_t *client = argument;
    while (1) {
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
        for (uint32_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
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
        if (continue_send && client->cid_valid) {
            (void)xqc_conn_continue_send(client->engine, &client->cid);
        }

        now_us = netins_udp_client_now_us();
        netins_udp_client_run_engine_timer(client, now_us);
        netins_udp_client_maybe_create_paths(client, now_us);
        netins_udp_client_refresh_status(client, now_us);
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
        return NETINS_UDP_ERROR;
    }
    *out_client = NULL;
    if (config->struct_size != sizeof(*config)
        || config->api_version != NETINS_UDP_CLIENT_API_VERSION)
    {
        return NETINS_UDP_VERSION_MISMATCH;
    }
    if (config->server_address[0] == '\0' || config->server_port == 0
        || config->interface_count > NETINS_UDP_MAX_PATHS)
    {
        return NETINS_UDP_ERROR;
    }

    netins_udp_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        return NETINS_UDP_ERROR;
    }
    client->config = *config;
    client->config.server_address[NETINS_UDP_SERVER_ADDRESS_LEN - 1] = '\0';
    client->config.server_name[NETINS_UDP_SERVER_NAME_LEN - 1] = '\0';
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

    client->wake_pipe[0] = -1;
    client->wake_pipe[1] = -1;
    client->state = NETINS_UDP_STATE_NEW;
    snprintf(client->ciphers, sizeof(client->ciphers), "%s", XQC_TLS_CIPHERS);
    snprintf(client->groups, sizeof(client->groups), "%s", XQC_TLS_GROUPS);
    if (pthread_mutex_init(&client->lock, NULL) != 0) {
        free(client);
        return NETINS_UDP_ERROR;
    }
    if (pthread_cond_init(&client->state_cond, NULL) != 0) {
        pthread_mutex_destroy(&client->lock);
        free(client);
        return NETINS_UDP_ERROR;
    }
    client->status = netins_udp_new();
    if (client->status == NULL) {
        pthread_cond_destroy(&client->state_cond);
        pthread_mutex_destroy(&client->lock);
        free(client);
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
netins_udp_client_connect(netins_udp_client_t *client)
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
    pthread_mutex_unlock(&client->lock);

    struct sockaddr_storage peer_addr;
    socklen_t peer_addrlen = 0;
    if (netins_udp_client_resolve_peer(client, &peer_addr, &peer_addrlen)
            != NETINS_UDP_OK
        || netins_udp_client_init_wake_pipe(client) != NETINS_UDP_OK)
    {
        goto fail;
    }

    const char *initial_interface = client->config.interface_count > 0
        ? client->config.interface_names[0] : NULL;
    if (netins_udp_client_create_socket(client, &client->paths[0],
            initial_interface, &peer_addr, peer_addrlen) != NETINS_UDP_OK)
    {
        goto fail;
    }
    client->paths[0].path_id = XQC_INITIAL_PATH_ID;
    client->paths[0].active = 1;
    client->path_count = 1;

    if (netins_udp_client_create_engine(client) != NETINS_UDP_OK
        || netins_udp_client_start_connection(client) != NETINS_UDP_OK
        || netins_udp_client_bind_status(client) != NETINS_UDP_OK)
    {
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
    return netins_udp_client_wait_connected(client,
        client->config.connect_timeout_ms);

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
    if (client == NULL || client->status == NULL) {
        return NETINS_UDP_INVALID_CONNECTION_ID;
    }
    netins_udp_info_t info = NETINS_UDP_INFO_INITIALIZER;
    if (netins_udp_get_info(client->status, &info) != NETINS_UDP_OK) {
        return NETINS_UDP_INVALID_CONNECTION_ID;
    }
    return info.connection_id;
}

int
netins_udp_client_get_snapshot(netins_udp_client_t *client,
    netins_udp_snapshot_t *out)
{
    if (client == NULL || client->status == NULL) {
        return NETINS_UDP_ERROR;
    }
    return netins_udp_get_snapshot(client->status, out);
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
netins_udp_client_close(netins_udp_client_t *client)
{
    if (client == NULL) {
        return;
    }

    if (client->thread_started) {
        pthread_mutex_lock(&client->lock);
        client->close_requested = 1;
        pthread_mutex_unlock(&client->lock);
        netins_udp_client_wake(client);

        struct timespec deadline;
        if (netins_udp_client_make_deadline(NETINS_UDP_CLIENT_CLOSE_WAIT_MS,
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

    if (client->status != NULL) {
        (void)netins_udp_refresh(client->status);
        (void)netins_udp_mark_closed(client->status,
            client->last_error_code);
    }
    if (client->engine != NULL) {
        (void)xqc_h3_ctx_destroy(client->engine);
        xqc_engine_destroy(client->engine);
        client->engine = NULL;
    }
    for (uint32_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        netins_udp_client_close_path(&client->paths[i]);
    }
    if (client->wake_pipe[0] >= 0) {
        close(client->wake_pipe[0]);
    }
    if (client->wake_pipe[1] >= 0) {
        close(client->wake_pipe[1]);
    }
    if (client->status != NULL) {
        netins_udp_close(client->status);
        client->status = NULL;
    }
    pthread_cond_destroy(&client->state_cond);
    pthread_mutex_destroy(&client->lock);
    free(client);
}
