#define _POSIX_C_SOURCE 200809L

#include "api/draft/netins_udp_v2.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netins_udp_v2_transport.h"

#define NETINS_UDP_V2_DEFAULT_CONNECT_TIMEOUT_MS 20000U
#define NETINS_UDP_V2_DEFAULT_CLOSE_TIMEOUT_MS    1000U
#define NETINS_UDP_V2_DEFAULT_SOCKET_BUFFER       (16 * 1024 * 1024)
#define NETINS_UDP_V2_DEFAULT_INITIAL_CWND        100U

_Static_assert(NETINS_UDP_MAX_PATHS
        == NETINS_UDP_V2_TRANSPORT_MAX_PATHS,
    "V2 public and transport path limits must match");
_Static_assert(NETINS_UDP_ADDRESS_LEN
        == NETINS_UDP_V2_TRANSPORT_HOST_LEN,
    "V2 public and transport host limits must match");
_Static_assert(NETINS_UDP_SERVER_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN,
    "V2 public and transport server-name limits must match");
_Static_assert(NETINS_UDP_INTERFACE_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_INTERFACE_NAME_LEN,
    "V2 public and transport interface-name limits must match");

struct netins_udp_s {
    pthread_mutex_t lock;
    pthread_cond_t idle_cond;
    size_t active_calls;
    int closing;
    int connect_attempted;

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
    netins_udp_interfaces_t interfaces;
    netins_udp_tls_config_t tls;

    netins_udp_v2_transport_client_t *transport;
    int last_error_code;
    char last_error_msg[NETINS_UDP_ERROR_MSG_LEN];
};

static pthread_mutex_t g_netins_udp_v2_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_netins_udp_v2_init_refs;
static uint32_t g_netins_udp_v2_handle_count;

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
netins_udp_v2_scalar_valid(const void *value, size_t value_len,
    size_t expected)
{
    return value != NULL && value_len == expected;
}

static int
netins_udp_v2_fixed_string_valid(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static int
netins_udp_v2_runtime_option(int option, int state)
{
    return option == NETINS_UDP_OPT_NONBLOCK
        || option == NETINS_UDP_OPT_SEND_TIMEOUT_MS
        || option == NETINS_UDP_OPT_RECV_TIMEOUT_MS
        || option == NETINS_UDP_OPT_CLOSE_TIMEOUT_MS
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
        && g_netins_udp_v2_handle_count != 0)
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
    init_error = pthread_cond_init(&netins->idle_cond, NULL);
    if (init_error != 0) {
        pthread_mutex_destroy(&netins->lock);
        free(netins);
        pthread_mutex_unlock(&g_netins_udp_v2_lock);
        errno = init_error;
        return NULL;
    }

    netins->requested_type = type;
    netins->role = NETINS_UDP_ROLE_NONE;
    netins->state = NETINS_UDP_STATE_NEW;
    netins->app_protocol = NETINS_UDP_APP_RAW_STREAM;
    netins->connect_timeout_ms =
        NETINS_UDP_V2_DEFAULT_CONNECT_TIMEOUT_MS;
    netins->close_timeout_ms = NETINS_UDP_V2_DEFAULT_CLOSE_TIMEOUT_MS;
    netins->kernel_sndbuf = NETINS_UDP_V2_DEFAULT_SOCKET_BUFFER;
    netins->kernel_rcvbuf = NETINS_UDP_V2_DEFAULT_SOCKET_BUFFER;
    netins->initial_cwnd_packets = NETINS_UDP_V2_DEFAULT_INITIAL_CWND;
    netins->interfaces = (netins_udp_interfaces_t)
        NETINS_UDP_INTERFACES_INITIALIZER;
    netins_udp_tls_config_init(&netins->tls);

    g_netins_udp_v2_handle_count++;
    pthread_mutex_unlock(&g_netins_udp_v2_lock);
    return netins;
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
        if (!netins_udp_v2_scalar_valid(value, value_len, sizeof(int))) {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "NONBLOCK requires an int");
            break;
        }
        int enabled = *(const int *)value;
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
        if (!netins_udp_v2_scalar_valid(value, value_len,
                sizeof(uint32_t)))
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "timeout option requires uint32_t");
            break;
        }
        uint32_t timeout = *(const uint32_t *)value;
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
            transport_to_update = netins->transport;
            close_timeout_to_update = timeout;
        } else {
            netins->idle_timeout_ms = timeout;
        }
        break;
    }
    case NETINS_UDP_OPT_KERNEL_SNDBUF:
    case NETINS_UDP_OPT_KERNEL_RCVBUF: {
        if (!netins_udp_v2_scalar_valid(value, value_len, sizeof(int))
            || *(const int *)value <= 0)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "kernel socket buffer requires a positive int");
            break;
        }
        if (option == NETINS_UDP_OPT_KERNEL_SNDBUF) {
            netins->kernel_sndbuf = *(const int *)value;
        } else {
            netins->kernel_rcvbuf = *(const int *)value;
        }
        break;
    }
    case NETINS_UDP_OPT_INTERFACES: {
        if (!netins_udp_v2_scalar_valid(value, value_len,
                sizeof(netins_udp_interfaces_t)))
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "INTERFACES requires netins_udp_interfaces_t");
            break;
        }
        const netins_udp_interfaces_t *interfaces = value;
        if (interfaces->struct_size != sizeof(*interfaces)
            || interfaces->api_version != NETINS_UDP_API_VERSION
            || interfaces->interface_count > NETINS_UDP_MAX_PATHS)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EPROTO,
                "INTERFACES structure version or count is invalid");
            break;
        }
        for (uint32_t i = 0; i < interfaces->interface_count; ++i) {
            if (!netins_udp_v2_fixed_string_valid(interfaces->names[i],
                    sizeof(interfaces->names[i]))
                || interfaces->names[i][0] == '\0')
            {
                result = netins_udp_v2_set_option_error_locked(netins,
                    EINVAL, "interface names must be nonempty strings");
                break;
            }
        }
        if (result == NETINS_UDP_OK) {
            netins->interfaces = *interfaces;
        }
        break;
    }
    case NETINS_UDP_OPT_APP_PROTOCOL: {
        if (!netins_udp_v2_scalar_valid(value, value_len, sizeof(int))) {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "APP_PROTOCOL requires an int");
            break;
        }
        int protocol = *(const int *)value;
        if (protocol != NETINS_UDP_APP_RAW_STREAM
            && protocol != NETINS_UDP_APP_HTTP3)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "unknown application protocol");
            break;
        }
        netins->app_protocol = protocol;
        break;
    }
    case NETINS_UDP_OPT_TLS: {
        if (!netins_udp_v2_scalar_valid(value, value_len,
                sizeof(netins_udp_tls_config_t)))
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "TLS requires netins_udp_tls_config_t");
            break;
        }
        const netins_udp_tls_config_t *tls = value;
        if (tls->struct_size != sizeof(*tls)
            || tls->api_version != NETINS_UDP_API_VERSION
            || (tls->verify_peer != 0 && tls->verify_peer != 1)
            || (tls->require_client_certificate != 0
                && tls->require_client_certificate != 1)
            || !netins_udp_v2_fixed_string_valid(tls->server_name,
                sizeof(tls->server_name))
            || !netins_udp_v2_fixed_string_valid(tls->certificate_file,
                sizeof(tls->certificate_file))
            || !netins_udp_v2_fixed_string_valid(tls->private_key_file,
                sizeof(tls->private_key_file))
            || !netins_udp_v2_fixed_string_valid(tls->ca_file,
                sizeof(tls->ca_file)))
        {
            result = netins_udp_v2_set_option_error_locked(netins, EPROTO,
                "TLS structure is invalid");
            break;
        }
        netins->tls = *tls;
        break;
    }
    case NETINS_UDP_OPT_INITIAL_CWND_PACKETS: {
        if (!netins_udp_v2_scalar_valid(value, value_len,
                sizeof(uint32_t))
            || *(const uint32_t *)value == 0)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "INITIAL_CWND_PACKETS requires a nonzero uint32_t");
            break;
        }
        netins->initial_cwnd_packets = *(const uint32_t *)value;
        break;
    }
    case NETINS_UDP_OPT_SCHEDULER: {
        if (!netins_udp_v2_scalar_valid(value, value_len, sizeof(int))
            || *(const int *)value < NETINS_UDP_SCHEDULER_DEFAULT
            || *(const int *)value > NETINS_UDP_SCHEDULER_ADAPTIVE)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "unknown scheduler");
            break;
        }
        netins->scheduler = *(const int *)value;
        break;
    }
    case NETINS_UDP_OPT_CONGESTION_CONTROL: {
        if (!netins_udp_v2_scalar_valid(value, value_len, sizeof(int))
            || *(const int *)value < NETINS_UDP_CC_DEFAULT
            || *(const int *)value > NETINS_UDP_CC_RENO)
        {
            result = netins_udp_v2_set_option_error_locked(netins, EINVAL,
                "unknown congestion controller");
            break;
        }
        netins->congestion_control = *(const int *)value;
        break;
    }
    case NETINS_UDP_OPT_SOCKOPT:
        result = netins_udp_v2_set_option_error_locked(netins,
            EOPNOTSUPP, "SOCKOPT is not implemented in the client lifecycle stage");
        break;
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
    case NETINS_UDP_OPT_SOCKOPT: {
        pthread_mutex_unlock(&netins->lock);
        int sockopt_result = netins_udp_v2_fail(netins, EOPNOTSUPP,
            "SOCKOPT is set-only");
        netins_udp_v2_end_call(netins);
        return sockopt_result;
    }
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

    pthread_mutex_lock(&netins->lock);
    if (netins->app_protocol != NETINS_UDP_APP_HTTP3) {
        pthread_mutex_unlock(&netins->lock);
        return netins_udp_v2_fail(netins, EOPNOTSUPP,
            "RAW_STREAM transport is deferred to the send/receive stage; set APP_PROTOCOL=HTTP3");
    }
    if (netins->tls.require_client_certificate
        || netins->tls.certificate_file[0] != '\0'
        || netins->tls.private_key_file[0] != '\0'
        || netins->tls.ca_file[0] != '\0')
    {
        pthread_mutex_unlock(&netins->lock);
        return netins_udp_v2_fail(netins, EOPNOTSUPP,
            "custom client certificates and CA files are not supported by the current backend");
    }

    memset(config, 0, sizeof(*config));
    memcpy(config->host, host, host_len + 1);
    config->port = (uint16_t)port;
    const char *server_name = netins->tls.server_name[0] != '\0'
        ? netins->tls.server_name : host;
    snprintf(config->server_name, sizeof(config->server_name), "%s",
        server_name);
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
        || netins->connect_attempted)
    {
        netins_udp_v2_set_error_locked(netins, EALREADY,
            "connect can only be called once");
        pthread_mutex_unlock(&netins->lock);
        errno = EALREADY;
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }
    netins->connect_attempted = 1;
    netins->role = NETINS_UDP_ROLE_CLIENT;
    netins->state = NETINS_UDP_STATE_CONNECTING;
    int nonblock = netins->nonblock;
    uint32_t connect_timeout_ms = netins->connect_timeout_ms;
    pthread_mutex_unlock(&netins->lock);

    netins_udp_v2_transport_config_t config;
    if (netins_udp_v2_prepare_transport_config(netins, host, port,
            &config) != NETINS_UDP_OK)
    {
        pthread_mutex_lock(&netins->lock);
        if (!netins->closing) {
            netins->state = NETINS_UDP_STATE_ERROR;
        }
        pthread_mutex_unlock(&netins->lock);
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
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    netins->transport = transport;
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
        netins_udp_v2_end_call(netins);
        return NETINS_UDP_ERROR;
    }

    if (nonblock) {
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
    netins_udp_v2_end_call(netins);
    return NETINS_UDP_OK;
}

int
netins_udp_get_state(netins_udp_t *netins)
{
    if (netins_udp_v2_begin_call(netins) != NETINS_UDP_OK) {
        return NETINS_UDP_STATE_ERROR;
    }

    pthread_mutex_lock(&netins->lock);
    int state = netins->state;
    netins_udp_v2_transport_client_t *transport = netins->transport;
    int preserve_terminal = state == NETINS_UDP_STATE_ERROR
        || netins->closing;
    pthread_mutex_unlock(&netins->lock);

    if (transport != NULL && !preserve_terminal) {
        int transport_state =
            netins_udp_v2_transport_client_get_state(transport);
        int next_state =
            netins_udp_v2_transport_state_to_public(transport_state);
        if (state == NETINS_UDP_STATE_CONNECTING
            && next_state == NETINS_UDP_STATE_NEW)
        {
            next_state = NETINS_UDP_STATE_CONNECTING;
        }
        if (next_state == NETINS_UDP_STATE_ERROR) {
            netins_udp_v2_sync_transport_error(netins, transport);
        }
        pthread_mutex_lock(&netins->lock);
        if (!netins->closing && netins->state != NETINS_UDP_STATE_ERROR) {
            netins->state = next_state;
        }
        state = netins->state;
        pthread_mutex_unlock(&netins->lock);
    }

    netins_udp_v2_end_call(netins);
    return state;
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
    pthread_mutex_lock(&netins->lock);
    snprintf(buffer, buffer_len, "%s", netins->last_error_msg);
    pthread_mutex_unlock(&netins->lock);
    netins_udp_v2_end_call(netins);
    return NETINS_UDP_OK;
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
    netins_udp_v2_transport_client_t *transport = netins->transport;
    pthread_mutex_unlock(&netins->lock);

    if (transport != NULL) {
        netins_udp_v2_transport_client_request_close(transport);
    }

    pthread_mutex_lock(&netins->lock);
    while (netins->active_calls != 0) {
        pthread_cond_wait(&netins->idle_cond, &netins->lock);
    }
    transport = netins->transport;
    netins->transport = NULL;
    netins->state = NETINS_UDP_STATE_CLOSED;
    pthread_mutex_unlock(&netins->lock);

    if (transport != NULL) {
        netins_udp_v2_transport_client_close(transport);
    }

    pthread_cond_destroy(&netins->idle_cond);
    pthread_mutex_destroy(&netins->lock);
    free(netins);

    pthread_mutex_lock(&g_netins_udp_v2_lock);
    if (g_netins_udp_v2_handle_count > 0) {
        g_netins_udp_v2_handle_count--;
    }
    pthread_mutex_unlock(&g_netins_udp_v2_lock);
}
