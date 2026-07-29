#define _POSIX_C_SOURCE 200809L

#include <xquic/netins_udp.h>

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netins_udp_data_backend.h"
#include "netins_udp_v2_internal.h"
#include "netins_udp_v2_notifier.h"
#include "netins_udp_v2_transport.h"

#define NETINS_UDP_V2_DEFAULT_CONNECT_TIMEOUT_MS 20000U
#define NETINS_UDP_V2_DEFAULT_CLOSE_TIMEOUT_MS    1000U
#define NETINS_UDP_V2_DEFAULT_SOCKET_BUFFER       (16 * 1024 * 1024)
#define NETINS_UDP_V2_DEFAULT_INITIAL_CWND        100U

#define NETINS_UDP_V2_LOGGED_CONNECTED (1U << 0)
#define NETINS_UDP_V2_LOGGED_LISTENING (1U << 1)
#define NETINS_UDP_V2_LOGGED_SHUTDOWN  (1U << 2)
#define NETINS_UDP_V2_LOGGED_CLOSED    (1U << 3)
#define NETINS_UDP_V2_LOGGED_ERROR     (1U << 4)
#define NETINS_UDP_V2_LOGGED_MP_STATUS (1U << 5)

_Static_assert(NETINS_UDP_MAX_PATHS
        == NETINS_UDP_V2_TRANSPORT_MAX_PATHS,
    "public and transport path limits must match");
_Static_assert(NETINS_UDP_ADDRESS_LEN
        == NETINS_UDP_V2_TRANSPORT_HOST_LEN,
    "public and transport host limits must match");
_Static_assert(NETINS_UDP_SERVER_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN,
    "public and transport server-name limits must match");
_Static_assert(NETINS_UDP_INTERFACE_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_INTERFACE_NAME_LEN,
    "public and transport interface-name limits must match");
_Static_assert(NETINS_UDP_FILE_PATH_LEN
        == NETINS_UDP_V2_TRANSPORT_FILE_PATH_LEN,
    "public and transport file-path limits must match");
_Static_assert(NETINS_UDP_EVENT_CONNECTED
        == NETINS_UDP_V2_EVENT_CONNECTED
        && NETINS_UDP_EVENT_ACCEPTABLE
            == NETINS_UDP_V2_EVENT_ACCEPTABLE
        && NETINS_UDP_EVENT_READABLE == NETINS_UDP_V2_EVENT_READABLE
        && NETINS_UDP_EVENT_WRITABLE == NETINS_UDP_V2_EVENT_WRITABLE
        && NETINS_UDP_EVENT_PATH_CHANGED
            == NETINS_UDP_V2_EVENT_PATH_CHANGED
        && NETINS_UDP_EVENT_CLOSED == NETINS_UDP_V2_EVENT_CLOSED
        && NETINS_UDP_EVENT_ERROR == NETINS_UDP_V2_EVENT_ERROR
        && NETINS_UDP_EVENT_H3_REQUEST_ACCEPTABLE
            == NETINS_UDP_V2_EVENT_H3_REQUEST_ACCEPTABLE,
    "public and private notification bits must match");

struct netins_udp_s {
    pthread_mutex_t lock;
    pthread_mutex_t data_lock;
    pthread_cond_t data_cond;
    pthread_cond_t idle_cond;
    size_t active_calls;
    size_t active_logs;
    int closing;
    int endpoint_attempted;

    int requested_type;
    int role;
    int state;
    int app_protocol;
    int nonblock;

    uint32_t connect_timeout_ms;
    uint32_t accept_timeout_ms;
    uint32_t send_timeout_ms;
    uint32_t recv_timeout_ms;
    uint32_t close_timeout_ms;
    uint32_t idle_timeout_ms;
    int kernel_sndbuf;
    int kernel_rcvbuf;
    uint32_t initial_cwnd_packets;
    int scheduler;
    int congestion_control;
    int log_level;
    netins_udp_logger_cb logger;
    uint32_t logged_lifecycle;
    netins_udp_interfaces_t interfaces;
    netins_udp_tls_config_t tls;

    netins_udp_v2_transport_client_t *client_transport;
    netins_udp_v2_transport_server_t *listener_transport;
    netins_udp_v2_transport_server_connection_t *accepted_transport;
    netins_udp_data_backend_t *data_backend;
    int data_backend_creating;
    int data_api_selected;
    int h3_api_selected;
    int last_error_code;
    char last_error_msg[NETINS_UDP_ERROR_MSG_LEN];
    netins_udp_close_reason_t close_reason;
    netins_udp_v2_notifier_t notifier;
};

static pthread_mutex_t g_netins_udp_v2_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_netins_udp_v2_init_refs;
static uint32_t g_netins_udp_v2_handle_count;

static void netins_udp_v2_sync_data_backend_error(netins_udp_t *netins,
    netins_udp_data_backend_t *backend, int fallback_error,
    const char *fallback_message);

static void
netins_udp_v2_log(netins_udp_t *netins, int level, uint32_t once,
    const char *message)
{
    netins_udp_logger_cb logger = NULL;
    pthread_mutex_lock(&netins->lock);
    if (once != 0 && (netins->logged_lifecycle & once) != 0) {
        pthread_mutex_unlock(&netins->lock);
        return;
    }
    if (once != 0) {
        netins->logged_lifecycle |= once;
    }
    if (netins->logger != NULL && level >= netins->log_level) {
        logger = netins->logger;
        netins->active_logs++;
    }
    pthread_mutex_unlock(&netins->lock);

    if (logger != NULL) {
        logger(level, message);
        pthread_mutex_lock(&netins->lock);
        if (netins->active_logs > 0) {
            netins->active_logs--;
        }
        if (netins->active_logs == 0) {
            pthread_cond_broadcast(&netins->idle_cond);
        }
        pthread_mutex_unlock(&netins->lock);
    }
}

static void
netins_udp_v2_log_current_error(netins_udp_t *netins,
    const char *fallback)
{
    char message[NETINS_UDP_ERROR_MSG_LEN + 32];
    pthread_mutex_lock(&netins->lock);
    const char *detail = netins->last_error_msg[0] != '\0'
        ? netins->last_error_msg : fallback;
    snprintf(message, sizeof(message), "NetIns UDP error: %s", detail);
    pthread_mutex_unlock(&netins->lock);
    netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_ERROR,
        NETINS_UDP_V2_LOGGED_ERROR, message);
}

/*
 * This helper is called only after a transport operation has returned. The
 * transport may invoke the generic notify callback while holding its own
 * lock, so querying the observer directly from notify would risk re-entry.
 */
static void
netins_udp_v2_log_connection_summary(netins_udp_t *netins)
{
    pthread_mutex_lock(&netins->lock);
    if (netins->closing
        || (netins->logged_lifecycle & NETINS_UDP_V2_LOGGED_MP_STATUS) != 0)
    {
        pthread_mutex_unlock(&netins->lock);
        return;
    }
    int requested_type = netins->requested_type;
    int role = netins->role;
    netins_udp_v2_transport_client_t *client_transport =
        netins->client_transport;
    netins_udp_v2_transport_server_connection_t *accepted_transport =
        netins->accepted_transport;
    pthread_mutex_unlock(&netins->lock);

    netins_udp_v2_transport_snapshot_t snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    int snapshot_result = -1;
    if (role == NETINS_UDP_ROLE_CLIENT && client_transport != NULL) {
        snapshot_result =
            netins_udp_v2_transport_client_get_snapshot(client_transport,
                &snapshot);
    } else if (role == NETINS_UDP_ROLE_ACCEPTED
        && accepted_transport != NULL)
    {
        snapshot_result =
            netins_udp_v2_transport_server_connection_get_snapshot(
                accepted_transport, &snapshot);
    }
    if (snapshot_result != 0) {
        return;
    }
    if (!snapshot.handshake_finished || snapshot.path_count == 0) {
        return;
    }

    char message[128];
    if (snapshot.multipath_active) {
        snprintf(message, sizeof(message),
            "multipath QUIC active: paths=%u active=%u",
            snapshot.path_count, snapshot.active_path_count);
        netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_INFO,
            NETINS_UDP_V2_LOGGED_MP_STATUS, message);

    } else if (requested_type == NETINS_UDP_TYPE_MPQUIC) {
        netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_WARN,
            NETINS_UDP_V2_LOGGED_MP_STATUS,
            "multipath QUIC is not active; connection is using one path");

    } else {
        netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_DEBUG,
            NETINS_UDP_V2_LOGGED_MP_STATUS, "single-path QUIC active");
    }
}

static void
netins_udp_v2_notify_handle(void *user_data, uint32_t events)
{
    netins_udp_t *netins = user_data;
    if (netins != NULL) {
        netins_udp_v2_notifier_signal(&netins->notifier, events);
        if ((events & NETINS_UDP_V2_EVENT_CONNECTED) != 0) {
            pthread_mutex_lock(&netins->lock);
            int accepted = netins->role == NETINS_UDP_ROLE_ACCEPTED;
            pthread_mutex_unlock(&netins->lock);
            netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_INFO,
                NETINS_UDP_V2_LOGGED_CONNECTED,
                accepted ? "accepted QUIC connection established"
                         : "QUIC connection established");
        }
        if ((events & NETINS_UDP_V2_EVENT_PATH_CHANGED) != 0) {
            netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_WARN, 0,
                "QUIC path availability changed; query snapshot for details");
        }
        if ((events & NETINS_UDP_V2_EVENT_CLOSED) != 0) {
            netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_INFO,
                NETINS_UDP_V2_LOGGED_CLOSED, "QUIC connection closed");
        }
        if ((events & NETINS_UDP_V2_EVENT_ERROR) != 0) {
            netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_ERROR,
                NETINS_UDP_V2_LOGGED_ERROR,
                "QUIC transport reported an asynchronous failure");
        }
    }
}

static void
netins_udp_v2_set_error_locked(netins_udp_t *netins, int error_code,
    const char *message)
{
    netins->last_error_code = error_code;
    if (message == NULL) {
        netins->last_error_msg[0] = '\0';
    } else {
        snprintf(netins->last_error_msg, sizeof(netins->last_error_msg),
            "%s", message);
    }
}

static void
netins_udp_v2_set_close_reason_locked(netins_udp_t *netins, int source,
    int system_error, uint64_t transport_error,
    uint64_t application_error, const char *message)
{
    /*
     * The first terminal cause is the stable public close reason.  A later
     * transport snapshot may add the raw QUIC error, but must not replace a
     * local shutdown, timeout, peer close, or earlier network error.
     */
    if (netins->close_reason.source != NETINS_UDP_CLOSE_NONE) {
        if (transport_error != 0
            && netins->close_reason.transport_error == 0)
        {
            netins->close_reason.transport_error = transport_error;
        }
        return;
    }
    memset(&netins->close_reason, 0, sizeof(netins->close_reason));
    netins->close_reason.struct_size = sizeof(netins->close_reason);
    netins->close_reason.api_version = NETINS_UDP_API_VERSION;
    netins->close_reason.source = source;
    netins->close_reason.system_error = system_error;
    netins->close_reason.transport_error = transport_error;
    netins->close_reason.application_error = application_error;
    if (message != NULL) {
        snprintf(netins->close_reason.message,
            sizeof(netins->close_reason.message), "%s", message);
    }
}

static int
netins_udp_v2_error_close_source(int error_code, int state)
{
    if (error_code == ETIMEDOUT) {
        return state == NETINS_UDP_STATE_CONNECTING
            ? NETINS_UDP_CLOSE_HANDSHAKE_TIMEOUT
            : NETINS_UDP_CLOSE_IDLE_TIMEOUT;
    }
    switch (error_code) {
    case ECONNABORTED:
    case ECONNREFUSED:
    case ECONNRESET:
    case EHOSTDOWN:
    case EHOSTUNREACH:
    case ENETDOWN:
    case ENETRESET:
    case ENETUNREACH:
        return NETINS_UDP_CLOSE_NETWORK_ERROR;
    default:
        return NETINS_UDP_CLOSE_TRANSPORT;
    }
}

static int
netins_udp_v2_fail(netins_udp_t *netins, int error_code,
    const char *message)
{
    pthread_mutex_lock(&netins->lock);
    netins_udp_v2_set_error_locked(netins, error_code, message);
    pthread_mutex_unlock(&netins->lock);
    errno = error_code;
    return NETINS_UDP_ERROR;
}

static int
netins_udp_v2_begin_call(netins_udp_t *netins)
{
    if (netins == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    if (netins->closing) {
        netins_udp_v2_set_error_locked(netins, EBADF,
            "connection handle is closing");
        pthread_mutex_unlock(&netins->lock);
        errno = EBADF;
        return NETINS_UDP_ERROR;
    }
    netins->active_calls++;
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

static void
netins_udp_v2_end_call(netins_udp_t *netins)
{
    pthread_mutex_lock(&netins->lock);
    if (netins->active_calls > 0) {
        netins->active_calls--;
    }
    if (netins->active_calls == 0) {
        pthread_cond_broadcast(&netins->idle_cond);
    }
    pthread_mutex_unlock(&netins->lock);
}

static int
netins_udp_v2_scalar_copy(const void *value, size_t value_len,
    void *out, size_t expected)
{
    if (value == NULL || out == NULL || value_len != expected) {
        return 0;
    }
    /*
     * The public option argument is a void pointer and therefore does not
     * promise natural alignment. Copy before interpreting it so packed
     * protocol adapters remain valid on strict-alignment targets.
     */
    memcpy(out, value, expected);
    return 1;
}

static int
netins_udp_v2_fixed_string_valid(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static int
netins_udp_v2_host_text_valid(const char *value, size_t capacity)
{
    if (!netins_udp_v2_fixed_string_valid(value, capacity)) {
        return 0;
    }
    for (const unsigned char *cursor = (const unsigned char *)value;
         *cursor != '\0'; ++cursor)
    {
        if (*cursor <= 0x20 || *cursor == 0x7f) {
            return 0;
        }
    }
    return 1;
}

static int
netins_udp_v2_runtime_option(int option, int state)
{
    return option == NETINS_UDP_OPT_NONBLOCK
        || option == NETINS_UDP_OPT_SEND_TIMEOUT_MS
        || option == NETINS_UDP_OPT_RECV_TIMEOUT_MS
        || option == NETINS_UDP_OPT_CLOSE_TIMEOUT_MS
        || option == NETINS_UDP_OPT_LOG_LEVEL
        || (option == NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS
            && state == NETINS_UDP_STATE_LISTENING);
}

static int
netins_udp_v2_set_option_error_locked(netins_udp_t *netins,
    int error_code, const char *message)
{
    netins_udp_v2_set_error_locked(netins, error_code, message);
    errno = error_code;
    return NETINS_UDP_ERROR;
}

static int
netins_udp_v2_transport_state_to_public(int state)
{
    switch (state) {
    case NETINS_UDP_V2_TRANSPORT_NEW:
        return NETINS_UDP_STATE_NEW;
    case NETINS_UDP_V2_TRANSPORT_CONNECTING:
        return NETINS_UDP_STATE_CONNECTING;
    case NETINS_UDP_V2_TRANSPORT_LISTENING:
        return NETINS_UDP_STATE_LISTENING;
    case NETINS_UDP_V2_TRANSPORT_CONNECTED:
        return NETINS_UDP_STATE_CONNECTED;
    case NETINS_UDP_V2_TRANSPORT_CLOSING:
        return NETINS_UDP_STATE_CLOSING;
    case NETINS_UDP_V2_TRANSPORT_CLOSED:
        return NETINS_UDP_STATE_CLOSED;
    case NETINS_UDP_V2_TRANSPORT_ERROR:
    default:
        return NETINS_UDP_STATE_ERROR;
    }
}

static void
netins_udp_v2_sync_transport_error(netins_udp_t *netins,
    netins_udp_v2_transport_client_t *transport)
{
    int error_code =
        netins_udp_v2_transport_client_get_last_error_code(transport);
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    (void)netins_udp_v2_transport_client_get_last_error_msg(transport,
        message, sizeof(message));

    pthread_mutex_lock(&netins->lock);
    netins_udp_v2_set_error_locked(netins,
        error_code != 0 ? error_code : EIO,
        message[0] != '\0' ? message : "underlying QUIC client failed");
    netins_udp_v2_set_close_reason_locked(netins,
        netins_udp_v2_error_close_source(
            error_code != 0 ? error_code : EIO, netins->state),
        error_code != 0 ? error_code : EIO, 0, 0,
        message[0] != '\0' ? message : "underlying QUIC client failed");
    pthread_mutex_unlock(&netins->lock);
}

static void
netins_udp_v2_sync_listener_error(netins_udp_t *netins,
    netins_udp_v2_transport_server_t *transport)
{
    int error_code =
        netins_udp_v2_transport_server_get_last_error_code(transport);
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    (void)netins_udp_v2_transport_server_get_last_error_msg(transport,
        message, sizeof(message));

    pthread_mutex_lock(&netins->lock);
    netins_udp_v2_set_error_locked(netins,
        error_code != 0 ? error_code : EIO,
        message[0] != '\0' ? message : "underlying QUIC server failed");
    netins_udp_v2_set_close_reason_locked(netins,
        NETINS_UDP_CLOSE_NETWORK_ERROR,
        error_code != 0 ? error_code : EIO, 0, 0,
        message[0] != '\0' ? message : "underlying QUIC server failed");
    pthread_mutex_unlock(&netins->lock);
}

static void
netins_udp_v2_sync_accepted_error(netins_udp_t *netins,
    netins_udp_v2_transport_server_connection_t *transport)
{
    int error_code =
        netins_udp_v2_transport_server_connection_get_last_error_code(
            transport);
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    (void)netins_udp_v2_transport_server_connection_get_last_error_msg(
        transport, message, sizeof(message));

    pthread_mutex_lock(&netins->lock);
    netins_udp_v2_set_error_locked(netins,
        error_code != 0 ? error_code : EIO,
        message[0] != '\0' ? message
            : "accepted QUIC connection failed");
    netins_udp_v2_set_close_reason_locked(netins,
        netins_udp_v2_error_close_source(
            error_code != 0 ? error_code : EIO, netins->state),
        error_code != 0 ? error_code : EIO, 0, 0,
        message[0] != '\0' ? message
            : "accepted QUIC connection failed");
    pthread_mutex_unlock(&netins->lock);
}

void
netins_udp_tls_config_init(netins_udp_tls_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->api_version = NETINS_UDP_API_VERSION;
    config->verify_peer = 1;
}

int
netins_udp_init(void)
{
    pthread_mutex_lock(&g_netins_udp_v2_lock);
    if (g_netins_udp_v2_init_refs == UINT32_MAX) {
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = EOVERFLOW;
        return NETINS_UDP_ERROR;
    }
    g_netins_udp_v2_init_refs++;
    pthread_mutex_unlock(&g_netins_udp_v2_lock);
    return NETINS_UDP_OK;
}

int
netins_udp_cleanup(void)
{
    pthread_mutex_lock(&g_netins_udp_v2_lock);
    if (g_netins_udp_v2_init_refs == 0) {
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = EPERM;
        return NETINS_UDP_ERROR;
    }
    if (g_netins_udp_v2_init_refs == 1
        && (g_netins_udp_v2_handle_count != 0
            || netins_udp_v2_transport_has_pending_resolvers()))
    {
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = EBUSY;
        return NETINS_UDP_ERROR;
    }
    g_netins_udp_v2_init_refs--;
    pthread_mutex_unlock(&g_netins_udp_v2_lock);
    return NETINS_UDP_OK;
}

netins_udp_t *
netins_udp_new(int type)
{
    if (type != NETINS_UDP_TYPE_QUIC && type != NETINS_UDP_TYPE_MPQUIC) {
        errno = EINVAL;
        return NULL;
    }

    pthread_mutex_lock(&g_netins_udp_v2_lock);
    if (g_netins_udp_v2_init_refs == 0) {
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = EPERM;
        return NULL;
    }
    if (g_netins_udp_v2_handle_count == UINT32_MAX) {
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = EOVERFLOW;
        return NULL;
    }

    netins_udp_t *netins = calloc(1, sizeof(*netins));
    if (netins == NULL) {
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = ENOMEM;
        return NULL;
    }
    int init_error = pthread_mutex_init(&netins->lock, NULL);
    if (init_error != 0) {
        free(netins);
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = init_error;
        return NULL;
    }
    init_error = pthread_mutex_init(&netins->data_lock, NULL);
    if (init_error != 0) {
        pthread_mutex_destroy(&netins->lock);
        free(netins);
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = init_error;
        return NULL;
    }
    init_error = pthread_cond_init(&netins->idle_cond, NULL);
    if (init_error != 0) {
        pthread_mutex_destroy(&netins->data_lock);
        pthread_mutex_destroy(&netins->lock);
        free(netins);
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = init_error;
        return NULL;
    }
    init_error = pthread_cond_init(&netins->data_cond, NULL);
    if (init_error != 0) {
        pthread_cond_destroy(&netins->idle_cond);
        pthread_mutex_destroy(&netins->data_lock);
        pthread_mutex_destroy(&netins->lock);
        free(netins);
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = init_error;
        return NULL;
    }
    if (netins_udp_v2_notifier_init(&netins->notifier) != 0) {
        int error_code = errno != 0 ? errno : EIO;
        pthread_cond_destroy(&netins->data_cond);
        pthread_cond_destroy(&netins->idle_cond);
        pthread_mutex_destroy(&netins->data_lock);
        pthread_mutex_destroy(&netins->lock);
        free(netins);
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = error_code;
        return NULL;
    }

    netins->requested_type = type;
    netins->role = NETINS_UDP_ROLE_NONE;
    netins->state = NETINS_UDP_STATE_NEW;
    netins->app_protocol = NETINS_UDP_APP_HTTP3;
    netins->connect_timeout_ms =
        NETINS_UDP_V2_DEFAULT_CONNECT_TIMEOUT_MS;
    netins->close_timeout_ms = NETINS_UDP_V2_DEFAULT_CLOSE_TIMEOUT_MS;
    netins->kernel_sndbuf = NETINS_UDP_V2_DEFAULT_SOCKET_BUFFER;
    netins->kernel_rcvbuf = NETINS_UDP_V2_DEFAULT_SOCKET_BUFFER;
    netins->initial_cwnd_packets = NETINS_UDP_V2_DEFAULT_INITIAL_CWND;
    netins->log_level = NETINS_UDP_LOG_LEVEL_INFO;
    netins->interfaces = (netins_udp_interfaces_t)
        NETINS_UDP_INTERFACES_INITIALIZER;
    netins_udp_tls_config_init(&netins->tls);
    netins->close_reason = (netins_udp_close_reason_t)
        NETINS_UDP_CLOSE_REASON_INITIALIZER;

    g_netins_udp_v2_handle_count++;
    pthread_mutex_unlock(&g_netins_udp_v2_lock);
    return netins;
}

int
netins_udp_set_logger(netins_udp_t *netins, netins_udp_logger_cb logger)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    /*
     * Stop new callback entries before waiting. This gives callers a stable
     * handoff point at which the previous callback and its application-owned
     * state are no longer in use.
     */
    netins->logger = NULL;
    while (netins->active_logs != 0) {
        pthread_cond_wait(&netins->idle_cond, &netins->lock);
    }
    netins->logger = logger;
    netins_udp_v2_set_error_locked(netins, 0, NULL);
    pthread_mutex_unlock(&netins->lock);

    netins_udp_v2_end_call(netins);
    return NETINS_UDP_OK;
}

int
netins_udp_set_opt(netins_udp_t *netins, int option,
    const void *value, size_t value_len)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }

    int result = NETINS_UDP_OK;
    netins_udp_v2_transport_client_t *transport_to_update = NULL;
    uint32_t close_timeout_to_update = 0;
    pthread_mutex_lock(&netins->lock);
    if (netins->closing
        || netins->state == NETINS_UDP_STATE_CLOSED)
    {
        result = netins_udp_v2_set_option_error_locked(netins, EBADF,
            "options cannot be changed on a terminal connection");
        goto done;
    }
    if (netins->state != NETINS_UDP_STATE_NEW
        && !netins_udp_v2_runtime_option(option, netins->state))
    {
        result = netins_udp_v2_set_option_error_locked(netins, EBUSY,
            "option is immutable after connect/listen begins");
        goto done;
    }

    switch (option) {
    case NETINS_UDP_OPT_NONBLOCK: {
        int enabled = 0;
        if (!netins_udp_v2_scalar_copy(value, value_len, &enabled,
                sizeof(enabled)))
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "NONBLOCK requires an int");
            break;
        }
        if (enabled != 0 && enabled != 1) {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "NONBLOCK must be 0 or 1");
            break;
        }
        netins->nonblock = enabled;
        break;
    }
    case NETINS_UDP_OPT_CONNECT_TIMEOUT_MS:
    case NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS:
    case NETINS_UDP_OPT_SEND_TIMEOUT_MS:
    case NETINS_UDP_OPT_RECV_TIMEOUT_MS:
    case NETINS_UDP_OPT_CLOSE_TIMEOUT_MS:
    case NETINS_UDP_OPT_IDLE_TIMEOUT_MS: {
        uint32_t timeout = 0;
        if (!netins_udp_v2_scalar_copy(value, value_len, &timeout,
                sizeof(timeout)))
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "timeout option requires uint32_t");
            break;
        }
        if ((option == NETINS_UDP_OPT_CONNECT_TIMEOUT_MS
                || option == NETINS_UDP_OPT_CLOSE_TIMEOUT_MS)
            && timeout == 0)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "connect and close timeouts must be nonzero");
            break;
        }
        if (option == NETINS_UDP_OPT_CONNECT_TIMEOUT_MS) {
            netins->connect_timeout_ms = timeout;
        } else if (option == NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS) {
            netins->accept_timeout_ms = timeout;
        } else if (option == NETINS_UDP_OPT_SEND_TIMEOUT_MS) {
            netins->send_timeout_ms = timeout;
        } else if (option == NETINS_UDP_OPT_RECV_TIMEOUT_MS) {
            netins->recv_timeout_ms = timeout;
        } else if (option == NETINS_UDP_OPT_CLOSE_TIMEOUT_MS) {
            netins->close_timeout_ms = timeout;
            transport_to_update = netins->client_transport;
            close_timeout_to_update = timeout;
        } else {
            netins->idle_timeout_ms = timeout;
        }
        break;
    }
    case NETINS_UDP_OPT_KERNEL_SNDBUF:
    case NETINS_UDP_OPT_KERNEL_RCVBUF: {
        int socket_buffer = 0;
        if (!netins_udp_v2_scalar_copy(value, value_len, &socket_buffer,
                sizeof(socket_buffer))
            || socket_buffer <= 0)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "kernel socket buffer requires a positive int");
            break;
        }
        if (option == NETINS_UDP_OPT_KERNEL_SNDBUF) {
            netins->kernel_sndbuf = socket_buffer;
        } else {
            netins->kernel_rcvbuf = socket_buffer;
        }
        break;
    }
    case NETINS_UDP_OPT_INTERFACES: {
        netins_udp_interfaces_t interfaces;
        if (!netins_udp_v2_scalar_copy(value, value_len, &interfaces,
                sizeof(interfaces)))
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "INTERFACES requires netins_udp_interfaces_t");
            break;
        }
        if (interfaces.struct_size != sizeof(interfaces)
            || interfaces.api_version != NETINS_UDP_API_VERSION
            || interfaces.interface_count > NETINS_UDP_MAX_PATHS)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EPROTO,
                "INTERFACES structure version or count is invalid");
            break;
        }
        for (uint32_t i = 0; i < interfaces.interface_count; ++i) {
            if (!netins_udp_v2_fixed_string_valid(interfaces.names[i],
                    sizeof(interfaces.names[i]))
                || interfaces.names[i][0] == '\0')
            {
                result = netins_udp_v2_set_option_error_locked(netins,
                    EINVAL, "interface names must be nonempty strings");
                break;
            }
        }
        if (result == NETINS_UDP_OK) {
            netins->interfaces = interfaces;
        }
        break;
    }
    case NETINS_UDP_OPT_APP_PROTOCOL: {
        int protocol = 0;
        if (!netins_udp_v2_scalar_copy(value, value_len, &protocol,
                sizeof(protocol)))
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "APP_PROTOCOL requires an int");
            break;
        }
        if (protocol != NETINS_UDP_APP_HTTP3)
        {
            result = netins_udp_v2_set_option_error_locked(netins,
                EOPNOTSUPP,
                "the NetIns UDP API supports HTTP/3 only");
            break;
        }
        netins->app_protocol = protocol;
        break;
    }
    case NETINS_UDP_OPT_TLS: {
        netins_udp_tls_config_t tls;
        if (!netins_udp_v2_scalar_copy(value, value_len, &tls,
                sizeof(tls)))
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "TLS requires netins_udp_tls_config_t");
            break;
        }
        if (tls.struct_size != sizeof(tls)
            || tls.api_version != NETINS_UDP_API_VERSION
            || (tls.verify_peer != 0 && tls.verify_peer != 1)
            || (tls.require_client_certificate != 0
                && tls.require_client_certificate != 1)
            || !netins_udp_v2_fixed_string_valid(tls.server_name,
                sizeof(tls.server_name))
            || !netins_udp_v2_host_text_valid(tls.server_name,
                sizeof(tls.server_name))
            || !netins_udp_v2_fixed_string_valid(tls.certificate_file,
                sizeof(tls.certificate_file))
            || !netins_udp_v2_fixed_string_valid(tls.private_key_file,
                sizeof(tls.private_key_file))
            || !netins_udp_v2_fixed_string_valid(tls.ca_file,
                sizeof(tls.ca_file)))
        {
            result = netins_udp_v2_set_option_error_locked(netins, EPROTO,
                "TLS structure is invalid");
            break;
        }
        netins->tls = tls;
        break;
    }
    case NETINS_UDP_OPT_INITIAL_CWND_PACKETS: {
        uint32_t initial_cwnd = 0;
        if (!netins_udp_v2_scalar_copy(value, value_len, &initial_cwnd,
                sizeof(initial_cwnd))
            || initial_cwnd == 0)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "INITIAL_CWND_PACKETS requires a nonzero uint32_t");
            break;
        }
        netins->initial_cwnd_packets = initial_cwnd;
        break;
    }
    case NETINS_UDP_OPT_SCHEDULER: {
        int scheduler = 0;
        if (!netins_udp_v2_scalar_copy(value, value_len, &scheduler,
                sizeof(scheduler))
            || scheduler < NETINS_UDP_SCHEDULER_DEFAULT
            || scheduler > NETINS_UDP_SCHEDULER_FASTONLY)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "unknown scheduler");
            break;
        }
        netins->scheduler = scheduler;
        break;
    }
    case NETINS_UDP_OPT_CONGESTION_CONTROL: {
        int congestion_control = 0;
        if (!netins_udp_v2_scalar_copy(value, value_len,
                &congestion_control, sizeof(congestion_control))
            || congestion_control < NETINS_UDP_CC_DEFAULT
            || congestion_control > NETINS_UDP_CC_RENO)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "unknown congestion controller");
            break;
        }
        netins->congestion_control = congestion_control;
        break;
    }
    case NETINS_UDP_OPT_LOG_LEVEL: {
        int log_level = 0;
        if (!netins_udp_v2_scalar_copy(value, value_len, &log_level,
                sizeof(log_level))
            || log_level < NETINS_UDP_LOG_LEVEL_DEBUG
            || log_level > NETINS_UDP_LOG_LEVEL_ERROR)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "LOG_LEVEL requires a valid netins_udp_log_level_t value");
            break;
        }
        netins->log_level = log_level;
        break;
    }
    default:
        result = netins_udp_v2_set_option_error_locked(netins,
            EOPNOTSUPP, "unknown option");
        break;
    }

    if (result == NETINS_UDP_OK) {
        netins_udp_v2_set_error_locked(netins, 0, NULL);
    }

done:
    pthread_mutex_unlock(&netins->lock);
    if (result == NETINS_UDP_OK && transport_to_update != NULL) {
        netins_udp_v2_transport_client_set_close_timeout(
            transport_to_update, close_timeout_to_update);
    }
    netins_udp_v2_end_call(netins);
    return result;
}

static int
netins_udp_v2_copy_option(netins_udp_t *netins, const void *source,
    size_t required, void *value, size_t *value_len)
{
    if (value_len == NULL) {
        return netins_udp_v2_fail(netins, EINVAL,
            "value_len must not be NULL");
    }
    if (value == NULL) {
        *value_len = required;
        return NETINS_UDP_OK;
    }
    if (*value_len < required) {
        *value_len = required;
        return netins_udp_v2_fail(netins, ENOBUFS,
            "option output buffer is too small");
    }
    memcpy(value, source, required);
    *value_len = required;
    return NETINS_UDP_OK;
}

int
netins_udp_get_opt(netins_udp_t *netins, int option,
    void *value, size_t *value_len)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }

    int int_value = 0;
    uint32_t uint32_value = 0;
    const void *source = NULL;
    size_t required = 0;

    pthread_mutex_lock(&netins->lock);
    switch (option) {
    case NETINS_UDP_OPT_NONBLOCK:
        int_value = netins->nonblock;
        source = &int_value;
        required = sizeof(int_value);
        break;
    case NETINS_UDP_OPT_CONNECT_TIMEOUT_MS:
        uint32_value = netins->connect_timeout_ms;
        source = &uint32_value;
        required = sizeof(uint32_value);
        break;
    case NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS:
        uint32_value = netins->accept_timeout_ms;
        source = &uint32_value;
        required = sizeof(uint32_value);
        break;
    case NETINS_UDP_OPT_SEND_TIMEOUT_MS:
        uint32_value = netins->send_timeout_ms;
        source = &uint32_value;
        required = sizeof(uint32_value);
        break;
    case NETINS_UDP_OPT_RECV_TIMEOUT_MS:
        uint32_value = netins->recv_timeout_ms;
        source = &uint32_value;
        required = sizeof(uint32_value);
        break;
    case NETINS_UDP_OPT_CLOSE_TIMEOUT_MS:
        uint32_value = netins->close_timeout_ms;
        source = &uint32_value;
        required = sizeof(uint32_value);
        break;
    case NETINS_UDP_OPT_IDLE_TIMEOUT_MS:
        uint32_value = netins->idle_timeout_ms;
        source = &uint32_value;
        required = sizeof(uint32_value);
        break;
    case NETINS_UDP_OPT_KERNEL_SNDBUF:
        int_value = netins->kernel_sndbuf;
        source = &int_value;
        required = sizeof(int_value);
        break;
    case NETINS_UDP_OPT_KERNEL_RCVBUF:
        int_value = netins->kernel_rcvbuf;
        source = &int_value;
        required = sizeof(int_value);
        break;
    case NETINS_UDP_OPT_INTERFACES:
        source = &netins->interfaces;
        required = sizeof(netins->interfaces);
        break;
    case NETINS_UDP_OPT_APP_PROTOCOL:
        int_value = netins->app_protocol;
        source = &int_value;
        required = sizeof(int_value);
        break;
    case NETINS_UDP_OPT_TLS:
        source = &netins->tls;
        required = sizeof(netins->tls);
        break;
    case NETINS_UDP_OPT_INITIAL_CWND_PACKETS:
        uint32_value = netins->initial_cwnd_packets;
        source = &uint32_value;
        required = sizeof(uint32_value);
        break;
    case NETINS_UDP_OPT_SCHEDULER:
        int_value = netins->scheduler;
        source = &int_value;
        required = sizeof(int_value);
        break;
    case NETINS_UDP_OPT_CONGESTION_CONTROL:
        int_value = netins->congestion_control;
        source = &int_value;
        required = sizeof(int_value);
        break;
    case NETINS_UDP_OPT_LOG_LEVEL:
        int_value = netins->log_level;
        source = &int_value;
        required = sizeof(int_value);
        break;
    default: {
        pthread_mutex_unlock(&netins->lock);
        int unknown_result = netins_udp_v2_fail(netins, EOPNOTSUPP,
            "unknown option");
        netins_udp_v2_end_call(netins);
        return unknown_result;
    }
    }

    unsigned char local[sizeof(netins_udp_tls_config_t)];
    if (required > sizeof(local)) {
        pthread_mutex_unlock(&netins->lock);
        int overflow_result = netins_udp_v2_fail(netins, EOVERFLOW,
            "internal option buffer is too small");
        netins_udp_v2_end_call(netins);
        return overflow_result;
    }
    memcpy(local, source, required);
    pthread_mutex_unlock(&netins->lock);

    int result = netins_udp_v2_copy_option(netins, local, required,
        value, value_len);
    netins_udp_v2_end_call(netins);
    return result;
}

static int
netins_udp_v2_prepare_transport_config(netins_udp_t *netins,
    const char *host, int port, netins_udp_v2_transport_config_t *config)
{
    if (host == NULL || host[0] == '\0' || port <= 0 || port > 65535) {
        return netins_udp_v2_fail(netins, EINVAL,
            "connect requires a host and a port in [1, 65535]");
    }
    size_t host_len = strnlen(host, NETINS_UDP_V2_TRANSPORT_HOST_LEN);
    if (host_len >= NETINS_UDP_V2_TRANSPORT_HOST_LEN) {
        return netins_udp_v2_fail(netins, ENAMETOOLONG,
            "host is too long for the current transport backend");
    }
    if (!netins_udp_v2_host_text_valid(host, host_len + 1)) {
        return netins_udp_v2_fail(netins, EINVAL,
            "host contains whitespace or control characters");
    }

    pthread_mutex_lock(&netins->lock);
    if (netins->app_protocol != NETINS_UDP_APP_HTTP3) {
        pthread_mutex_unlock(&netins->lock);
        return netins_udp_v2_fail(netins, EOPNOTSUPP,
            "the NetIns UDP API supports only APP_PROTOCOL=HTTP3");
    }
    if (netins->tls.require_client_certificate
        || netins->tls.certificate_file[0] != '\0'
        || netins->tls.private_key_file[0] != '\0')
    {
        pthread_mutex_unlock(&netins->lock);
        return netins_udp_v2_fail(netins, EOPNOTSUPP,
            "custom client certificates are not supported by the current backend");
    }

    memset(config, 0, sizeof(*config));
    memcpy(config->host, host, host_len + 1);
    config->port = (uint16_t)port;
    const char *server_name = netins->tls.server_name[0] != '\0'
        ? netins->tls.server_name : host;
    snprintf(config->server_name, sizeof(config->server_name), "%s",
        server_name);
    snprintf(config->ca_file, sizeof(config->ca_file), "%s",
        netins->tls.ca_file);
    config->interface_count = netins->interfaces.interface_count;
    for (uint32_t i = 0; i < config->interface_count; ++i) {
        snprintf(config->interface_names[i],
            sizeof(config->interface_names[i]), "%s",
            netins->interfaces.names[i]);
    }
    config->enable_multipath =
        netins->requested_type == NETINS_UDP_TYPE_MPQUIC;
    config->connect_timeout_ms = netins->connect_timeout_ms;
    config->idle_timeout_ms = netins->idle_timeout_ms;
    config->close_timeout_ms = netins->close_timeout_ms;
    config->kernel_sndbuf = netins->kernel_sndbuf;
    config->kernel_rcvbuf = netins->kernel_rcvbuf;
    config->initial_cwnd_packets = netins->initial_cwnd_packets;
    config->verify_peer = netins->tls.verify_peer;
    config->scheduler = netins->scheduler;
    config->congestion_control = netins->congestion_control;
    config->notify = netins_udp_v2_notify_handle;
    config->notify_user_data = netins;
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

int
netins_udp_connect(netins_udp_t *netins, const char *host, int port)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    if (netins->state != NETINS_UDP_STATE_NEW
        || netins->endpoint_attempted)
    {
        netins_udp_v2_set_error_locked(netins, EALREADY,
            "connect can only be called once");
        pthread_mutex_unlock(&netins->lock);
        errno = EALREADY;
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }
    netins->endpoint_attempted = 1;
    netins->role = NETINS_UDP_ROLE_CLIENT;
    netins->state = NETINS_UDP_STATE_CONNECTING;
    int nonblock = netins->nonblock;
    uint32_t connect_timeout_ms = netins->connect_timeout_ms;
    pthread_mutex_unlock(&netins->lock);
    netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_DEBUG, 0,
        "QUIC client connection startup requested");

    netins_udp_v2_transport_config_t config;
    if (netins_udp_v2_prepare_transport_config(netins, host, port,
            &config) != NETINS_UDP_OK)
    {
        pthread_mutex_lock(&netins->lock);
        if (!netins->closing) {
            netins->state = NETINS_UDP_STATE_ERROR;
        }
        pthread_mutex_unlock(&netins->lock);
        netins_udp_v2_log_current_error(netins,
            "client transport configuration failed");
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }

    netins_udp_v2_transport_client_t *transport = NULL;
    if (netins_udp_v2_transport_client_create(&config, &transport) != 0) {
        int error_code = errno != 0 ? errno : EIO;
        pthread_mutex_lock(&netins->lock);
        if (!netins->closing) {
            netins->state = NETINS_UDP_STATE_ERROR;
        }
        netins_udp_v2_set_error_locked(netins, error_code,
            "failed to create the underlying QUIC client");
        pthread_mutex_unlock(&netins->lock);
        errno = error_code;
        netins_udp_v2_log_current_error(netins,
            "failed to create the underlying QUIC client");
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    netins->client_transport = transport;
    int closing = netins->closing;
    uint32_t current_close_timeout_ms = netins->close_timeout_ms;
    pthread_cond_broadcast(&netins->idle_cond);
    pthread_mutex_unlock(&netins->lock);
    netins_udp_v2_transport_client_set_close_timeout(transport,
        current_close_timeout_ms);
    if (closing) {
        netins_udp_v2_transport_client_request_close(transport);
        (void)netins_udp_v2_fail(netins, ECANCELED,
            "connect was canceled by close");
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }

    if (netins_udp_v2_transport_client_start(transport) != 0) {
        int error_code = errno != 0 ? errno : EIO;
        netins_udp_v2_sync_transport_error(netins, transport);
        pthread_mutex_lock(&netins->lock);
        if (!netins->closing) {
            netins->state = NETINS_UDP_STATE_ERROR;
        }
        pthread_mutex_unlock(&netins->lock);
        errno = error_code;
        netins_udp_v2_log_current_error(netins,
            "failed to start the underlying QUIC client");
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }

    if (nonblock) {
        int transport_state =
            netins_udp_v2_transport_client_get_state(transport);
        if (transport_state == NETINS_UDP_V2_TRANSPORT_CONNECTED) {
            pthread_mutex_lock(&netins->lock);
            if (!netins->closing) {
                netins->state = NETINS_UDP_STATE_CONNECTED;
                netins_udp_v2_set_error_locked(netins, 0, NULL);
            }
            pthread_mutex_unlock(&netins->lock);
            netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_INFO,
                NETINS_UDP_V2_LOGGED_CONNECTED,
                "QUIC connection established");
            netins_udp_v2_log_connection_summary(netins);
            netins_udp_v2_end_call(netins);
            return NETINS_UDP_OK;
        }
        if (transport_state == NETINS_UDP_V2_TRANSPORT_ERROR
            || transport_state == NETINS_UDP_V2_TRANSPORT_CLOSED)
        {
            netins_udp_v2_sync_transport_error(netins, transport);
            int error_code = netins_udp_v2_transport_client_get_last_error_code(
                transport);
            if (error_code == 0) {
                error_code = ECONNABORTED;
            }
            pthread_mutex_lock(&netins->lock);
            if (!netins->closing) {
                netins->state = NETINS_UDP_STATE_ERROR;
            }
            pthread_mutex_unlock(&netins->lock);
            errno = error_code;
            netins_udp_v2_log_current_error(netins,
                "nonblocking QUIC connection establishment failed");
            netins_udp_v2_end_call(netins);
            return NETINS_UDP_ERROR;
        }
        pthread_mutex_lock(&netins->lock);
        netins_udp_v2_set_error_locked(netins, EINPROGRESS,
            "connection establishment is in progress");
        pthread_mutex_unlock(&netins->lock);
        errno = EINPROGRESS;
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }

    if (netins_udp_v2_transport_client_wait_connected(transport,
            connect_timeout_ms) != 0)
    {
        int error_code = errno != 0 ? errno : ECONNABORTED;
        netins_udp_v2_sync_transport_error(netins, transport);
        netins_udp_v2_transport_client_request_close(transport);
        pthread_mutex_lock(&netins->lock);
        if (!netins->closing) {
            netins->state = NETINS_UDP_STATE_ERROR;
        }
        pthread_mutex_unlock(&netins->lock);
        errno = error_code;
        netins_udp_v2_log_current_error(netins,
            "QUIC connection establishment failed");
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    if (netins->closing) {
        pthread_mutex_unlock(&netins->lock);
        netins_udp_v2_transport_client_request_close(transport);
        (void)netins_udp_v2_fail(netins, ECANCELED,
            "connect was canceled by close");
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }
    netins->state = NETINS_UDP_STATE_CONNECTED;
    netins_udp_v2_set_error_locked(netins, 0, NULL);
    pthread_mutex_unlock(&netins->lock);
    netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_INFO,
        NETINS_UDP_V2_LOGGED_CONNECTED, "QUIC connection established");
    netins_udp_v2_log_connection_summary(netins);
    netins_udp_v2_end_call(netins);
    return NETINS_UDP_OK;
}

static int
netins_udp_v2_prepare_server_config(netins_udp_t *netins,
    const char *host, int port,
    netins_udp_v2_transport_server_config_t *config)
{
    if (port <= 0 || port > 65535) {
        return netins_udp_v2_fail(netins, EINVAL,
            "listen requires a port in [1, 65535]");
    }
    const char *bind_host = host != NULL ? host : "";
    size_t host_len = strnlen(bind_host,
        NETINS_UDP_V2_TRANSPORT_HOST_LEN);
    if (host_len >= NETINS_UDP_V2_TRANSPORT_HOST_LEN) {
        return netins_udp_v2_fail(netins, ENAMETOOLONG,
            "listen host is too long for the server backend");
    }
    if (!netins_udp_v2_host_text_valid(bind_host, host_len + 1)) {
        return netins_udp_v2_fail(netins, EINVAL,
            "listen host contains whitespace or control characters");
    }

    pthread_mutex_lock(&netins->lock);
    if (netins->app_protocol != NETINS_UDP_APP_HTTP3) {
        pthread_mutex_unlock(&netins->lock);
        return netins_udp_v2_fail(netins, EOPNOTSUPP,
            "the NetIns UDP API supports only APP_PROTOCOL=HTTP3");
    }
    if (netins->interfaces.interface_count != 0) {
        pthread_mutex_unlock(&netins->lock);
        return netins_udp_v2_fail(netins, EOPNOTSUPP,
            "INTERFACES applies to client path sockets, not a listener");
    }
    if (netins->tls.certificate_file[0] == '\0'
        || netins->tls.private_key_file[0] == '\0')
    {
        pthread_mutex_unlock(&netins->lock);
        return netins_udp_v2_fail(netins, EINVAL,
            "listener TLS requires certificate_file and private_key_file");
    }
    if (netins->tls.require_client_certificate
        || netins->tls.ca_file[0] != '\0')
    {
        pthread_mutex_unlock(&netins->lock);
        return netins_udp_v2_fail(netins, EOPNOTSUPP,
            "server client-certificate verification is not supported by this XQUIC TLS backend");
    }

    memset(config, 0, sizeof(*config));
    memcpy(config->host, bind_host, host_len + 1);
    config->port = (uint16_t)port;
    snprintf(config->certificate_file, sizeof(config->certificate_file),
        "%s", netins->tls.certificate_file);
    snprintf(config->private_key_file, sizeof(config->private_key_file),
        "%s", netins->tls.private_key_file);
    config->enable_multipath =
        netins->requested_type == NETINS_UDP_TYPE_MPQUIC;
    config->idle_timeout_ms = netins->idle_timeout_ms;
    config->kernel_sndbuf = netins->kernel_sndbuf;
    config->kernel_rcvbuf = netins->kernel_rcvbuf;
    config->initial_cwnd_packets = netins->initial_cwnd_packets;
    config->scheduler = netins->scheduler;
    config->congestion_control = netins->congestion_control;
    config->notify = netins_udp_v2_notify_handle;
    config->notify_user_data = netins;
    pthread_mutex_unlock(&netins->lock);
    return NETINS_UDP_OK;
}

netins_udp_t *
netins_udp_listen(netins_udp_t *netins, const char *host, int port)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NULL;
    }

    pthread_mutex_lock(&netins->lock);
    if (netins->state != NETINS_UDP_STATE_NEW
        || netins->endpoint_attempted)
    {
        netins_udp_v2_set_error_locked(netins, EALREADY,
            "listen can only be called once and cannot follow connect");
        pthread_mutex_unlock(&netins->lock);
        errno = EALREADY;
        netins_udp_v2_end_call(netins);
        return NULL;
    }
    netins->endpoint_attempted = 1;
    netins->role = NETINS_UDP_ROLE_LISTENER;
    netins->state = NETINS_UDP_STATE_LISTENING;
    pthread_mutex_unlock(&netins->lock);
    netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_DEBUG, 0,
        "QUIC listener startup requested");

    netins_udp_v2_transport_server_config_t config;
    if (netins_udp_v2_prepare_server_config(netins, host, port, &config)
        != NETINS_UDP_OK)
    {
        pthread_mutex_lock(&netins->lock);
        if (!netins->closing) {
            netins->state = NETINS_UDP_STATE_ERROR;
        }
        pthread_mutex_unlock(&netins->lock);
        netins_udp_v2_log_current_error(netins,
            "server transport configuration failed");
        netins_udp_v2_end_call(netins);
        return NULL;
    }

    netins_udp_v2_transport_server_t *transport = NULL;
    if (netins_udp_v2_transport_server_create(&config, &transport) != 0) {
        int error_code = errno != 0 ? errno : EIO;
        pthread_mutex_lock(&netins->lock);
        if (!netins->closing) {
            netins->state = NETINS_UDP_STATE_ERROR;
        }
        netins_udp_v2_set_error_locked(netins, error_code,
            "failed to create the shared QUIC server backend");
        pthread_mutex_unlock(&netins->lock);
        errno = error_code;
        netins_udp_v2_log_current_error(netins,
            "failed to create the shared QUIC server backend");
        netins_udp_v2_end_call(netins);
        return NULL;
    }

    pthread_mutex_lock(&netins->lock);
    netins->listener_transport = transport;
    int closing = netins->closing;
    if (!closing) {
        netins->state = NETINS_UDP_STATE_LISTENING;
        netins_udp_v2_set_error_locked(netins, 0, NULL);
    }
    pthread_cond_broadcast(&netins->idle_cond);
    pthread_mutex_unlock(&netins->lock);
    if (closing) {
        netins_udp_v2_transport_server_begin_close(transport);
        (void)netins_udp_v2_fail(netins, ECANCELED,
            "listen was canceled by close");
        netins_udp_v2_end_call(netins);
        return NULL;
    }

    netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_INFO,
        NETINS_UDP_V2_LOGGED_LISTENING, "QUIC listener is ready");
    netins_udp_v2_end_call(netins);
    return netins;
}

netins_udp_t *
netins_udp_accept(netins_udp_t *listener, struct sockaddr *addr,
    socklen_t *addrlen)
{
    if (netins_udp_v2_begin_call(listener) != NETINS_UDP_OK) {
        return NULL;
    }
    if (addr != NULL && addrlen == NULL) {
        (void)netins_udp_v2_fail(listener, EINVAL,
            "addrlen is required when addr is non-NULL");
        netins_udp_v2_end_call(listener);
        return NULL;
    }

    pthread_mutex_lock(&listener->lock);
    if (listener->role != NETINS_UDP_ROLE_LISTENER
        || listener->state != NETINS_UDP_STATE_LISTENING
        || listener->listener_transport == NULL)
    {
        netins_udp_v2_set_error_locked(listener, EINVAL,
            "accept requires a listening server handle");
        pthread_mutex_unlock(&listener->lock);
        errno = EINVAL;
        netins_udp_v2_end_call(listener);
        return NULL;
    }
    netins_udp_v2_transport_server_t *server =
        listener->listener_transport;
    int nonblock = listener->nonblock;
    uint32_t accept_timeout_ms = listener->accept_timeout_ms;
    pthread_mutex_unlock(&listener->lock);

    netins_udp_v2_transport_server_connection_t *connection = NULL;
    int wait_forever = !nonblock && accept_timeout_ms == 0;
    uint32_t backend_timeout_ms = nonblock ? 0 : accept_timeout_ms;
    if (netins_udp_v2_transport_server_accept(server, backend_timeout_ms,
            wait_forever, &connection) != 0)
    {
        int error_code = errno != 0 ? errno : EIO;
        const char *message = error_code == EAGAIN
            ? "no accepted connection is currently ready"
            : error_code == ETIMEDOUT
                ? "timed out waiting for an accepted connection"
                : error_code == ECANCELED
                    ? "accept was canceled because the listener is closing"
                    : "the shared QUIC server backend failed to accept";
        pthread_mutex_lock(&listener->lock);
        netins_udp_v2_set_error_locked(listener, error_code, message);
        pthread_mutex_unlock(&listener->lock);
        errno = error_code;
        netins_udp_v2_end_call(listener);
        return NULL;
    }

    struct sockaddr_storage peer_address;
    socklen_t peer_address_len = 0;
    (void)netins_udp_v2_transport_server_connection_get_peer_address(
        connection, &peer_address, &peer_address_len);

    pthread_mutex_lock(&listener->lock);
    int canceled = listener->closing
        || listener->state != NETINS_UDP_STATE_LISTENING;
    int requested_type = listener->requested_type;
    int app_protocol = listener->app_protocol;
    int inherited_nonblock = listener->nonblock;
    uint32_t send_timeout_ms = listener->send_timeout_ms;
    uint32_t recv_timeout_ms = listener->recv_timeout_ms;
    uint32_t close_timeout_ms = listener->close_timeout_ms;
    uint32_t idle_timeout_ms = listener->idle_timeout_ms;
    int kernel_sndbuf = listener->kernel_sndbuf;
    int kernel_rcvbuf = listener->kernel_rcvbuf;
    uint32_t initial_cwnd_packets = listener->initial_cwnd_packets;
    int scheduler = listener->scheduler;
    int congestion_control = listener->congestion_control;
    int log_level = listener->log_level;
    netins_udp_logger_cb logger = listener->logger;
    netins_udp_tls_config_t tls = listener->tls;
    pthread_mutex_unlock(&listener->lock);

    if (canceled) {
        netins_udp_v2_transport_server_connection_close(connection,
            close_timeout_ms);
        (void)netins_udp_v2_fail(listener, ECANCELED,
            "accept completed while the listener was closing");
        netins_udp_v2_end_call(listener);
        return NULL;
    }

    netins_udp_t *accepted = netins_udp_new(requested_type);
    if (accepted == NULL) {
        int error_code = errno != 0 ? errno : ENOMEM;
        netins_udp_v2_transport_server_connection_close(connection,
            close_timeout_ms);
        (void)netins_udp_v2_fail(listener, error_code,
            "failed to allocate the accepted connection handle");
        netins_udp_v2_end_call(listener);
        errno = error_code;
        return NULL;
    }

    pthread_mutex_lock(&accepted->lock);
    accepted->endpoint_attempted = 1;
    accepted->role = NETINS_UDP_ROLE_ACCEPTED;
    accepted->state = netins_udp_v2_transport_state_to_public(
        netins_udp_v2_transport_server_connection_get_state(connection));
    accepted->app_protocol = app_protocol;
    accepted->nonblock = inherited_nonblock;
    accepted->send_timeout_ms = send_timeout_ms;
    accepted->recv_timeout_ms = recv_timeout_ms;
    accepted->close_timeout_ms = close_timeout_ms;
    accepted->idle_timeout_ms = idle_timeout_ms;
    accepted->kernel_sndbuf = kernel_sndbuf;
    accepted->kernel_rcvbuf = kernel_rcvbuf;
    accepted->initial_cwnd_packets = initial_cwnd_packets;
    accepted->scheduler = scheduler;
    accepted->congestion_control = congestion_control;
    accepted->log_level = log_level;
    accepted->logger = logger;
    accepted->tls = tls;
    accepted->accepted_transport = connection;
    pthread_mutex_unlock(&accepted->lock);
    netins_udp_v2_transport_server_connection_set_notify(connection,
        netins_udp_v2_notify_handle, accepted);
    netins_udp_v2_log_connection_summary(accepted);

    if (addr != NULL) {
        socklen_t capacity = *addrlen;
        socklen_t copied = capacity < peer_address_len
            ? capacity : peer_address_len;
        if (copied > 0) {
            memcpy(addr, &peer_address, copied);
        }
        *addrlen = peer_address_len;
    }
    pthread_mutex_lock(&listener->lock);
    netins_udp_v2_set_error_locked(listener, 0, NULL);
    pthread_mutex_unlock(&listener->lock);
    netins_udp_v2_end_call(listener);
    return accepted;
}

int
netins_udp_shutdown(netins_udp_t *netins, uint64_t application_error)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    int error_code = 0;
    const char *message = NULL;
    if (netins->role == NETINS_UDP_ROLE_LISTENER) {
        error_code = EOPNOTSUPP;
        message = "shutdown applies to a connection, not a listener";
    } else if (netins->state == NETINS_UDP_STATE_CLOSING
        || netins->state == NETINS_UDP_STATE_CLOSED)
    {
        error_code = EALREADY;
        message = "connection shutdown has already begun";
    } else if (netins->state != NETINS_UDP_STATE_CONNECTED) {
        error_code = ENOTCONN;
        message = "shutdown requires a connected handle";
    } else if (netins->role == NETINS_UDP_ROLE_CLIENT
        && netins->client_transport == NULL)
    {
        error_code = ENOTCONN;
        message = "client transport is not available";
    } else if (netins->role == NETINS_UDP_ROLE_ACCEPTED
        && netins->accepted_transport == NULL)
    {
        error_code = ENOTCONN;
        message = "accepted transport is not available";
    } else if (netins->role != NETINS_UDP_ROLE_CLIENT
        && netins->role != NETINS_UDP_ROLE_ACCEPTED)
    {
        error_code = ENOTCONN;
        message = "handle does not represent a connection";
    }

    netins_udp_v2_transport_client_t *client_transport =
        netins->client_transport;
    netins_udp_v2_transport_server_connection_t *accepted_transport =
        netins->accepted_transport;
    if (error_code == 0) {
        netins->state = NETINS_UDP_STATE_CLOSING;
        netins_udp_v2_set_close_reason_locked(netins,
            NETINS_UDP_CLOSE_LOCAL, 0, 0, application_error,
            "connection shutdown requested locally");
        netins_udp_v2_set_error_locked(netins, 0, NULL);
    } else {
        netins_udp_v2_set_error_locked(netins, error_code, message);
    }
    pthread_mutex_unlock(&netins->lock);

    if (error_code != 0) {
        errno = error_code;
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }

    int transport_shutdown_requested = 0;
    if (application_error == 0) {
        /*
         * A peer-side first recv can be blocked inside lazy request accept.
         * Once the public state is CLOSING no new creator can start. If one
         * was already active, request transport shutdown before waiting so
         * that accept/open is canceled instead of deadlocking shutdown.
        */
        pthread_mutex_lock(&netins->data_lock);
        int creation_in_progress = netins->data_backend_creating;
        pthread_mutex_unlock(&netins->data_lock);
        if (creation_in_progress) {
            if (client_transport != NULL) {
                netins_udp_v2_transport_client_request_shutdown(
                    client_transport, 0);
            } else {
                netins_udp_v2_transport_server_connection_request_shutdown(
                    accepted_transport, 0);
            }
            transport_shutdown_requested = 1;
        }

        pthread_mutex_lock(&netins->data_lock);
        while (netins->data_backend_creating) {
            int wait_result = pthread_cond_wait(&netins->data_cond,
                &netins->data_lock);
            if (wait_result != 0) {
                error_code = wait_result;
                break;
            }
        }
        netins_udp_data_backend_t *data_backend =
            netins->data_backend;
        pthread_mutex_unlock(&netins->data_lock);

        if (error_code == 0 && data_backend != NULL
            && netins_udp_data_backend_finish_send(data_backend)
                != NETINS_UDP_OK)
        {
            error_code = errno != 0 ? errno : EIO;
            netins_udp_v2_sync_data_backend_error(netins, data_backend,
                error_code,
                "failed to finish queued connection data before shutdown");
        }
        if (error_code != 0) {
            pthread_mutex_lock(&netins->lock);
            netins->state = NETINS_UDP_STATE_ERROR;
            netins_udp_v2_set_error_locked(netins, error_code,
                "graceful shutdown could not finish queued application data");
            pthread_mutex_unlock(&netins->lock);
            netins_udp_v2_notifier_signal(&netins->notifier,
                NETINS_UDP_EVENT_ERROR);
            errno = error_code;
            netins_udp_v2_end_call(netins);
            return NETINS_UDP_ERROR;
        }
    }

    if (!transport_shutdown_requested || application_error != 0) {
        if (client_transport != NULL) {
            netins_udp_v2_transport_client_request_shutdown(client_transport,
                application_error);
        } else {
            netins_udp_v2_transport_server_connection_request_shutdown(
                accepted_transport, application_error);
        }
    }
    netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_INFO,
        NETINS_UDP_V2_LOGGED_SHUTDOWN,
        "graceful QUIC connection shutdown requested");
    netins_udp_v2_end_call(netins);
    return NETINS_UDP_OK;
}

/*
 * Refreshes state for a caller that already owns an active-call reference.
 * Calling the public get_state() from another public API would repeat the
 * closing check: close could start between those checks and incorrectly turn
 * an otherwise valid in-flight snapshot into NETINS_UDP_STATE_ERROR.
 */
static int
netins_udp_v2_refresh_state(netins_udp_t *netins)
{
    pthread_mutex_lock(&netins->lock);
    int state = netins->state;
    int role = netins->role;
    netins_udp_v2_transport_client_t *client_transport =
        netins->client_transport;
    netins_udp_v2_transport_server_t *listener_transport =
        netins->listener_transport;
    netins_udp_v2_transport_server_connection_t *accepted_transport =
        netins->accepted_transport;
    int preserve_terminal = state == NETINS_UDP_STATE_ERROR
        || netins->closing;
    pthread_mutex_unlock(&netins->lock);

    uint32_t notify_events = 0;
    if (!preserve_terminal
        && (client_transport != NULL || listener_transport != NULL
            || accepted_transport != NULL))
    {
        int transport_state;
        if (role == NETINS_UDP_ROLE_CLIENT && client_transport != NULL) {
            transport_state =
                netins_udp_v2_transport_client_get_state(client_transport);
        } else if (role == NETINS_UDP_ROLE_LISTENER
            && listener_transport != NULL)
        {
            transport_state =
                netins_udp_v2_transport_server_get_state(
                    listener_transport);
        } else if (role == NETINS_UDP_ROLE_ACCEPTED
            && accepted_transport != NULL)
        {
            transport_state =
                netins_udp_v2_transport_server_connection_get_state(
                    accepted_transport);
        } else {
            transport_state = NETINS_UDP_V2_TRANSPORT_ERROR;
        }
        int next_state =
            netins_udp_v2_transport_state_to_public(transport_state);
        if (state == NETINS_UDP_STATE_CONNECTING
            && next_state == NETINS_UDP_STATE_NEW)
        {
            next_state = NETINS_UDP_STATE_CONNECTING;
        }
        if (state == NETINS_UDP_STATE_CLOSING
            && (next_state == NETINS_UDP_STATE_CONNECTING
                || next_state == NETINS_UDP_STATE_CONNECTED))
        {
            next_state = NETINS_UDP_STATE_CLOSING;
        }
        if (next_state == NETINS_UDP_STATE_ERROR) {
            if (role == NETINS_UDP_ROLE_CLIENT) {
                netins_udp_v2_sync_transport_error(netins,
                    client_transport);
            } else if (role == NETINS_UDP_ROLE_LISTENER) {
                netins_udp_v2_sync_listener_error(netins,
                    listener_transport);
            } else {
                netins_udp_v2_sync_accepted_error(netins,
                    accepted_transport);
            }
        }
        pthread_mutex_lock(&netins->lock);
        if (!netins->closing && netins->state != NETINS_UDP_STATE_ERROR) {
            /*
             * shutdown() may have moved the public handle to CLOSING while
             * the transport state query above was in progress.  Re-check the
             * live public state before committing the sampled transport
             * value; an older CONNECTED sample must never reopen a closing
             * handle.
             */
            if (netins->state == NETINS_UDP_STATE_CLOSING
                && (next_state == NETINS_UDP_STATE_CONNECTING
                    || next_state == NETINS_UDP_STATE_CONNECTED))
            {
                next_state = NETINS_UDP_STATE_CLOSING;
            }
            if (next_state == NETINS_UDP_STATE_CLOSED
                && netins->close_reason.source == NETINS_UDP_CLOSE_NONE)
            {
                netins_udp_v2_set_close_reason_locked(netins,
                    NETINS_UDP_CLOSE_PEER, 0, 0, 0,
                    "connection closed by peer");
            }
            if (netins->state != next_state) {
                if (next_state == NETINS_UDP_STATE_CONNECTED) {
                    notify_events |= NETINS_UDP_EVENT_CONNECTED;
                    /*
                     * A nonblocking connect reports EINPROGRESS while the
                     * transport is being established.  Completing that
                     * asynchronous operation successfully must clear the
                     * provisional error; otherwise a direct error query
                     * after CONNECTED would still report EINPROGRESS.
                     */
                    if (netins->state == NETINS_UDP_STATE_CONNECTING) {
                        netins_udp_v2_set_error_locked(netins, 0, NULL);
                    }
                } else if (next_state == NETINS_UDP_STATE_CLOSED) {
                    notify_events |= NETINS_UDP_EVENT_CLOSED;
                } else if (next_state == NETINS_UDP_STATE_ERROR) {
                    notify_events |= NETINS_UDP_EVENT_ERROR;
                }
            }
            netins->state = next_state;
        }
        state = netins->state;
        pthread_mutex_unlock(&netins->lock);
    }

    if (notify_events != 0) {
        netins_udp_v2_notify_handle(netins, notify_events);
        if ((notify_events & NETINS_UDP_EVENT_CONNECTED) != 0) {
            netins_udp_v2_log_connection_summary(netins);
        }
    }
    return state;
}

int
netins_udp_get_state(netins_udp_t *netins)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_STATE_ERROR;
    }

    int state = netins_udp_v2_refresh_state(netins);
    netins_udp_v2_end_call(netins);
    return state;
}

static void
netins_udp_v2_sync_data_backend_error(netins_udp_t *netins,
    netins_udp_data_backend_t *backend, int fallback_error,
    const char *fallback_message)
{
    int saved_errno = errno;
    int error_code = 0;
    char message[NETINS_UDP_ERROR_MSG_LEN] = {0};
    if (backend != NULL) {
        (void)netins_udp_data_backend_get_error(backend, &error_code,
            message, sizeof(message));
    }
    if (error_code == 0) {
        error_code = saved_errno != 0 ? saved_errno : fallback_error;
    }
    netins_udp_v2_connection_set_error(netins, error_code,
        message[0] != '\0' ? message : fallback_message);
    errno = error_code;
}

static int
netins_udp_v2_default_data_backend_get(netins_udp_t *netins,
    int flags, netins_udp_data_backend_t **out_backend)
{
    if (out_backend == NULL) {
        return netins_udp_v2_fail(netins, EINVAL,
            "default data backend output must not be NULL");
    }
    *out_backend = NULL;

    (void)netins_udp_v2_refresh_state(netins);
    pthread_mutex_lock(&netins->lock);
    int role = netins->role;
    int state = netins->state;
    int app_protocol = netins->app_protocol;
    int nonblock = netins->nonblock;
    int call_dontwait = nonblock
        || (flags & NETINS_UDP_MSG_DONTWAIT) != 0;
    uint32_t send_timeout_ms = netins->send_timeout_ms;
    uint32_t recv_timeout_ms = netins->recv_timeout_ms;
    uint32_t close_timeout_ms = netins->close_timeout_ms;
    int error_code = 0;
    const char *message = NULL;
    if (role == NETINS_UDP_ROLE_LISTENER) {
        error_code = EOPNOTSUPP;
        message = "connection data send/recv is not valid on a listener";
    } else if (role != NETINS_UDP_ROLE_CLIENT
        && role != NETINS_UDP_ROLE_ACCEPTED)
    {
        error_code = ENOTCONN;
        message = "handle does not represent a connected endpoint";
    } else if (state != NETINS_UDP_STATE_CONNECTED) {
        error_code = ENOTCONN;
        message = "connection data send/recv requires a connected handle";
    } else if (app_protocol != NETINS_UDP_APP_HTTP3) {
        error_code = EOPNOTSUPP;
        message = "connection data send/recv requires HTTP/3";
    }
    if (error_code != 0) {
        netins_udp_v2_set_error_locked(netins, error_code, message);
    }
    pthread_mutex_unlock(&netins->lock);
    if (error_code != 0) {
        errno = error_code;
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->data_lock);
    if (netins->h3_api_selected) {
        pthread_mutex_unlock(&netins->data_lock);
        return netins_udp_v2_fail(netins, EBUSY,
            "connection already uses another application data API");
    }
    netins->data_api_selected = 1;
    while (netins->data_backend_creating)
    {
        if (call_dontwait) {
            pthread_mutex_unlock(&netins->data_lock);
            return netins_udp_v2_fail(netins, EAGAIN,
                "parallel data channel creation is already in progress");
        }
        int wait_result = pthread_cond_wait(&netins->data_cond,
            &netins->data_lock);
        if (wait_result != 0) {
            pthread_mutex_unlock(&netins->data_lock);
            return netins_udp_v2_fail(netins, wait_result,
                "failed waiting for default data channel creation");
        }
    }
    netins_udp_data_backend_t *backend = netins->data_backend;
    netins->data_backend_creating = 1;
    pthread_mutex_unlock(&netins->data_lock);

    int result = NETINS_UDP_OK;
    int backend_created = 0;
    if (backend == NULL) {
        netins_udp_data_backend_config_t config;
        memset(&config, 0, sizeof(config));
        config.connection = netins;
        config.parent_notifier = &netins->notifier;
        config.role = role;
        config.nonblock = nonblock;
        config.send_timeout_ms = send_timeout_ms;
        config.recv_timeout_ms = recv_timeout_ms;
        config.close_timeout_ms = close_timeout_ms;
        result = netins_udp_data_backend_create(&config, &backend);
        backend_created = result == NETINS_UDP_OK;
    }
    if (result == NETINS_UDP_OK) {
        result = netins_udp_data_backend_establish(backend,
            call_dontwait);
    }

    int saved_error = errno != 0 ? errno : EIO;
    if (result != NETINS_UDP_OK
        && saved_error != EAGAIN && saved_error != ETIMEDOUT)
    {
        netins_udp_v2_sync_data_backend_error(netins, backend,
            saved_error, "failed to establish parallel data channels");
    }

    pthread_mutex_lock(&netins->data_lock);
    if (backend_created && netins->data_backend == NULL
        && (result == NETINS_UDP_OK || saved_error == EAGAIN
            || saved_error == ETIMEDOUT))
    {
        netins->data_backend = backend;
    }
    netins->data_backend_creating = 0;
    pthread_cond_broadcast(&netins->data_cond);
    pthread_mutex_unlock(&netins->data_lock);

    if (result != NETINS_UDP_OK) {
        /*
         * EAGAIN/ETIMEDOUT can occur after only some server-side lanes were
         * accepted. Keep that private backend so the next call resumes
         * establishment without losing already accepted requests.
         */
        if (backend_created && saved_error != EAGAIN
            && saved_error != ETIMEDOUT)
        {
            netins_udp_data_backend_destroy(backend);
        }
        errno = saved_error;
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->data_lock);
    if (netins->data_backend == NULL) {
        netins->data_backend = backend;
    }
    *out_backend = netins->data_backend;
    pthread_mutex_unlock(&netins->data_lock);
    netins_udp_v2_connection_set_error(netins, 0, NULL);
    return NETINS_UDP_OK;
}

ssize_t
netins_udp_send(netins_udp_t *netins, const void *data,
    size_t length, int flags)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return -1;
    }
    if ((flags & ~(NETINS_UDP_MSG_DONTWAIT
            | NETINS_UDP_MSG_FIN)) != 0
        || (length != 0 && data == NULL))
    {
        (void)netins_udp_v2_fail(netins, EINVAL,
            "invalid connection data send arguments");
        netins_udp_v2_end_call(netins);
        return -1;
    }
    netins_udp_data_backend_t *backend = NULL;
    if (netins_udp_v2_default_data_backend_get(netins, flags, &backend)
        != NETINS_UDP_OK)
    {
        netins_udp_v2_end_call(netins);
        return -1;
    }

    ssize_t sent = netins_udp_data_backend_send(backend, data, length,
        flags);
    if (sent < 0) {
        netins_udp_v2_sync_data_backend_error(netins, backend,
            EIO, "connection data send failed");
    } else {
        netins_udp_v2_connection_set_error(netins, 0, NULL);
    }
    netins_udp_v2_end_call(netins);
    return sent;
}

ssize_t
netins_udp_recv(netins_udp_t *netins, void *data,
    size_t length, int flags)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return -1;
    }
    if ((flags & ~(NETINS_UDP_MSG_DONTWAIT
            | NETINS_UDP_MSG_PEEK)) != 0
        || (length != 0 && data == NULL))
    {
        (void)netins_udp_v2_fail(netins, EINVAL,
            "invalid connection data receive arguments");
        netins_udp_v2_end_call(netins);
        return -1;
    }
    netins_udp_data_backend_t *backend = NULL;
    if (netins_udp_v2_default_data_backend_get(netins, flags, &backend)
        != NETINS_UDP_OK)
    {
        netins_udp_v2_end_call(netins);
        return -1;
    }

    ssize_t received = netins_udp_data_backend_recv(backend, data,
        length, flags);
    if (received < 0) {
        netins_udp_v2_sync_data_backend_error(netins, backend,
            EIO, "connection data receive failed");
    } else {
        netins_udp_v2_connection_set_error(netins, 0, NULL);
    }
    netins_udp_v2_end_call(netins);
    return received;
}

static int
netins_udp_v2_validate_output(netins_udp_t *netins,
    uint32_t struct_size, uint32_t api_version, size_t required_size,
    const char *name)
{
    if (api_version != NETINS_UDP_API_VERSION) {
        char message[NETINS_UDP_ERROR_MSG_LEN];
        snprintf(message, sizeof(message), "%s API version mismatch", name);
        return netins_udp_v2_fail(netins, EPROTO, message);
    }
    if ((size_t)struct_size < required_size) {
        char message[NETINS_UDP_ERROR_MSG_LEN];
        snprintf(message, sizeof(message), "%s output buffer is too small",
            name);
        return netins_udp_v2_fail(netins, ENOBUFS, message);
    }
    return NETINS_UDP_OK;
}

static void
netins_udp_v2_init_public_snapshot(netins_udp_snapshot_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->struct_size = sizeof(*snapshot);
    snapshot->api_version = NETINS_UDP_API_VERSION;
    snapshot->info = (netins_udp_info_t)NETINS_UDP_INFO_INITIALIZER;
    snapshot->statis =
        (netins_udp_statis_t)NETINS_UDP_STATIS_INITIALIZER;
    snapshot->close_reason =
        (netins_udp_close_reason_t)NETINS_UDP_CLOSE_REASON_INITIALIZER;
    for (size_t i = 0; i < NETINS_UDP_MAX_PATHS; ++i) {
        snapshot->paths[i] = (netins_udp_path_statis_t)
            NETINS_UDP_PATH_STATIS_INITIALIZER;
        snapshot->paths[i].path_index = (uint32_t)i;
    }
}

/*
 * Caller owns one active-call reference.  Observer data is copied atomically
 * by the private transport bridge; V2 handle metadata is copied under its own
 * lock and never points into XQUIC memory.
 */
static int
netins_udp_v2_build_snapshot(netins_udp_t *netins,
    netins_udp_snapshot_t *out)
{
    int state = netins_udp_v2_refresh_state(netins);
    pthread_mutex_lock(&netins->lock);
    int requested_type = netins->requested_type;
    int role = netins->role;
    int app_protocol = netins->app_protocol;
    netins_udp_v2_transport_client_t *client_transport =
        netins->client_transport;
    netins_udp_v2_transport_server_connection_t *accepted_transport =
        netins->accepted_transport;
    netins_udp_close_reason_t close_reason = netins->close_reason;
    pthread_mutex_unlock(&netins->lock);

    netins_udp_v2_transport_snapshot_t transport;
    memset(&transport, 0, sizeof(transport));
    transport.connection_id = NETINS_UDP_INVALID_CONNECTION_ID;
    int have_transport_snapshot = 0;
    if (role == NETINS_UDP_ROLE_CLIENT && client_transport != NULL) {
        have_transport_snapshot =
            netins_udp_v2_transport_client_get_snapshot(client_transport,
                &transport) == 0;
    } else if (role == NETINS_UDP_ROLE_ACCEPTED
        && accepted_transport != NULL)
    {
        have_transport_snapshot =
            netins_udp_v2_transport_server_connection_get_snapshot(
                accepted_transport, &transport) == 0;
    }
    int transport_present =
        (role == NETINS_UDP_ROLE_CLIENT && client_transport != NULL)
        || (role == NETINS_UDP_ROLE_ACCEPTED
            && accepted_transport != NULL);
    if (transport_present && !have_transport_snapshot)
    {
        return netins_udp_v2_fail(netins,
            errno != 0 ? errno : EIO,
            "failed to copy the private connection observer snapshot");
    }

    netins_udp_v2_init_public_snapshot(out);
    out->info.connection_id = have_transport_snapshot
        ? transport.connection_id : NETINS_UDP_INVALID_CONNECTION_ID;
    out->info.requested_type = requested_type;
    out->info.role = role;
    out->info.state = state;
    out->info.app_protocol = app_protocol;
    out->info.handshake_finished = have_transport_snapshot
        ? transport.handshake_finished : 0;
    out->info.multipath_mode = have_transport_snapshot
        ? transport.multipath_mode : 0;
    out->info.multipath_requested =
        requested_type == NETINS_UDP_TYPE_MPQUIC;
    out->info.multipath_active = have_transport_snapshot
        ? transport.multipath_active : 0;
    out->info.path_count = have_transport_snapshot
        ? transport.path_count : 0;
    out->info.active_path_count = have_transport_snapshot
        ? transport.active_path_count : 0;
    out->info.validating_path_count = have_transport_snapshot
        ? transport.validating_path_count : 0;
    out->info.path_table_truncated = have_transport_snapshot
        ? transport.path_table_truncated : 0;
    if (have_transport_snapshot) {
        snprintf(out->info.alpn, sizeof(out->info.alpn), "%s",
            transport.alpn);
        snprintf(out->info.local_address,
            sizeof(out->info.local_address), "%s",
            transport.local_address);
        snprintf(out->info.peer_address,
            sizeof(out->info.peer_address), "%s",
            transport.peer_address);
    }

    if (have_transport_snapshot) {
        out->statis.snapshot_time_us = transport.snapshot_time_us;
        out->statis.sample_interval_us = transport.sample_interval_us;
        out->statis.bytes_sent = transport.bytes_sent;
        out->statis.bytes_received = transport.bytes_received;
        out->statis.bytes_wire_sent = transport.bytes_wire_sent;
        out->statis.bytes_wire_received = transport.bytes_wire_received;
        out->statis.bytes_acked = transport.bytes_acked;
        out->statis.bytes_lost = transport.bytes_lost;
        out->statis.bytes_inflight = transport.bytes_inflight;
        out->statis.send_mbps = transport.send_mbps;
        out->statis.receive_mbps = transport.receive_mbps;
        out->statis.acked_mbps = transport.acked_mbps;
        out->statis.loss_rate = transport.loss_rate;
    }

    /*
     * In connection-level byte mode, expose exactly the bytes accepted by
     * send() and consumed by recv(). Transport and per-path counters remain
     * wire/QUIC observations, including the private aggregation channels.
     */
    pthread_mutex_lock(&netins->data_lock);
    netins_udp_data_backend_t *data_backend = netins->data_backend;
    if (data_backend != NULL) {
        uint64_t application_bytes_sent = 0;
        uint64_t application_bytes_received = 0;
        if (netins_udp_data_backend_get_counters(data_backend,
                &application_bytes_sent,
                &application_bytes_received) == NETINS_UDP_OK)
        {
            out->statis.bytes_sent = application_bytes_sent;
            out->statis.bytes_received = application_bytes_received;
        }
    }
    pthread_mutex_unlock(&netins->data_lock);

    out->path_count = out->info.path_count;
    for (uint32_t i = 0; i < out->path_count
         && i < NETINS_UDP_MAX_PATHS; ++i)
    {
        const netins_udp_v2_transport_path_snapshot_t *source =
            &transport.paths[i];
        netins_udp_path_statis_t *path = &out->paths[i];
        path->path_id = source->path_id;
        path->path_index = source->path_index;
        path->path_state = source->path_state;
        path->app_status = source->app_status;
        path->cwnd_blocked = source->cwnd_blocked;
        snprintf(path->interface_name, sizeof(path->interface_name), "%s",
            source->interface_name);
        snprintf(path->local_address, sizeof(path->local_address), "%s",
            source->local_address);
        snprintf(path->peer_address, sizeof(path->peer_address), "%s",
            source->peer_address);
        path->bytes_sent = source->bytes_sent;
        path->bytes_received = source->bytes_received;
        path->bytes_acked = source->bytes_acked;
        path->bytes_lost = source->bytes_lost;
        path->bytes_inflight = source->bytes_inflight;
        path->cwnd_bytes = source->cwnd_bytes;
        path->srtt_us = source->srtt_us;
        path->latest_rtt_us = source->latest_rtt_us;
        path->min_rtt_us = source->min_rtt_us;
        path->last_received_time_us = source->last_received_time_us;
        path->last_sent_time_us = source->last_sent_time_us;
        path->send_mbps = source->send_mbps;
        path->receive_mbps = source->receive_mbps;
        path->acked_mbps = source->acked_mbps;
        path->loss_rate = source->loss_rate;
    }

    if (have_transport_snapshot && transport.connection_error != 0) {
        pthread_mutex_lock(&netins->lock);
        netins_udp_v2_set_close_reason_locked(netins,
            NETINS_UDP_CLOSE_TRANSPORT, 0,
            (uint64_t)(unsigned int)transport.connection_error, 0,
            "XQUIC connection reported a transport error");
        close_reason = netins->close_reason;
        pthread_mutex_unlock(&netins->lock);
    }
    out->close_reason = close_reason;
    return NETINS_UDP_OK;
}

int
netins_udp_is_multipath(netins_udp_t *netins)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    netins_udp_snapshot_t snapshot;
    int result = netins_udp_v2_build_snapshot(netins, &snapshot);
    int active = result == NETINS_UDP_OK
        ? snapshot.info.multipath_active : NETINS_UDP_ERROR;
    netins_udp_v2_end_call(netins);
    return active;
}

int
netins_udp_get_path_count(netins_udp_t *netins, uint32_t *out_count)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    if (out_count == NULL) {
        int result = netins_udp_v2_fail(netins, EINVAL,
            "path count output must not be NULL");
        netins_udp_v2_end_call(netins);
        return result;
    }
    netins_udp_snapshot_t snapshot;
    int result = netins_udp_v2_build_snapshot(netins, &snapshot);
    if (result == NETINS_UDP_OK) {
        *out_count = snapshot.path_count;
    }
    netins_udp_v2_end_call(netins);
    return result;
}

int
netins_udp_get_snapshot(netins_udp_t *netins, netins_udp_snapshot_t *out)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    if (out == NULL) {
        int result = netins_udp_v2_fail(netins, EINVAL,
            "snapshot output must not be NULL");
        netins_udp_v2_end_call(netins);
        return result;
    }
    if (netins_udp_v2_validate_output(netins, out->struct_size,
            out->api_version, sizeof(*out), "snapshot") != NETINS_UDP_OK)
    {
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }
    netins_udp_snapshot_t snapshot;
    int result = netins_udp_v2_build_snapshot(netins, &snapshot);
    if (result == NETINS_UDP_OK) {
        *out = snapshot;
    }
    netins_udp_v2_end_call(netins);
    return result;
}

int
netins_udp_get_info(netins_udp_t *netins, netins_udp_info_t *out)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    if (out == NULL) {
        int result = netins_udp_v2_fail(netins, EINVAL,
            "info output must not be NULL");
        netins_udp_v2_end_call(netins);
        return result;
    }
    if (netins_udp_v2_validate_output(netins, out->struct_size,
            out->api_version, sizeof(*out), "info") != NETINS_UDP_OK)
    {
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }
    netins_udp_snapshot_t snapshot;
    int result = netins_udp_v2_build_snapshot(netins, &snapshot);
    if (result == NETINS_UDP_OK) {
        *out = snapshot.info;
    }
    netins_udp_v2_end_call(netins);
    return result;
}

int
netins_udp_get_statis(netins_udp_t *netins, netins_udp_statis_t *out)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    if (out == NULL) {
        int result = netins_udp_v2_fail(netins, EINVAL,
            "statistics output must not be NULL");
        netins_udp_v2_end_call(netins);
        return result;
    }
    if (netins_udp_v2_validate_output(netins, out->struct_size,
            out->api_version, sizeof(*out), "statistics")
        != NETINS_UDP_OK)
    {
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }
    netins_udp_snapshot_t snapshot;
    int result = netins_udp_v2_build_snapshot(netins, &snapshot);
    if (result == NETINS_UDP_OK) {
        *out = snapshot.statis;
    }
    netins_udp_v2_end_call(netins);
    return result;
}

int
netins_udp_get_path_statis(netins_udp_t *netins, size_t path_index,
    netins_udp_path_statis_t *out)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    if (out == NULL) {
        int result = netins_udp_v2_fail(netins, EINVAL,
            "path statistics output must not be NULL");
        netins_udp_v2_end_call(netins);
        return result;
    }
    if (netins_udp_v2_validate_output(netins, out->struct_size,
            out->api_version, sizeof(*out), "path statistics")
        != NETINS_UDP_OK)
    {
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }
    netins_udp_snapshot_t snapshot;
    int result = netins_udp_v2_build_snapshot(netins, &snapshot);
    if (result == NETINS_UDP_OK && path_index >= snapshot.path_count) {
        result = netins_udp_v2_fail(netins, ENOENT,
            "path index is outside the stable observer table");
    }
    if (result == NETINS_UDP_OK) {
        *out = snapshot.paths[path_index];
    }
    netins_udp_v2_end_call(netins);
    return result;
}

int
netins_udp_get_close_reason(netins_udp_t *netins,
    netins_udp_close_reason_t *out)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    if (out == NULL) {
        int result = netins_udp_v2_fail(netins, EINVAL,
            "close reason output must not be NULL");
        netins_udp_v2_end_call(netins);
        return result;
    }
    if (netins_udp_v2_validate_output(netins, out->struct_size,
            out->api_version, sizeof(*out), "close reason")
        != NETINS_UDP_OK)
    {
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }
    (void)netins_udp_v2_refresh_state(netins);
    pthread_mutex_lock(&netins->lock);
    *out = netins->close_reason;
    pthread_mutex_unlock(&netins->lock);
    netins_udp_v2_end_call(netins);
    return NETINS_UDP_OK;
}

int
netins_udp_get_fd(netins_udp_t *netins)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return -1;
    }
    int fd = netins_udp_v2_notifier_get_fd(&netins->notifier);
    if (fd < 0) {
        (void)netins_udp_v2_fail(netins, EBADF,
            "connection notification descriptor is unavailable");
    }
    netins_udp_v2_end_call(netins);
    return fd;
}

int
netins_udp_get_events(netins_udp_t *netins, uint32_t *out_events)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    if (out_events == NULL) {
        int result = netins_udp_v2_fail(netins, EINVAL,
            "event output must not be NULL");
        netins_udp_v2_end_call(netins);
        return result;
    }
    (void)netins_udp_v2_refresh_state(netins);
    pthread_mutex_lock(&netins->lock);
    int role = netins->role;
    int state = netins->state;
    netins_udp_v2_transport_client_t *client_transport =
        netins->client_transport;
    netins_udp_v2_transport_server_t *listener_transport =
        netins->listener_transport;
    netins_udp_v2_transport_server_connection_t *accepted_transport =
        netins->accepted_transport;
    pthread_mutex_unlock(&netins->lock);

    uint64_t level_generation =
        netins_udp_v2_notifier_begin_level_query(&netins->notifier);
    uint32_t level_events = 0;
    int transport_result = 0;
    if (role == NETINS_UDP_ROLE_CLIENT && client_transport != NULL) {
        transport_result = netins_udp_v2_transport_client_get_events(
            client_transport, &level_events);
    } else if (role == NETINS_UDP_ROLE_LISTENER
        && listener_transport != NULL)
    {
        transport_result = netins_udp_v2_transport_server_get_events(
            listener_transport, &level_events);
    } else if (role == NETINS_UDP_ROLE_ACCEPTED
        && accepted_transport != NULL)
    {
        transport_result =
            netins_udp_v2_transport_server_connection_get_events(
                accepted_transport, &level_events);
    }
    if (transport_result == 0) {
        /*
         * Default-channel creation never holds data_lock while it performs a
         * potentially blocking transport operation. A normal lock therefore
         * gives get_events() a complete level snapshot without consuming a
         * notification on a failed trylock.
         */
        pthread_mutex_lock(&netins->data_lock);
        netins_udp_data_backend_t *data_backend =
            netins->data_backend;
        if (data_backend != NULL) {
            uint32_t data_events = 0;
            if (netins_udp_data_backend_get_events(data_backend,
                    &data_events) != NETINS_UDP_OK)
            {
                netins_udp_v2_sync_data_backend_error(netins,
                    data_backend, EIO,
                    "connection data channels failed");
                /*
                 * A private data-channel failure does not redefine the QUIC
                 * connection state. Wake both byte-stream directions so the
                 * next send/recv reports the stored backend error.
                 */
                level_events |= NETINS_UDP_EVENT_READABLE
                    | NETINS_UDP_EVENT_WRITABLE;
            } else {
                level_events |= data_events;
            }
            if (role == NETINS_UDP_ROLE_ACCEPTED
                && (level_events
                    & NETINS_UDP_EVENT_H3_REQUEST_ACCEPTABLE) != 0)
            {
                /*
                 * A nonblocking server may have accepted only part of the
                 * private lane set. The next lane is progress for either
                 * public byte-stream direction.
                 */
                level_events |= NETINS_UDP_EVENT_READABLE
                    | NETINS_UDP_EVENT_WRITABLE;
            }
        } else if (role == NETINS_UDP_ROLE_CLIENT
            && state == NETINS_UDP_STATE_CONNECTED)
        {
            /*
             * The client can create its lazy default data channel on the
             * first send/recv call, so it is connection-level writable.
             */
            level_events |= NETINS_UDP_EVENT_WRITABLE;

        } else if (role == NETINS_UDP_ROLE_ACCEPTED
            && (level_events
                & NETINS_UDP_EVENT_H3_REQUEST_ACCEPTABLE) != 0)
        {
            /*
             * A peer data request is ready to be bound. Wake simple API
             * users for either direction without hiding the advanced H3
             * accept event from applications that explicitly use it.
             */
            level_events |= NETINS_UDP_EVENT_READABLE
                | NETINS_UDP_EVENT_WRITABLE;
        }
        pthread_mutex_unlock(&netins->data_lock);
    }
    int result = NETINS_UDP_OK;
    if (transport_result != 0) {
        result = netins_udp_v2_fail(netins,
            errno != 0 ? errno : EIO,
            "failed to query connection readiness");
    } else if (netins_udp_v2_notifier_consume(&netins->notifier,
            level_generation, level_events, out_events) != 0)
    {
        result = netins_udp_v2_fail(netins,
            errno != 0 ? errno : EIO,
            "failed to acknowledge connection notification");
    }
    if (result == NETINS_UDP_OK
        && (*out_events & NETINS_UDP_EVENT_PATH_CHANGED) != 0)
    {
        netins_udp_v2_log_connection_summary(netins);
    }
    netins_udp_v2_end_call(netins);
    return result;
}

int
netins_udp_get_last_error_code(netins_udp_t *netins)
{
    if (netins == NULL) {
        return EINVAL;
    }
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return EBADF;
    }
    /*
     * Asynchronous transport failures may be the reason the caller is asking
     * for the error in the first place. Synchronize them here instead of
     * requiring a preceding get_state()/get_events() call.
     */
    (void)netins_udp_v2_refresh_state(netins);
    pthread_mutex_lock(&netins->lock);
    int error_code = netins->last_error_code;
    pthread_mutex_unlock(&netins->lock);
    netins_udp_v2_end_call(netins);
    return error_code;
}

int
netins_udp_get_last_error_msg(netins_udp_t *netins,
    char *buffer, size_t buffer_len)
{
    if (netins == NULL || buffer == NULL || buffer_len == 0) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    (void)netins_udp_v2_refresh_state(netins);
    pthread_mutex_lock(&netins->lock);
    snprintf(buffer, buffer_len, "%s", netins->last_error_msg);
    pthread_mutex_unlock(&netins->lock);
    netins_udp_v2_end_call(netins);
    return NETINS_UDP_OK;
}

/*
 * Public advanced HTTP/3 requests and the connection-level byte API are two
 * application modes. The byte backend opens its private requests through the
 * internal entry points, so public mode selection remains simple and stable.
 */
int
netins_udp_v2_connection_select_h3(netins_udp_t *netins)
{
    if (netins == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }

    int conflict = 0;
    pthread_mutex_lock(&netins->data_lock);
    if (netins->data_api_selected) {
        conflict = 1;
    } else {
        netins->h3_api_selected = 1;
    }
    pthread_mutex_unlock(&netins->data_lock);

    if (conflict) {
        return netins_udp_v2_fail(netins, EBUSY,
            "connection already uses another application data API");
    }
    return NETINS_UDP_OK;
}

int
netins_udp_v2_connection_acquire_h3(
    netins_udp_t *netins,
    netins_udp_v2_h3_parent_snapshot_t *out)
{
    if (out == NULL) {
        if (netins != NULL) {
            return netins_udp_v2_fail(netins, EINVAL,
                "H3 parent snapshot must not be NULL");
        }
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    memset(out, 0, sizeof(*out));
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    int error_code = 0;
    const char *message = NULL;
    if (netins->state != NETINS_UDP_STATE_CONNECTED
        || netins->client_transport == NULL)
    {
        error_code = ENOTCONN;
        message = "HTTP/3 requests require a connected handle";

    } else if (netins->role != NETINS_UDP_ROLE_CLIENT) {
        error_code = EOPNOTSUPP;
        message = "request_open is valid only on a client connection";

    } else if (netins->app_protocol != NETINS_UDP_APP_HTTP3) {
        error_code = EOPNOTSUPP;
        message = "HTTP/3 request APIs require APP_PROTOCOL=HTTP3";
    }
    if (error_code == 0) {
        out->transport = netins->client_transport;
        out->nonblock = netins->nonblock;
        out->send_timeout_ms = netins->send_timeout_ms;
        out->recv_timeout_ms = netins->recv_timeout_ms;
        out->close_timeout_ms = netins->close_timeout_ms;
    } else {
        netins_udp_v2_set_error_locked(netins, error_code, message);
    }
    pthread_mutex_unlock(&netins->lock);

    if (error_code != 0) {
        errno = error_code;
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }
    return NETINS_UDP_OK;
}

int
netins_udp_v2_connection_acquire_h3_server(
    netins_udp_t *netins,
    netins_udp_v2_h3_server_parent_snapshot_t *out)
{
    if (out == NULL) {
        if (netins != NULL) {
            return netins_udp_v2_fail(netins, EINVAL,
                "server H3 parent snapshot must not be NULL");
        }
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    memset(out, 0, sizeof(*out));
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    int error_code = 0;
    const char *message = NULL;
    if (netins->role != NETINS_UDP_ROLE_ACCEPTED) {
        error_code = EOPNOTSUPP;
        message = "request_accept is valid only on an accepted connection";

    } else if (netins->state != NETINS_UDP_STATE_CONNECTED
        || netins->accepted_transport == NULL)
    {
        error_code = ENOTCONN;
        message = "request_accept requires a connected server handle";

    } else if (netins->app_protocol != NETINS_UDP_APP_HTTP3) {
        error_code = EOPNOTSUPP;
        message = "HTTP/3 request APIs require APP_PROTOCOL=HTTP3";
    }
    if (error_code == 0) {
        out->transport = netins->accepted_transport;
        out->nonblock = netins->nonblock;
        out->send_timeout_ms = netins->send_timeout_ms;
        out->recv_timeout_ms = netins->recv_timeout_ms;
        out->close_timeout_ms = netins->close_timeout_ms;
    } else {
        netins_udp_v2_set_error_locked(netins, error_code, message);
    }
    pthread_mutex_unlock(&netins->lock);

    if (error_code != 0) {
        errno = error_code;
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }
    return NETINS_UDP_OK;
}

void
netins_udp_v2_connection_release(netins_udp_t *netins)
{
    if (netins != NULL) {
        netins_udp_v2_end_call(netins);
    }
}

void
netins_udp_v2_connection_set_error(netins_udp_t *netins,
    int error_code, const char *message)
{
    if (netins == NULL) {
        return;
    }
    pthread_mutex_lock(&netins->lock);
    netins_udp_v2_set_error_locked(netins, error_code, message);
    pthread_mutex_unlock(&netins->lock);
    if (error_code != 0) {
        errno = error_code;
    }
}

int
netins_udp_v2_child_handle_acquire(void)
{
    pthread_mutex_lock(&g_netins_udp_v2_lock);
    if (g_netins_udp_v2_init_refs == 0) {
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = EPERM;
        return NETINS_UDP_ERROR;
    }
    if (g_netins_udp_v2_handle_count == UINT32_MAX) {
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = EOVERFLOW;
        return NETINS_UDP_ERROR;
    }
    g_netins_udp_v2_handle_count++;
    pthread_mutex_unlock(&g_netins_udp_v2_lock);
    return NETINS_UDP_OK;
}

void
netins_udp_v2_child_handle_release(void)
{
    pthread_mutex_lock(&g_netins_udp_v2_lock);
    if (g_netins_udp_v2_handle_count > 0) {
        g_netins_udp_v2_handle_count--;
    }
    pthread_mutex_unlock(&g_netins_udp_v2_lock);
}

void
netins_udp_close(netins_udp_t *netins)
{
    if (netins == NULL) {
        return;
    }

    pthread_mutex_lock(&netins->lock);
    netins->closing = 1;
    if (netins->state != NETINS_UDP_STATE_ERROR
        && netins->state != NETINS_UDP_STATE_CLOSED)
    {
        netins->state = NETINS_UDP_STATE_CLOSING;
    }
    netins_udp_v2_transport_client_t *client_transport =
        netins->client_transport;
    netins_udp_v2_transport_server_t *listener_transport =
        netins->listener_transport;
    netins_udp_v2_transport_server_connection_t *accepted_transport =
        netins->accepted_transport;
    uint32_t close_timeout_ms = netins->close_timeout_ms;
    pthread_mutex_unlock(&netins->lock);

    if (client_transport != NULL) {
        netins_udp_v2_transport_client_set_notify(client_transport,
            NULL, NULL);
        netins_udp_v2_transport_client_request_close(client_transport);
    }
    if (listener_transport != NULL) {
        netins_udp_v2_transport_server_set_notify(listener_transport,
            NULL, NULL);
        netins_udp_v2_transport_server_begin_close(listener_transport);
    }
    if (accepted_transport != NULL) {
        netins_udp_v2_transport_server_connection_set_notify(
            accepted_transport, NULL, NULL);
        netins_udp_v2_transport_server_connection_request_shutdown(
            accepted_transport, 0);
    }

    pthread_mutex_lock(&netins->lock);
    while (netins->active_calls != 0 || netins->active_logs != 0) {
        pthread_cond_wait(&netins->idle_cond, &netins->lock);
    }
    client_transport = netins->client_transport;
    listener_transport = netins->listener_transport;
    accepted_transport = netins->accepted_transport;
    netins->client_transport = NULL;
    netins->listener_transport = NULL;
    netins->accepted_transport = NULL;
    netins->state = NETINS_UDP_STATE_CLOSED;
    pthread_mutex_unlock(&netins->lock);

    pthread_mutex_lock(&netins->data_lock);
    netins_udp_data_backend_t *data_backend = netins->data_backend;
    netins->data_backend = NULL;
    pthread_mutex_unlock(&netins->data_lock);

    /*
     * Child request handles refer to transport-owned request objects. Release
     * every private lane while its parent transport is still alive. A prior
     * graceful shutdown has already flushed the backend; direct close is an
     * abort and may intentionally discard unsent application bytes.
     */
    netins_udp_data_backend_destroy(data_backend);
    if (client_transport != NULL) {
        netins_udp_v2_transport_client_close(client_transport);
    }
    if (listener_transport != NULL) {
        netins_udp_v2_transport_server_close(listener_transport);
    }
    if (accepted_transport != NULL) {
        netins_udp_v2_transport_server_connection_close(
            accepted_transport, close_timeout_ms);
    }

    netins_udp_v2_log(netins, NETINS_UDP_LOG_LEVEL_INFO,
        NETINS_UDP_V2_LOGGED_CLOSED, "NetIns UDP handle closed");
    netins_udp_v2_notifier_destroy(&netins->notifier);
    pthread_cond_destroy(&netins->data_cond);
    pthread_cond_destroy(&netins->idle_cond);
    pthread_mutex_destroy(&netins->data_lock);
    pthread_mutex_destroy(&netins->lock);
    free(netins);

    pthread_mutex_lock(&g_netins_udp_v2_lock);
    if (g_netins_udp_v2_handle_count > 0) {
        g_netins_udp_v2_handle_count--;
    }
    pthread_mutex_unlock(&g_netins_udp_v2_lock);
}
