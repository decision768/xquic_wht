#include "netins_udp_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t netins_udp_global_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int netins_udp_init_references;
static unsigned int netins_udp_handle_count;

static int
netins_udp_valid(const netins_udp_t *handle)
{
    return handle != NULL && handle->magic == NETINS_UDP_HANDLE_MAGIC;
}

static int
netins_udp_set_error(netins_udp_t *handle, int error_code,
    const char *format, ...)
{
    if (error_code == 0) {
        error_code = EIO;
    }
    if (netins_udp_valid(handle)) {
        handle->last_error_code = error_code;
        va_list args;
        va_start(args, format);
        vsnprintf(handle->last_error_msg, sizeof(handle->last_error_msg),
            format != NULL ? format : "NetIns UDP error", args);
        va_end(args);
    }
    errno = error_code;
    return -1;
}

static void
netins_udp_clear_error(netins_udp_t *handle)
{
    handle->last_error_code = 0;
    handle->last_error_msg[0] = '\0';
}

static const char *
netins_udp_scheduler_name(int scheduler)
{
    switch (scheduler) {
    case NETINS_UDP_SCHEDULER_BALANCED:
        return "balanced";
    case NETINS_UDP_SCHEDULER_MINRTT:
        return "minrtt";
    case NETINS_UDP_SCHEDULER_BACKUP:
        return "backup";
    case NETINS_UDP_SCHEDULER_RAP:
        return "rap";
    case NETINS_UDP_SCHEDULER_ACT:
        return "act";
    case NETINS_UDP_SCHEDULER_BANDWIDTH:
        return "bw";
    case NETINS_UDP_SCHEDULER_FASTPROBE:
        return "fastprobe";
    case NETINS_UDP_SCHEDULER_ADAPTIVE:
        return "adaptive";
    case NETINS_UDP_SCHEDULER_FASTPROBE5:
        return "fastprobe5";
    case NETINS_UDP_SCHEDULER_FASTPROBE10:
        return "fastprobe10";
    case NETINS_UDP_SCHEDULER_FASTPROBE20:
        return "fastprobe20";
    case NETINS_UDP_SCHEDULER_FASTONLY:
        return "fastonly";
    default:
        return NULL;
    }
}

uint32_t
netins_udp_get_api_version(void)
{
    return NETINS_UDP_API_VERSION;
}

int
netins_udp_init(void)
{
    int error_code = 0;
    pthread_mutex_lock(&netins_udp_global_lock);
    if (netins_udp_init_references == UINT_MAX) {
        error_code = EOVERFLOW;
    } else {
        netins_udp_init_references++;
    }
    pthread_mutex_unlock(&netins_udp_global_lock);
    if (error_code != 0) {
        errno = error_code;
        return -1;
    }
    return NETINS_UDP_OK;
}

int
netins_udp_cleanup(void)
{
    int error_code = 0;
    pthread_mutex_lock(&netins_udp_global_lock);
    if (netins_udp_init_references == 0) {
        error_code = EINVAL;
    } else if (netins_udp_init_references == 1
        && netins_udp_handle_count != 0)
    {
        error_code = EBUSY;
    } else {
        netins_udp_init_references--;
    }
    pthread_mutex_unlock(&netins_udp_global_lock);
    if (error_code != 0) {
        errno = error_code;
        return -1;
    }
    return 0;
}

netins_udp_t *
netins_udp_new(int role)
{
    if (role != NETINS_UDP_ROLE_CLIENT && role != NETINS_UDP_ROLE_SERVER) {
        errno = EINVAL;
        return NULL;
    }

    netins_udp_t *handle = calloc(1, sizeof(*handle));
    if (handle == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    pthread_mutex_lock(&netins_udp_global_lock);
    if (netins_udp_init_references == 0) {
        pthread_mutex_unlock(&netins_udp_global_lock);
        free(handle);
        errno = EPERM;
        return NULL;
    }
    if (netins_udp_handle_count == UINT_MAX) {
        pthread_mutex_unlock(&netins_udp_global_lock);
        free(handle);
        errno = EOVERFLOW;
        return NULL;
    }
    netins_udp_handle_count++;
    pthread_mutex_unlock(&netins_udp_global_lock);

    handle->magic = NETINS_UDP_HANDLE_MAGIC;
    handle->role = role;
    handle->state = NETINS_UDP_STATE_NEW;
    handle->connect_timeout_ms = 10000;
    handle->accept_timeout_ms = 0;
    handle->close_timeout_ms = 3000;
    mini_backend_config_init(&handle->backend_config);
    handle->backend_config.load_config_file = 0;
    handle->backend_config.api_mode = 1;

    return handle;
}

static int
netins_udp_set_u32_option(netins_udp_t *handle, const void *value,
    size_t value_len, uint32_t *destination)
{
    if (value == NULL || value_len != sizeof(uint32_t)) {
        return netins_udp_set_error(handle, EINVAL,
            "option requires a uint32_t value");
    }
    *destination = *(const uint32_t *)value;
    return 0;
}

int
netins_udp_set_opt(netins_udp_t *handle, int option,
    const void *value, size_t value_len)
{
    if (!netins_udp_valid(handle)) {
        errno = EINVAL;
        return -1;
    }
    if (handle->state != NETINS_UDP_STATE_NEW) {
        return netins_udp_set_error(handle, EBUSY,
            "options must be set before connect or listen");
    }

    switch (option) {
    case NETINS_UDP_OPT_INTERFACES: {
        if (handle->role != NETINS_UDP_ROLE_CLIENT) {
            return netins_udp_set_error(handle, EOPNOTSUPP,
                "interfaces are only configurable on clients");
        }
        if (value == NULL || value_len < sizeof(netins_udp_interfaces_t)) {
            return netins_udp_set_error(handle, EINVAL,
                "invalid interfaces option size");
        }
        const netins_udp_interfaces_t *interfaces = value;
        if (interfaces->struct_size < sizeof(*interfaces)
            || interfaces->api_version != NETINS_UDP_API_VERSION
            || interfaces->interface_count == 0
            || interfaces->interface_count > NETINS_UDP_MAX_INTERFACES)
        {
            return netins_udp_set_error(handle, EINVAL,
                "invalid interface list");
        }
        handle->backend_config.has_interfaces = 1;
        handle->backend_config.interface_count =
            (int)interfaces->interface_count;
        memset(handle->backend_config.interfaces, 0,
            sizeof(handle->backend_config.interfaces));
        for (uint32_t i = 0; i < interfaces->interface_count; i++) {
            if (interfaces->names[i][0] == '\0') {
                return netins_udp_set_error(handle, EINVAL,
                    "interface name must not be empty");
            }
            strncpy(handle->backend_config.interfaces[i],
                interfaces->names[i],
                sizeof(handle->backend_config.interfaces[i]) - 1);
        }
        break;
    }
    case NETINS_UDP_OPT_SCHEDULER: {
        if (value == NULL || value_len != sizeof(int)) {
            return netins_udp_set_error(handle, EINVAL,
                "scheduler option requires an int");
        }
        int scheduler = *(const int *)value;
        if (scheduler == NETINS_UDP_SCHEDULER_DEFAULT) {
            handle->backend_config.has_scheduler = 0;
            handle->backend_config.scheduler[0] = '\0';
            break;
        }
        const char *name = netins_udp_scheduler_name(scheduler);
        if (name == NULL) {
            return netins_udp_set_error(handle, EINVAL,
                "unsupported scheduler");
        }
        handle->backend_config.has_scheduler = 1;
        strncpy(handle->backend_config.scheduler, name,
            sizeof(handle->backend_config.scheduler) - 1);
        break;
    }
    case NETINS_UDP_OPT_CONGESTION_CONTROL: {
        if (value == NULL || value_len != sizeof(int)) {
            return netins_udp_set_error(handle, EINVAL,
                "congestion control option requires an int");
        }
        int cc = *(const int *)value;
        if (cc != NETINS_UDP_CC_DEFAULT && cc != NETINS_UDP_CC_BBR
            && cc != NETINS_UDP_CC_CUBIC)
        {
            return netins_udp_set_error(handle, EINVAL,
                "unsupported congestion control");
        }
        handle->backend_config.has_congestion_control =
            cc != NETINS_UDP_CC_DEFAULT;
        handle->backend_config.congestion_control =
            cc == NETINS_UDP_CC_CUBIC
            ? MINI_BACKEND_CC_CUBIC : MINI_BACKEND_CC_BBR;
        break;
    }
    case NETINS_UDP_OPT_TLS: {
        if (value == NULL || value_len < sizeof(netins_udp_tls_config_t)) {
            return netins_udp_set_error(handle, EINVAL,
                "invalid TLS option size");
        }
        const netins_udp_tls_config_t *tls = value;
        if (tls->struct_size < sizeof(*tls)
            || tls->api_version != NETINS_UDP_API_VERSION)
        {
            return netins_udp_set_error(handle, EINVAL,
                "invalid TLS option version");
        }
        if (handle->role == NETINS_UDP_ROLE_SERVER
            && (tls->certificate_file[0] == '\0'
                || tls->private_key_file[0] == '\0'))
        {
            return netins_udp_set_error(handle, EINVAL,
                "server TLS certificate and private key are required");
        }
        if (handle->role == NETINS_UDP_ROLE_CLIENT
            && (tls->certificate_file[0] != '\0'
                || tls->private_key_file[0] != '\0'))
        {
            return netins_udp_set_error(handle, EOPNOTSUPP,
                "client certificates are not supported");
        }
        handle->backend_config.has_tls = 1;
        strncpy(handle->backend_config.tls_server_name, tls->server_name,
            sizeof(handle->backend_config.tls_server_name) - 1);
        strncpy(handle->backend_config.certificate_file,
            tls->certificate_file,
            sizeof(handle->backend_config.certificate_file) - 1);
        strncpy(handle->backend_config.private_key_file,
            tls->private_key_file,
            sizeof(handle->backend_config.private_key_file) - 1);
        break;
    }
    case NETINS_UDP_OPT_CONNECT_TIMEOUT_MS:
        if (handle->role != NETINS_UDP_ROLE_CLIENT) {
            return netins_udp_set_error(handle, EOPNOTSUPP,
                "connect timeout is only configurable on clients");
        }
        if (netins_udp_set_u32_option(handle, value, value_len,
                &handle->connect_timeout_ms) != 0)
        {
            return -1;
        }
        if (handle->connect_timeout_ms == 0) {
            return netins_udp_set_error(handle, EINVAL,
                "connect timeout must be nonzero");
        }
        break;
    case NETINS_UDP_OPT_ACCEPT_TIMEOUT_MS:
        if (handle->role != NETINS_UDP_ROLE_SERVER) {
            return netins_udp_set_error(handle, EOPNOTSUPP,
                "accept timeout is only configurable on servers");
        }
        if (netins_udp_set_u32_option(handle, value, value_len,
                &handle->accept_timeout_ms) != 0)
        {
            return -1;
        }
        break;
    case NETINS_UDP_OPT_CLOSE_TIMEOUT_MS:
        if (netins_udp_set_u32_option(handle, value, value_len,
                &handle->close_timeout_ms) != 0)
        {
            return -1;
        }
        break;
    case NETINS_UDP_OPT_KERNEL_SNDBUF:
    case NETINS_UDP_OPT_KERNEL_RCVBUF: {
        if (value == NULL || value_len != sizeof(int)
            || *(const int *)value <= 0)
        {
            return netins_udp_set_error(handle, EINVAL,
                "socket buffer option requires a positive int");
        }
        if (option == NETINS_UDP_OPT_KERNEL_SNDBUF) {
            handle->backend_config.has_kernel_sndbuf = 1;
            handle->backend_config.kernel_sndbuf = *(const int *)value;
        } else {
            handle->backend_config.has_kernel_rcvbuf = 1;
            handle->backend_config.kernel_rcvbuf = *(const int *)value;
        }
        break;
    }
    default:
        return netins_udp_set_error(handle, ENOPROTOOPT,
            "unsupported option");
    }
    netins_udp_clear_error(handle);
    return 0;
}

int
netins_udp_connect(netins_udp_t *client, const char *host, uint16_t port)
{
    struct in_addr ipv4;
    if (!netins_udp_valid(client) || client->role != NETINS_UDP_ROLE_CLIENT
        || client->state != NETINS_UDP_STATE_NEW || host == NULL
        || host[0] == '\0' || port == 0
        || inet_pton(AF_INET, host, &ipv4) != 1)
    {
        return netins_udp_set_error(client, EINVAL,
            "invalid client, host, port, or state");
    }

    client->state = NETINS_UDP_STATE_CONNECTING;
    int ret = mini_backend_client_create(&client->backend_config,
        &client->client_backend);
    if (ret == 0) {
        ret = mini_backend_client_connect(client->client_backend, host, port,
            client->connect_timeout_ms);
    }
    if (ret != 0) {
        client->state = NETINS_UDP_STATE_ERROR;
        return netins_udp_set_error(client, ret,
            "Mini backend client connection failed");
    }
    client->state = NETINS_UDP_STATE_CONNECTED;
    netins_udp_clear_error(client);
    return 0;
}

int
netins_udp_listen(netins_udp_t *server, const char *host, uint16_t port)
{
    struct in_addr ipv4;
    if (!netins_udp_valid(server) || server->role != NETINS_UDP_ROLE_SERVER
        || server->state != NETINS_UDP_STATE_NEW || host == NULL
        || host[0] == '\0' || port == 0
        || inet_pton(AF_INET, host, &ipv4) != 1)
    {
        return netins_udp_set_error(server, EINVAL,
            "invalid server, host, port, or state");
    }
    if (!server->backend_config.has_tls) {
        return netins_udp_set_error(server, EINVAL,
            "server TLS certificate and private key must be configured");
    }

    int ret = mini_backend_server_create(&server->backend_config,
        &server->server_backend);
    if (ret == 0) {
        ret = mini_backend_server_listen(server->server_backend, host, port);
    }
    if (ret != 0) {
        server->state = NETINS_UDP_STATE_ERROR;
        return netins_udp_set_error(server, ret,
            "Mini backend server listen failed");
    }
    server->state = NETINS_UDP_STATE_LISTENING;
    netins_udp_clear_error(server);
    return 0;
}

netins_udp_t *
netins_udp_accept(netins_udp_t *server)
{
    if (!netins_udp_valid(server) || server->role != NETINS_UDP_ROLE_SERVER
        || server->state != NETINS_UDP_STATE_LISTENING
        || server->server_backend == NULL)
    {
        netins_udp_set_error(server, EINVAL,
            "handle is not a listening Mini server");
        return NULL;
    }

    mini_backend_server_connection_t *connection = NULL;
    int ret = mini_backend_server_accept(server->server_backend,
        server->accept_timeout_ms, &connection);
    if (ret != 0) {
        netins_udp_set_error(server, ret, "Mini backend accept failed");
        return NULL;
    }

    netins_udp_t *accepted = calloc(1, sizeof(*accepted));
    if (accepted == NULL) {
        (void)mini_backend_server_connection_shutdown(server->server_backend,
            connection, server->close_timeout_ms);
        mini_backend_server_connection_release(server->server_backend,
            connection);
        netins_udp_set_error(server, ENOMEM,
            "allocate accepted connection failed");
        return NULL;
    }

    int handle_limit_reached = 0;
    pthread_mutex_lock(&netins_udp_global_lock);
    if (netins_udp_handle_count == UINT_MAX) {
        handle_limit_reached = 1;
    } else {
        netins_udp_handle_count++;
    }
    pthread_mutex_unlock(&netins_udp_global_lock);
    if (handle_limit_reached) {
        free(accepted);
        (void)mini_backend_server_connection_shutdown(server->server_backend,
            connection, server->close_timeout_ms);
        mini_backend_server_connection_release(server->server_backend,
            connection);
        netins_udp_set_error(server, EOVERFLOW,
            "Mini API handle limit reached");
        return NULL;
    }

    accepted->magic = NETINS_UDP_HANDLE_MAGIC;
    accepted->role = NETINS_UDP_ROLE_ACCEPTED;
    accepted->state = NETINS_UDP_STATE_CONNECTED;
    accepted->connect_timeout_ms = server->connect_timeout_ms;
    accepted->accept_timeout_ms = server->accept_timeout_ms;
    accepted->close_timeout_ms = server->close_timeout_ms;
    accepted->backend_config = server->backend_config;
    accepted->server_backend = server->server_backend;
    accepted->server_connection = connection;
    mini_backend_server_retain(accepted->server_backend);

    netins_udp_clear_error(server);
    return accepted;
}

int
netins_udp_transfer(netins_udp_t *client,
    const netins_udp_transfer_t *task,
    netins_udp_transfer_result_t *result)
{
    if (!netins_udp_valid(client) || client->role != NETINS_UDP_ROLE_CLIENT
        || client->state != NETINS_UDP_STATE_CONNECTED
        || client->client_backend == NULL || task == NULL || result == NULL)
    {
        return netins_udp_set_error(client, EINVAL,
            "transfer requires a connected client, task and result");
    }
    if (task->struct_size < sizeof(*task)
        || task->api_version != NETINS_UDP_API_VERSION
        || result->struct_size < sizeof(*result)
        || result->api_version != NETINS_UDP_API_VERSION
        || (task->direction != NETINS_UDP_TRANSFER_GET
            && task->direction != NETINS_UDP_TRANSFER_POST)
        || task->local_path == NULL || task->local_path[0] == '\0'
        || task->remote_path == NULL || task->remote_path[0] != '/'
        || task->remote_path[1] == '\0'
        || strlen(task->local_path) >= NETINS_UDP_FILE_PATH_LEN
        || strlen(task->remote_path) >= NETINS_UDP_REMOTE_PATH_LEN)
    {
        return netins_udp_set_error(client, EINVAL,
            "invalid transfer version, direction, path, or result");
    }

    uint32_t result_size = result->struct_size;
    memset(result, 0, sizeof(*result));
    result->struct_size = result_size;
    result->api_version = NETINS_UDP_API_VERSION;

    mini_backend_transfer_result_t backend_result = {0};
    int backend_direction =
        task->direction == NETINS_UDP_TRANSFER_GET
        ? MINI_BACKEND_TRANSFER_GET : MINI_BACKEND_TRANSFER_POST;
    int ret = mini_backend_client_transfer(client->client_backend,
        backend_direction, task->local_path, task->remote_path,
        task->timeout_ms, &backend_result);

    result->total_bytes = backend_result.total_bytes;
    result->total_fragments = backend_result.total_fragments;
    result->completed_fragments = backend_result.completed_fragments;
    result->elapsed_us = backend_result.elapsed_us;
    result->throughput_mbps = result->elapsed_us > 0
        ? (double)result->total_bytes * 8.0 / (double)result->elapsed_us
        : 0.0;

    if (ret != 0) {
        if (ret == ETIMEDOUT
            || mini_backend_client_is_closed(client->client_backend))
        {
            client->state = NETINS_UDP_STATE_ERROR;
        }
        int error_code = ret > 0 ? ret : EIO;
        return netins_udp_set_error(client, error_code,
            "Mini transfer failed with backend code %d", ret);
    }
    netins_udp_clear_error(client);
    return 0;
}

int
netins_udp_serve(netins_udp_t *connection,
    const netins_udp_serve_config_t *config)
{
    if (!netins_udp_valid(connection)
        || connection->role != NETINS_UDP_ROLE_ACCEPTED
        || connection->state != NETINS_UDP_STATE_CONNECTED
        || connection->server_backend == NULL
        || connection->server_connection == NULL || config == NULL)
    {
        return netins_udp_set_error(connection, EINVAL,
            "serve requires a connected accepted handle");
    }
    if (config->struct_size < sizeof(*config)
        || config->api_version != NETINS_UDP_API_VERSION
        || config->root_path == NULL || config->root_path[0] == '\0'
        || strlen(config->root_path) >= NETINS_UDP_FILE_PATH_LEN)
    {
        return netins_udp_set_error(connection, EINVAL,
            "invalid serve configuration or root path");
    }

    int ret = mini_backend_server_connection_serve(
        connection->server_backend, connection->server_connection,
        config->root_path, config->timeout_ms);
    if (ret != 0) {
        if (mini_backend_server_connection_is_closed(
                connection->server_connection))
        {
            connection->state = NETINS_UDP_STATE_ERROR;
        }
        int error_code = ret > 0 ? ret : EIO;
        return netins_udp_set_error(connection, error_code,
            "Mini serve loop failed with backend code %d", ret);
    }

    connection->state = NETINS_UDP_STATE_CLOSED;
    netins_udp_clear_error(connection);
    return 0;
}

int
netins_udp_get_state(netins_udp_t *handle)
{
    if (!netins_udp_valid(handle)) {
        errno = EINVAL;
        return NETINS_UDP_STATE_ERROR;
    }
    int poll_error = 0;
    if (handle->role == NETINS_UDP_ROLE_CLIENT
        && handle->client_backend != NULL)
    {
        poll_error = mini_backend_client_poll(handle->client_backend);
    } else if ((handle->role == NETINS_UDP_ROLE_SERVER
            || handle->role == NETINS_UDP_ROLE_ACCEPTED)
        && handle->server_backend != NULL)
    {
        poll_error = mini_backend_server_poll(handle->server_backend);
        if (poll_error == 0) {
            poll_error =
                mini_backend_server_last_error(handle->server_backend);
        }
    }
    if (poll_error != 0) {
        handle->state = NETINS_UDP_STATE_ERROR;
        (void)netins_udp_set_error(handle, poll_error,
            "Mini backend event processing failed");
        return handle->state;
    }
    if (handle->role == NETINS_UDP_ROLE_CLIENT
        && handle->client_backend != NULL
        && mini_backend_client_is_closed(handle->client_backend))
    {
        int backend_error =
            mini_backend_client_last_error(handle->client_backend);
        if (backend_error != 0) {
            handle->state = NETINS_UDP_STATE_ERROR;
            (void)netins_udp_set_error(handle, ECONNRESET,
                "XQUIC client connection closed with code %d",
                backend_error);
        } else {
            handle->state = NETINS_UDP_STATE_CLOSED;
        }
    } else if (handle->role == NETINS_UDP_ROLE_ACCEPTED
        && handle->server_connection != NULL
        && mini_backend_server_connection_is_closed(
            handle->server_connection))
    {
        int backend_error = mini_backend_server_connection_last_error(
            handle->server_connection);
        if (backend_error != 0) {
            handle->state = NETINS_UDP_STATE_ERROR;
            (void)netins_udp_set_error(handle, ECONNRESET,
                "XQUIC accepted connection closed with code %d",
                backend_error);
        } else {
            handle->state = NETINS_UDP_STATE_CLOSED;
        }
    }
    return handle->state;
}

int
netins_udp_shutdown(netins_udp_t *handle)
{
    if (!netins_udp_valid(handle)) {
        errno = EINVAL;
        return -1;
    }
    if (handle->state == NETINS_UDP_STATE_CLOSED) {
        return 0;
    }

    handle->state = NETINS_UDP_STATE_CLOSING;
    int ret = 0;
    if (handle->role == NETINS_UDP_ROLE_CLIENT
        && handle->client_backend != NULL)
    {
        ret = mini_backend_client_shutdown(handle->client_backend,
            handle->close_timeout_ms);
    } else if (handle->role == NETINS_UDP_ROLE_SERVER
        && handle->server_backend != NULL)
    {
        mini_backend_server_shutdown(handle->server_backend);
    } else if (handle->role == NETINS_UDP_ROLE_ACCEPTED
        && handle->server_backend != NULL
        && handle->server_connection != NULL)
    {
        ret = mini_backend_server_connection_shutdown(handle->server_backend,
            handle->server_connection, handle->close_timeout_ms);
    }
    if (ret != 0) {
        handle->state = NETINS_UDP_STATE_ERROR;
        return netins_udp_set_error(handle, ret,
            "Mini backend shutdown failed");
    }
    handle->state = NETINS_UDP_STATE_CLOSED;
    netins_udp_clear_error(handle);
    return 0;
}

void
netins_udp_close(netins_udp_t *handle)
{
    if (!netins_udp_valid(handle)) {
        return;
    }
    if (handle->state != NETINS_UDP_STATE_CLOSED) {
        (void)netins_udp_shutdown(handle);
    }

    if (handle->role == NETINS_UDP_ROLE_CLIENT) {
        mini_backend_client_destroy(handle->client_backend);
        handle->client_backend = NULL;
    } else if (handle->role == NETINS_UDP_ROLE_SERVER) {
        mini_backend_server_release(handle->server_backend);
        handle->server_backend = NULL;
    } else if (handle->role == NETINS_UDP_ROLE_ACCEPTED) {
        mini_backend_server_connection_release(handle->server_backend,
            handle->server_connection);
        handle->server_connection = NULL;
        mini_backend_server_release(handle->server_backend);
        handle->server_backend = NULL;
    }

    handle->magic = 0;
    pthread_mutex_lock(&netins_udp_global_lock);
    if (netins_udp_handle_count > 0) {
        netins_udp_handle_count--;
    }
    pthread_mutex_unlock(&netins_udp_global_lock);
    free(handle);
}

int
netins_udp_get_last_error_code(netins_udp_t *handle)
{
    if (!netins_udp_valid(handle)) {
        errno = EINVAL;
        return EINVAL;
    }
    return handle->last_error_code;
}

int
netins_udp_get_last_error_msg(netins_udp_t *handle, char *buffer,
    size_t buffer_len)
{
    if (!netins_udp_valid(handle) || buffer == NULL || buffer_len == 0) {
        errno = EINVAL;
        return -1;
    }
    size_t length = strlen(handle->last_error_msg);
    if (length >= buffer_len) {
        errno = ENOBUFS;
        return -1;
    }
    memcpy(buffer, handle->last_error_msg, length + 1);
    return 0;
}
