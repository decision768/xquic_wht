#define _POSIX_C_SOURCE 200809L

#include "api/draft/netins_udp_v2.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "netins_udp_v2_internal.h"
#include "netins_udp_v2_transport.h"

_Static_assert(NETINS_UDP_METHOD_LEN
        == NETINS_UDP_V2_TRANSPORT_METHOD_LEN,
    "V2 public and transport method limits must match");
_Static_assert(NETINS_UDP_REQUEST_PATH_LEN
        == NETINS_UDP_V2_TRANSPORT_REQUEST_PATH_LEN,
    "V2 public and transport request-path limits must match");
_Static_assert(NETINS_UDP_SERVER_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN,
    "V2 public and transport authority limits must match");
_Static_assert(NETINS_UDP_CONTENT_TYPE_LEN
        == NETINS_UDP_V2_TRANSPORT_CONTENT_TYPE_LEN,
    "V2 public and transport content-type limits must match");
_Static_assert(NETINS_UDP_MAX_H3_HEADERS
        == NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS,
    "V2 public and transport H3 header counts must match");
_Static_assert(NETINS_UDP_H3_HEADER_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_H3_HEADER_NAME_LEN,
    "V2 public and transport H3 header-name limits must match");
_Static_assert(NETINS_UDP_H3_HEADER_VALUE_LEN
        == NETINS_UDP_V2_TRANSPORT_H3_HEADER_VALUE_LEN,
    "V2 public and transport H3 header-value limits must match");

struct netins_udp_h3_request_s {
    pthread_mutex_t lock;
    pthread_mutex_t send_lock;
    pthread_mutex_t recv_lock;
    pthread_cond_t idle_cond;
    size_t active_calls;
    int closing;

    netins_udp_v2_transport_request_t *transport;
    netins_udp_h3_request_config_t config;
    int nonblock;
    uint32_t send_timeout_ms;
    uint32_t recv_timeout_ms;
    uint32_t close_timeout_ms;
    uint64_t bytes_sent;
    int send_fin_accepted;

    int last_error_code;
    char last_error_msg[NETINS_UDP_ERROR_MSG_LEN];
};

static void
netins_udp_v2_h3_set_error_locked(netins_udp_h3_request_t *request,
    int error_code, const char *message)
{
    request->last_error_code = error_code;
    if (message == NULL) {
        request->last_error_msg[0] = '\0';
    } else {
        snprintf(request->last_error_msg,
            sizeof(request->last_error_msg), "%s", message);
    }
}

static int
netins_udp_v2_h3_fail(netins_udp_h3_request_t *request,
    int error_code, const char *message)
{
    pthread_mutex_lock(&request->lock);
    netins_udp_v2_h3_set_error_locked(request, error_code, message);
    pthread_mutex_unlock(&request->lock);
    errno = error_code;
    return NETINS_UDP_ERROR;
}

static int
netins_udp_v2_h3_begin_call(netins_udp_h3_request_t *request)
{
    if (request == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    pthread_mutex_lock(&request->lock);
    if (request->closing) {
        netins_udp_v2_h3_set_error_locked(request, EBADF,
            "HTTP/3 request handle is closing");
        pthread_mutex_unlock(&request->lock);
        errno = EBADF;
        return NETINS_UDP_ERROR;
    }
    request->active_calls++;
    pthread_mutex_unlock(&request->lock);
    return NETINS_UDP_OK;
}

static void
netins_udp_v2_h3_end_call(netins_udp_h3_request_t *request)
{
    pthread_mutex_lock(&request->lock);
    if (request->active_calls > 0) {
        request->active_calls--;
    }
    if (request->active_calls == 0) {
        pthread_cond_broadcast(&request->idle_cond);
    }
    pthread_mutex_unlock(&request->lock);
}

static int
netins_udp_v2_h3_fixed_string_valid(const char *value, size_t capacity)
{
    return value != NULL && memchr(value, '\0', capacity) != NULL;
}

static int
netins_udp_v2_h3_header_name_char_valid(unsigned char value)
{
    return (value >= 'a' && value <= 'z')
        || (value >= '0' && value <= '9')
        || value == '!' || value == '#' || value == '$'
        || value == '%' || value == '&' || value == '\''
        || value == '*' || value == '+' || value == '-'
        || value == '.' || value == '^' || value == '_'
        || value == '`' || value == '|' || value == '~';
}

static int
netins_udp_v2_h3_header_managed(const char *name)
{
    return name[0] == ':'
        || strcmp(name, "host") == 0
        || strcmp(name, "content-length") == 0
        || strcmp(name, "content-type") == 0;
}

static int
netins_udp_v2_h3_header_valid(const netins_udp_h3_header_t *header)
{
    if (header == NULL
        || !netins_udp_v2_h3_fixed_string_valid(header->name,
            sizeof(header->name))
        || !netins_udp_v2_h3_fixed_string_valid(header->value,
            sizeof(header->value))
        || header->name[0] == '\0'
        || netins_udp_v2_h3_header_managed(header->name)
        || strchr(header->value, '\r') != NULL
        || strchr(header->value, '\n') != NULL)
    {
        return 0;
    }
    for (const unsigned char *cursor =
            (const unsigned char *)header->name;
         *cursor != '\0'; ++cursor)
    {
        if (!netins_udp_v2_h3_header_name_char_valid(*cursor)) {
            return 0;
        }
    }
    return 1;
}

static int
netins_udp_v2_h3_request_config_error(
    const netins_udp_h3_request_config_t *config,
    const char **message)
{
    if (config == NULL) {
        *message = "H3 request configuration must not be NULL";
        return EINVAL;
    }
    if (config->struct_size != sizeof(*config)
        || config->api_version != NETINS_UDP_API_VERSION)
    {
        *message = "H3 request configuration version is invalid";
        return EPROTO;
    }
    if (!netins_udp_v2_h3_fixed_string_valid(config->method,
            sizeof(config->method))
        || !netins_udp_v2_h3_fixed_string_valid(config->path,
            sizeof(config->path))
        || !netins_udp_v2_h3_fixed_string_valid(config->authority,
            sizeof(config->authority))
        || !netins_udp_v2_h3_fixed_string_valid(config->content_type,
            sizeof(config->content_type))
        || config->method[0] == '\0'
        || config->path[0] != '/'
        || config->content_type[0] == '\0'
        || config->header_count > NETINS_UDP_MAX_H3_HEADERS)
    {
        *message = "H3 request configuration fields are invalid";
        return EINVAL;
    }
    if (strcmp(config->method, "GET") != 0
        && strcmp(config->method, "POST") != 0)
    {
        *message = "the current H3 backend supports GET and POST only";
        return EOPNOTSUPP;
    }
    if (strcmp(config->method, "GET") == 0
        && config->content_length != 0)
    {
        *message = "GET requests cannot contain a request body";
        return EINVAL;
    }
    for (uint32_t i = 0; i < config->header_count; ++i) {
        if (!netins_udp_v2_h3_header_valid(&config->headers[i])) {
            *message = "an H3 extension header is invalid or managed";
            return EINVAL;
        }
    }
    return 0;
}

static int
netins_udp_v2_h3_config_add_header(
    uint32_t struct_size, uint32_t api_version,
    uint32_t *header_count, netins_udp_h3_header_t *headers,
    const char *name, const char *value)
{
    if (struct_size == 0 || api_version != NETINS_UDP_API_VERSION
        || header_count == NULL || headers == NULL
        || name == NULL || value == NULL)
    {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    if (*header_count >= NETINS_UDP_MAX_H3_HEADERS) {
        errno = ENOBUFS;
        return NETINS_UDP_ERROR;
    }
    size_t name_length = strnlen(name, NETINS_UDP_H3_HEADER_NAME_LEN);
    size_t value_length = strnlen(value, NETINS_UDP_H3_HEADER_VALUE_LEN);
    if (name_length == 0
        || name_length >= NETINS_UDP_H3_HEADER_NAME_LEN
        || value_length >= NETINS_UDP_H3_HEADER_VALUE_LEN)
    {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }

    netins_udp_h3_header_t candidate;
    memset(&candidate, 0, sizeof(candidate));
    memcpy(candidate.name, name, name_length + 1);
    memcpy(candidate.value, value, value_length + 1);
    if (!netins_udp_v2_h3_header_valid(&candidate)) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    headers[*header_count] = candidate;
    (*header_count)++;
    return NETINS_UDP_OK;
}

static int
netins_udp_v2_h3_request_is_closing(
    netins_udp_h3_request_t *request)
{
    pthread_mutex_lock(&request->lock);
    int closing = request->closing;
    pthread_mutex_unlock(&request->lock);
    return closing;
}

static void
netins_udp_v2_h3_sync_transport_error(
    netins_udp_h3_request_t *request)
{
    netins_udp_v2_transport_h3_info_t info;
    if (netins_udp_v2_transport_h3_request_get_info(
            request->transport, &info) == 0
        && info.stream_error != 0)
    {
        pthread_mutex_lock(&request->lock);
        netins_udp_v2_h3_set_error_locked(request, EIO,
            "underlying HTTP/3 request failed");
        pthread_mutex_unlock(&request->lock);
    }
}

void
netins_udp_h3_request_config_init(
    netins_udp_h3_request_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->api_version = NETINS_UDP_API_VERSION;
    snprintf(config->method, sizeof(config->method), "GET");
    snprintf(config->path, sizeof(config->path), "/");
    snprintf(config->content_type, sizeof(config->content_type),
        "application/octet-stream");
}

int
netins_udp_h3_request_config_add_header(
    netins_udp_h3_request_config_t *config,
    const char *name, const char *value)
{
    if (config == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    if (config->struct_size != sizeof(*config)
        || config->api_version != NETINS_UDP_API_VERSION)
    {
        errno = EPROTO;
        return NETINS_UDP_ERROR;
    }
    return netins_udp_v2_h3_config_add_header(config->struct_size,
        config->api_version, &config->header_count, config->headers,
        name, value);
}

void
netins_udp_h3_response_config_init(
    netins_udp_h3_response_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->struct_size = sizeof(*config);
    config->api_version = NETINS_UDP_API_VERSION;
    config->status = 200;
    snprintf(config->content_type, sizeof(config->content_type),
        "application/octet-stream");
}

int
netins_udp_h3_response_config_add_header(
    netins_udp_h3_response_config_t *config,
    const char *name, const char *value)
{
    if (config == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    if (config->struct_size != sizeof(*config)
        || config->api_version != NETINS_UDP_API_VERSION)
    {
        errno = EPROTO;
        return NETINS_UDP_ERROR;
    }
    return netins_udp_v2_h3_config_add_header(config->struct_size,
        config->api_version, &config->header_count, config->headers,
        name, value);
}

int
netins_udp_h3_request_open(netins_udp_t *netins,
    const netins_udp_h3_request_config_t *config,
    netins_udp_h3_request_t **out_request)
{
    if (out_request == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    *out_request = NULL;

    netins_udp_v2_h3_parent_snapshot_t parent;
    if (netins_udp_v2_connection_acquire_h3(netins, &parent)
        != NETINS_UDP_OK)
    {
        return NETINS_UDP_ERROR;
    }

    const char *message = NULL;
    int error_code =
        netins_udp_v2_h3_request_config_error(config, &message);
    if (error_code != 0) {
        netins_udp_v2_connection_set_error(netins, error_code, message);
        netins_udp_v2_connection_release(netins);
        return NETINS_UDP_ERROR;
    }

    netins_udp_h3_request_t *request =
        calloc(1, sizeof(*request));
    if (request == NULL) {
        netins_udp_v2_connection_set_error(netins, ENOMEM,
            "failed to allocate an HTTP/3 request handle");
        netins_udp_v2_connection_release(netins);
        return NETINS_UDP_ERROR;
    }
    int init_error = pthread_mutex_init(&request->lock, NULL);
    if (init_error == 0) {
        init_error = pthread_mutex_init(&request->send_lock, NULL);
        if (init_error != 0) {
            pthread_mutex_destroy(&request->lock);
        }
    }
    if (init_error == 0) {
        init_error = pthread_mutex_init(&request->recv_lock, NULL);
        if (init_error != 0) {
            pthread_mutex_destroy(&request->send_lock);
            pthread_mutex_destroy(&request->lock);
        }
    }
    if (init_error == 0) {
        init_error = pthread_cond_init(&request->idle_cond, NULL);
        if (init_error != 0) {
            pthread_mutex_destroy(&request->recv_lock);
            pthread_mutex_destroy(&request->send_lock);
            pthread_mutex_destroy(&request->lock);
        }
    }
    if (init_error != 0) {
        free(request);
        netins_udp_v2_connection_set_error(netins, init_error,
            "failed to initialize an HTTP/3 request handle");
        netins_udp_v2_connection_release(netins);
        return NETINS_UDP_ERROR;
    }

    request->config = *config;
    request->nonblock = parent.nonblock;
    request->send_timeout_ms = parent.send_timeout_ms;
    request->recv_timeout_ms = parent.recv_timeout_ms;
    request->close_timeout_ms = parent.close_timeout_ms;

    if (netins_udp_v2_child_handle_acquire() != NETINS_UDP_OK) {
        error_code = errno != 0 ? errno : EOVERFLOW;
        pthread_cond_destroy(&request->idle_cond);
        pthread_mutex_destroy(&request->recv_lock);
        pthread_mutex_destroy(&request->send_lock);
        pthread_mutex_destroy(&request->lock);
        free(request);
        netins_udp_v2_connection_set_error(netins, error_code,
            "failed to account for an HTTP/3 request handle");
        netins_udp_v2_connection_release(netins);
        return NETINS_UDP_ERROR;
    }

    netins_udp_v2_transport_h3_config_t transport_config;
    memset(&transport_config, 0, sizeof(transport_config));
    snprintf(transport_config.method, sizeof(transport_config.method),
        "%s", config->method);
    snprintf(transport_config.path, sizeof(transport_config.path), "%s",
        config->path);
    snprintf(transport_config.authority,
        sizeof(transport_config.authority), "%s", config->authority);
    snprintf(transport_config.content_type,
        sizeof(transport_config.content_type), "%s",
        config->content_type);
    transport_config.content_length = config->content_length;
    transport_config.content_length_unknown =
        config->content_length == NETINS_UDP_CONTENT_LENGTH_UNKNOWN;
    transport_config.header_count = config->header_count;
    for (uint32_t i = 0; i < config->header_count; ++i) {
        snprintf(transport_config.headers[i].name,
            sizeof(transport_config.headers[i].name), "%s",
            config->headers[i].name);
        snprintf(transport_config.headers[i].value,
            sizeof(transport_config.headers[i].value), "%s",
            config->headers[i].value);
    }

    if (netins_udp_v2_transport_h3_request_open(parent.transport,
            &transport_config, &request->transport) != 0)
    {
        error_code = errno != 0 ? errno : EIO;
        netins_udp_v2_child_handle_release();
        pthread_cond_destroy(&request->idle_cond);
        pthread_mutex_destroy(&request->recv_lock);
        pthread_mutex_destroy(&request->send_lock);
        pthread_mutex_destroy(&request->lock);
        free(request);
        netins_udp_v2_connection_set_error(netins, error_code,
            "failed to open the underlying HTTP/3 request");
        netins_udp_v2_connection_release(netins);
        return NETINS_UDP_ERROR;
    }

    netins_udp_v2_connection_set_error(netins, 0, NULL);
    *out_request = request;
    netins_udp_v2_connection_release(netins);
    return NETINS_UDP_OK;
}

ssize_t
netins_udp_h3_request_send(netins_udp_h3_request_t *request,
    const void *data, size_t length, int flags)
{
    if (netins_udp_v2_h3_begin_call(request) != NETINS_UDP_OK) {
        return -1;
    }
    pthread_mutex_lock(&request->send_lock);

    int allowed = NETINS_UDP_MSG_DONTWAIT | NETINS_UDP_MSG_FIN;
    if ((flags & ~allowed) != 0) {
        int error_code = (flags & NETINS_UDP_MSG_PEEK) != 0
            ? EINVAL : EOPNOTSUPP;
        (void)netins_udp_v2_h3_fail(request, error_code,
            "invalid flags for HTTP/3 request send");
        pthread_mutex_unlock(&request->send_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }
    if (length > 0 && data == NULL) {
        (void)netins_udp_v2_h3_fail(request, EINVAL,
            "send data must not be NULL when length is nonzero");
        pthread_mutex_unlock(&request->send_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }
    if (netins_udp_v2_h3_request_is_closing(request)) {
        (void)netins_udp_v2_h3_fail(request, ECANCELED,
            "HTTP/3 request was canceled");
        pthread_mutex_unlock(&request->send_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }
    if (strcmp(request->config.method, "POST") != 0) {
        (void)netins_udp_v2_h3_fail(request, EOPNOTSUPP,
            "request body send is valid only for POST");
        pthread_mutex_unlock(&request->send_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }

    int fin = (flags & NETINS_UDP_MSG_FIN) != 0;
    int content_length_unknown =
        request->config.content_length
            == NETINS_UDP_CONTENT_LENGTH_UNKNOWN;
    if (request->send_fin_accepted) {
        (void)netins_udp_v2_h3_fail(request, EPIPE,
            "request body FIN was already accepted");
        pthread_mutex_unlock(&request->send_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }
    if (!content_length_unknown) {
        if (request->bytes_sent > request->config.content_length) {
            (void)netins_udp_v2_h3_fail(request, EIO,
                "request body accounting exceeded Content-Length");
            pthread_mutex_unlock(&request->send_lock);
            netins_udp_v2_h3_end_call(request);
            return -1;
        }
        uint64_t remaining = request->config.content_length
            - request->bytes_sent;
        if ((uint64_t)length > remaining
            || (fin && (uint64_t)length != remaining)
            || (!fin && remaining != 0
                && (uint64_t)length == remaining))
        {
            (void)netins_udp_v2_h3_fail(request, EINVAL,
                "known-length request body must end exactly with FIN");
            pthread_mutex_unlock(&request->send_lock);
            netins_udp_v2_h3_end_call(request);
            return -1;
        }
    } else if ((uint64_t)length
        > UINT64_MAX - request->bytes_sent)
    {
        (void)netins_udp_v2_h3_fail(request, EOVERFLOW,
            "request body byte counter overflow");
        pthread_mutex_unlock(&request->send_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }
    if (length == 0 && !fin) {
        pthread_mutex_lock(&request->lock);
        netins_udp_v2_h3_set_error_locked(request, 0, NULL);
        pthread_mutex_unlock(&request->lock);
        pthread_mutex_unlock(&request->send_lock);
        netins_udp_v2_h3_end_call(request);
        return 0;
    }

    int dontwait = request->nonblock
        || (flags & NETINS_UDP_MSG_DONTWAIT) != 0;
    int wait_forever = !dontwait && request->send_timeout_ms == 0;
    uint32_t timeout_ms = dontwait ? 0 : request->send_timeout_ms;
    int64_t sent = netins_udp_v2_transport_h3_request_send(
        request->transport, data, length, fin, timeout_ms,
        wait_forever);
    if (sent < 0) {
        int error_code = errno != 0 ? errno : EIO;
        netins_udp_v2_h3_sync_transport_error(request);
        (void)netins_udp_v2_h3_fail(request, error_code,
            error_code == EAGAIN
                ? "HTTP/3 request send would block"
                : (error_code == ETIMEDOUT
                    ? "HTTP/3 request send timed out"
                    : (error_code == ECANCELED
                        ? "HTTP/3 request send was canceled"
                        : "HTTP/3 request send failed")));
        pthread_mutex_unlock(&request->send_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }

    request->bytes_sent += (uint64_t)sent;
    if (fin && (size_t)sent == length) {
        request->send_fin_accepted = 1;
    }
    pthread_mutex_lock(&request->lock);
    netins_udp_v2_h3_set_error_locked(request, 0, NULL);
    pthread_mutex_unlock(&request->lock);
    pthread_mutex_unlock(&request->send_lock);
    netins_udp_v2_h3_end_call(request);
    return (ssize_t)sent;
}

ssize_t
netins_udp_h3_request_recv(netins_udp_h3_request_t *request,
    void *data, size_t length, int flags)
{
    if (netins_udp_v2_h3_begin_call(request) != NETINS_UDP_OK) {
        return -1;
    }
    pthread_mutex_lock(&request->recv_lock);

    int allowed = NETINS_UDP_MSG_DONTWAIT | NETINS_UDP_MSG_PEEK;
    if ((flags & ~allowed) != 0) {
        int error_code = (flags & NETINS_UDP_MSG_FIN) != 0
            ? EINVAL : EOPNOTSUPP;
        (void)netins_udp_v2_h3_fail(request, error_code,
            "invalid flags for HTTP/3 request recv");
        pthread_mutex_unlock(&request->recv_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }
    if (length > 0 && data == NULL) {
        (void)netins_udp_v2_h3_fail(request, EINVAL,
            "recv data must not be NULL when length is nonzero");
        pthread_mutex_unlock(&request->recv_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }
    if (netins_udp_v2_h3_request_is_closing(request)) {
        (void)netins_udp_v2_h3_fail(request, ECANCELED,
            "HTTP/3 request was canceled");
        pthread_mutex_unlock(&request->recv_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }
    if (length == 0) {
        pthread_mutex_lock(&request->lock);
        netins_udp_v2_h3_set_error_locked(request, 0, NULL);
        pthread_mutex_unlock(&request->lock);
        pthread_mutex_unlock(&request->recv_lock);
        netins_udp_v2_h3_end_call(request);
        return 0;
    }

    int dontwait = request->nonblock
        || (flags & NETINS_UDP_MSG_DONTWAIT) != 0;
    int wait_forever = !dontwait && request->recv_timeout_ms == 0;
    uint32_t timeout_ms = dontwait ? 0 : request->recv_timeout_ms;
    int fin = 0;
    int peek = (flags & NETINS_UDP_MSG_PEEK) != 0;
    int64_t received = netins_udp_v2_transport_h3_request_recv(
        request->transport, data, length, &fin, peek, timeout_ms,
        wait_forever);
    (void)fin;
    if (received < 0) {
        int error_code = errno != 0 ? errno : EIO;
        netins_udp_v2_h3_sync_transport_error(request);
        (void)netins_udp_v2_h3_fail(request, error_code,
            error_code == EAGAIN
                ? "HTTP/3 request recv would block"
                : (error_code == ETIMEDOUT
                    ? "HTTP/3 request recv timed out"
                    : (error_code == ECANCELED
                        ? "HTTP/3 request recv was canceled"
                        : "HTTP/3 request recv failed")));
        pthread_mutex_unlock(&request->recv_lock);
        netins_udp_v2_h3_end_call(request);
        return -1;
    }

    pthread_mutex_lock(&request->lock);
    netins_udp_v2_h3_set_error_locked(request, 0, NULL);
    pthread_mutex_unlock(&request->lock);
    pthread_mutex_unlock(&request->recv_lock);
    netins_udp_v2_h3_end_call(request);
    return (ssize_t)received;
}

int
netins_udp_h3_request_get_info(netins_udp_h3_request_t *request,
    netins_udp_h3_request_info_t *out)
{
    if (request == NULL || out == NULL) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    if (netins_udp_v2_h3_begin_call(request) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    if (out->struct_size != sizeof(*out)
        || out->api_version != NETINS_UDP_API_VERSION)
    {
        int result = netins_udp_v2_h3_fail(request, EPROTO,
            "H3 request info structure version is invalid");
        netins_udp_v2_h3_end_call(request);
        return result;
    }

    netins_udp_v2_transport_h3_info_t transport_info;
    if (netins_udp_v2_transport_h3_request_get_info(
            request->transport, &transport_info) != 0)
    {
        int error_code = errno != 0 ? errno : EIO;
        int result = netins_udp_v2_h3_fail(request, error_code,
            "failed to read underlying HTTP/3 request info");
        netins_udp_v2_h3_end_call(request);
        return result;
    }
    if (transport_info.stream_error != 0) {
        pthread_mutex_lock(&request->lock);
        netins_udp_v2_h3_set_error_locked(request, EIO,
            "underlying HTTP/3 request failed");
        pthread_mutex_unlock(&request->lock);
    }

    netins_udp_h3_request_info_t info =
        NETINS_UDP_H3_REQUEST_INFO_INITIALIZER;
    info.stream_id = transport_info.stream_id;
    info.response_status = transport_info.response_status;
    info.request_headers_ready =
        transport_info.request_headers_ready;
    info.response_headers_ready =
        transport_info.response_headers_ready;
    info.send_finished = transport_info.send_finished;
    info.receive_finished = transport_info.receive_finished;
    pthread_mutex_lock(&request->lock);
    info.last_error_code = request->last_error_code;
    pthread_mutex_unlock(&request->lock);
    info.peer_header_count = transport_info.peer_header_count;
    snprintf(info.method, sizeof(info.method), "%s",
        request->config.method);
    snprintf(info.path, sizeof(info.path), "%s",
        request->config.path);
    snprintf(info.authority, sizeof(info.authority), "%s",
        transport_info.request_authority);
    snprintf(info.request_content_type,
        sizeof(info.request_content_type), "%s",
        request->config.content_type);
    snprintf(info.response_content_type,
        sizeof(info.response_content_type), "%s",
        transport_info.response_content_type);
    info.request_content_length = request->config.content_length;
    info.response_content_length =
        transport_info.response_content_length_known
            ? transport_info.response_content_length
            : NETINS_UDP_CONTENT_LENGTH_UNKNOWN;
    info.bytes_sent = transport_info.bytes_accepted;
    info.bytes_received = transport_info.bytes_delivered;
    for (uint32_t i = 0; i < transport_info.peer_header_count; ++i) {
        snprintf(info.peer_headers[i].name,
            sizeof(info.peer_headers[i].name), "%s",
            transport_info.peer_headers[i].name);
        snprintf(info.peer_headers[i].value,
            sizeof(info.peer_headers[i].value), "%s",
            transport_info.peer_headers[i].value);
    }
    *out = info;
    netins_udp_v2_h3_end_call(request);
    return NETINS_UDP_OK;
}

int
netins_udp_h3_request_get_last_error_code(
    netins_udp_h3_request_t *request)
{
    if (request == NULL) {
        return EINVAL;
    }
    if (netins_udp_v2_h3_begin_call(request) != NETINS_UDP_OK) {
        return EBADF;
    }
    netins_udp_v2_h3_sync_transport_error(request);
    pthread_mutex_lock(&request->lock);
    int error_code = request->last_error_code;
    pthread_mutex_unlock(&request->lock);
    netins_udp_v2_h3_end_call(request);
    return error_code;
}

int
netins_udp_h3_request_get_last_error_msg(
    netins_udp_h3_request_t *request, char *buffer, size_t buffer_len)
{
    if (request == NULL || buffer == NULL || buffer_len == 0) {
        errno = EINVAL;
        return NETINS_UDP_ERROR;
    }
    if (netins_udp_v2_h3_begin_call(request) != NETINS_UDP_OK) {
        return NETINS_UDP_ERROR;
    }
    netins_udp_v2_h3_sync_transport_error(request);
    pthread_mutex_lock(&request->lock);
    snprintf(buffer, buffer_len, "%s", request->last_error_msg);
    pthread_mutex_unlock(&request->lock);
    netins_udp_v2_h3_end_call(request);
    return NETINS_UDP_OK;
}

void
netins_udp_h3_request_close(netins_udp_h3_request_t *request)
{
    if (request == NULL) {
        return;
    }

    pthread_mutex_lock(&request->lock);
    request->closing = 1;
    netins_udp_v2_transport_request_t *transport = request->transport;
    pthread_mutex_unlock(&request->lock);

    if (transport != NULL) {
        netins_udp_v2_transport_h3_request_close_async(transport);
    }

    pthread_mutex_lock(&request->lock);
    while (request->active_calls != 0) {
        pthread_cond_wait(&request->idle_cond, &request->lock);
    }
    transport = request->transport;
    request->transport = NULL;
    uint32_t close_timeout_ms = request->close_timeout_ms;
    pthread_mutex_unlock(&request->lock);

    if (transport != NULL) {
        netins_udp_v2_transport_h3_request_close(transport,
            close_timeout_ms);
    }
    pthread_cond_destroy(&request->idle_cond);
    pthread_mutex_destroy(&request->recv_lock);
    pthread_mutex_destroy(&request->send_lock);
    pthread_mutex_destroy(&request->lock);
    free(request);
    netins_udp_v2_child_handle_release();
}
