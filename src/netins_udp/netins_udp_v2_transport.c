#include "netins_udp_v2_transport.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <xquic/netins_udp.h>
#include <xquic/xquic.h>

#include "netins_udp_client_internal.h"

_Static_assert(NETINS_UDP_MAX_PATHS
        == NETINS_UDP_V2_TRANSPORT_MAX_PATHS,
    "V1 client and V2 transport path limits must match");
_Static_assert(NETINS_UDP_SERVER_ADDRESS_LEN
        == NETINS_UDP_V2_TRANSPORT_HOST_LEN,
    "V1 client and V2 transport host limits must match");
_Static_assert(NETINS_UDP_SERVER_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN,
    "V1 client and V2 transport server-name limits must match");
_Static_assert(NETINS_UDP_INTERFACE_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_INTERFACE_NAME_LEN,
    "V1 client and V2 transport interface-name limits must match");
_Static_assert(NETINS_UDP_METHOD_LEN
        == NETINS_UDP_V2_TRANSPORT_METHOD_LEN,
    "V1 client and V2 transport method limits must match");
_Static_assert(NETINS_UDP_REQUEST_PATH_LEN
        == NETINS_UDP_V2_TRANSPORT_REQUEST_PATH_LEN,
    "V1 client and V2 transport request-path limits must match");
_Static_assert(NETINS_UDP_CONTENT_TYPE_LEN
        == NETINS_UDP_V2_TRANSPORT_CONTENT_TYPE_LEN,
    "V1 client and V2 transport content-type limits must match");
_Static_assert(NETINS_UDP_CLIENT_H3_MAX_HEADERS
        == NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS,
    "V1 client and V2 transport H3 header counts must match");
_Static_assert(NETINS_UDP_CLIENT_H3_HEADER_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_H3_HEADER_NAME_LEN,
    "V1 client and V2 transport H3 header-name limits must match");
_Static_assert(NETINS_UDP_CLIENT_H3_HEADER_VALUE_LEN
        == NETINS_UDP_V2_TRANSPORT_H3_HEADER_VALUE_LEN,
    "V1 client and V2 transport H3 header-value limits must match");

static const xqc_scheduler_callback_t *
netins_udp_v2_transport_scheduler(int scheduler)
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
netins_udp_v2_transport_congestion_control(int congestion_control)
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

int
netins_udp_v2_transport_client_create(
    const netins_udp_v2_transport_config_t *config,
    netins_udp_v2_transport_client_t **out_client)
{
    if (config == NULL || out_client == NULL) {
        errno = EINVAL;
        return -1;
    }
    *out_client = NULL;

    const xqc_scheduler_callback_t *scheduler =
        netins_udp_v2_transport_scheduler(config->scheduler);
    const xqc_cong_ctrl_callback_t *congestion_control =
        netins_udp_v2_transport_congestion_control(
            config->congestion_control);
    if (scheduler == NULL || congestion_control == NULL) {
        errno = EOPNOTSUPP;
        return -1;
    }

    netins_udp_client_config_t legacy;
    netins_udp_client_config_init(&legacy);
    snprintf(legacy.server_address, sizeof(legacy.server_address), "%s",
        config->host);
    legacy.server_port = config->port;
    snprintf(legacy.server_name, sizeof(legacy.server_name), "%s",
        config->server_name);
    legacy.interface_count = config->interface_count;
    for (uint32_t i = 0; i < config->interface_count; ++i) {
        snprintf(legacy.interface_names[i],
            sizeof(legacy.interface_names[i]), "%s",
            config->interface_names[i]);
    }
    legacy.enable_multipath = config->enable_multipath;
    legacy.connect_timeout_ms = config->connect_timeout_ms;
    legacy.kernel_sndbuf = config->kernel_sndbuf;
    legacy.kernel_rcvbuf = config->kernel_rcvbuf;
    legacy.initial_cwnd_packets = config->initial_cwnd_packets;
    legacy.verify_certificate = config->verify_peer;

    netins_udp_client_t *client = NULL;
    if (netins_udp_client_create(&legacy, &client) != NETINS_UDP_OK) {
        errno = ENOMEM;
        return -1;
    }
    if (netins_udp_client_configure_transport(client, congestion_control,
            scheduler, config->idle_timeout_ms,
            config->close_timeout_ms) != NETINS_UDP_OK)
    {
        int error_code = netins_udp_client_get_last_error_code(client);
        netins_udp_client_close(client);
        errno = error_code != 0 ? error_code : EIO;
        return -1;
    }

    *out_client = client;
    return 0;
}

int
netins_udp_v2_transport_client_start(
    netins_udp_v2_transport_client_t *client)
{
    if (client == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (netins_udp_client_start_async(client) != NETINS_UDP_OK) {
        int error_code = netins_udp_client_get_last_error_code(client);
        errno = error_code != 0 ? error_code : EIO;
        return -1;
    }
    return 0;
}

int
netins_udp_v2_transport_client_wait_connected(
    netins_udp_v2_transport_client_t *client, uint32_t timeout_ms)
{
    if (client == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (netins_udp_client_wait_connected(client, timeout_ms)
        != NETINS_UDP_OK)
    {
        int error_code = netins_udp_client_get_last_error_code(client);
        errno = error_code != 0 ? error_code : ECONNABORTED;
        return -1;
    }
    return 0;
}

int
netins_udp_v2_transport_client_get_state(
    netins_udp_v2_transport_client_t *client)
{
    if (client == NULL) {
        return NETINS_UDP_V2_TRANSPORT_ERROR;
    }
    switch (netins_udp_client_get_state(client)) {
    case NETINS_UDP_STATE_NEW:
        return NETINS_UDP_V2_TRANSPORT_NEW;
    case NETINS_UDP_STATE_CONNECTING:
        return NETINS_UDP_V2_TRANSPORT_CONNECTING;
    case NETINS_UDP_STATE_CONNECTED:
        return NETINS_UDP_V2_TRANSPORT_CONNECTED;
    case NETINS_UDP_STATE_CLOSING:
        return NETINS_UDP_V2_TRANSPORT_CLOSING;
    case NETINS_UDP_STATE_CLOSED:
        return NETINS_UDP_V2_TRANSPORT_CLOSED;
    case NETINS_UDP_STATE_ERROR:
    default:
        return NETINS_UDP_V2_TRANSPORT_ERROR;
    }
}

int
netins_udp_v2_transport_client_get_last_error_code(
    netins_udp_v2_transport_client_t *client)
{
    return client != NULL
        ? netins_udp_client_get_last_error_code(client) : EINVAL;
}

int
netins_udp_v2_transport_client_get_last_error_msg(
    netins_udp_v2_transport_client_t *client,
    char *buffer, size_t buffer_len)
{
    if (client == NULL || buffer == NULL || buffer_len == 0) {
        errno = EINVAL;
        return -1;
    }
    if (netins_udp_client_get_last_error_msg(client, buffer, buffer_len)
        != NETINS_UDP_OK)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

void
netins_udp_v2_transport_client_set_close_timeout(
    netins_udp_v2_transport_client_t *client, uint32_t close_timeout_ms)
{
    netins_udp_client_set_close_timeout(client, close_timeout_ms);
}

void
netins_udp_v2_transport_client_request_close(
    netins_udp_v2_transport_client_t *client)
{
    netins_udp_client_request_close(client);
}

void
netins_udp_v2_transport_client_close(
    netins_udp_v2_transport_client_t *client)
{
    netins_udp_client_close(client);
}

int
netins_udp_v2_transport_h3_request_open(
    netins_udp_v2_transport_client_t *client,
    const netins_udp_v2_transport_h3_config_t *config,
    netins_udp_v2_transport_request_t **out_request)
{
    if (client == NULL || config == NULL || out_request == NULL
        || config->header_count > NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS)
    {
        errno = EINVAL;
        return -1;
    }

    netins_udp_client_h3_config_t internal;
    memset(&internal, 0, sizeof(internal));
    snprintf(internal.method, sizeof(internal.method), "%s",
        config->method);
    snprintf(internal.path, sizeof(internal.path), "%s", config->path);
    snprintf(internal.authority, sizeof(internal.authority), "%s",
        config->authority);
    snprintf(internal.content_type, sizeof(internal.content_type), "%s",
        config->content_type);
    internal.content_length = config->content_length;
    internal.content_length_unknown = config->content_length_unknown;
    internal.header_count = config->header_count;
    for (uint32_t i = 0; i < config->header_count; ++i) {
        snprintf(internal.headers[i].name,
            sizeof(internal.headers[i].name), "%s",
            config->headers[i].name);
        snprintf(internal.headers[i].value,
            sizeof(internal.headers[i].value), "%s",
            config->headers[i].value);
    }

    if (netins_udp_client_h3_request_open(client, &internal,
            out_request) != NETINS_UDP_OK)
    {
        errno = EIO;
        return -1;
    }
    return 0;
}

int64_t
netins_udp_v2_transport_h3_request_send(
    netins_udp_v2_transport_request_t *request,
    const void *data, size_t length, int fin, uint32_t timeout_ms,
    int wait_forever)
{
    uint32_t legacy_timeout_ms = wait_forever
        ? UINT32_MAX : timeout_ms;
    int64_t result;
    do {
        result = netins_udp_stream_send(request, data, length, fin,
            legacy_timeout_ms);
    } while (wait_forever && result == NETINS_UDP_WOULD_BLOCK);
    if (result >= 0) {
        return result;
    }
    if (result == NETINS_UDP_WOULD_BLOCK) {
        errno = !wait_forever && timeout_ms > 0
            ? ETIMEDOUT : EAGAIN;
    } else if (result == NETINS_UDP_CLOSED) {
        errno = ECANCELED;
    } else if (result == NETINS_UDP_BUFFER_TOO_SMALL) {
        errno = ENOBUFS;
    } else {
        errno = EIO;
    }
    return -1;
}

int64_t
netins_udp_v2_transport_h3_request_recv(
    netins_udp_v2_transport_request_t *request,
    void *data, size_t length, int *fin, int peek,
    uint32_t timeout_ms, int wait_forever)
{
    uint32_t legacy_timeout_ms = wait_forever
        ? UINT32_MAX : timeout_ms;
    int64_t result;
    do {
        result = netins_udp_client_h3_request_recv(request, data,
            length, fin, peek, legacy_timeout_ms);
    } while (wait_forever && result == NETINS_UDP_WOULD_BLOCK);
    if (result >= 0) {
        return result;
    }
    if (result == NETINS_UDP_WOULD_BLOCK) {
        errno = !wait_forever && timeout_ms > 0
            ? ETIMEDOUT : EAGAIN;
    } else if (result == NETINS_UDP_CLOSED) {
        errno = ECANCELED;
    } else if (result == NETINS_UDP_BUFFER_TOO_SMALL) {
        errno = ENOBUFS;
    } else {
        errno = EIO;
    }
    return -1;
}

int
netins_udp_v2_transport_h3_request_get_info(
    netins_udp_v2_transport_request_t *request,
    netins_udp_v2_transport_h3_info_t *out)
{
    if (request == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }

    netins_udp_client_h3_info_t internal;
    if (netins_udp_client_h3_request_get_info(request, &internal)
        != NETINS_UDP_OK)
    {
        errno = EIO;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->stream_id = internal.stream_id;
    out->response_status = internal.response_status;
    out->request_headers_ready = internal.request_headers_ready;
    out->response_headers_ready = internal.response_headers_ready;
    out->send_finished = internal.send_finished;
    out->receive_finished = internal.receive_finished;
    out->stream_error = internal.stream_error;
    out->bytes_accepted = internal.bytes_accepted;
    out->bytes_delivered = internal.bytes_delivered;
    snprintf(out->request_authority, sizeof(out->request_authority),
        "%s", internal.request_authority);
    out->response_content_length_known =
        internal.response_content_length_known;
    out->response_content_length = internal.response_content_length;
    snprintf(out->response_content_type,
        sizeof(out->response_content_type), "%s",
        internal.response_content_type);
    out->peer_header_count = internal.peer_header_count;
    for (uint32_t i = 0; i < internal.peer_header_count; ++i) {
        snprintf(out->peer_headers[i].name,
            sizeof(out->peer_headers[i].name), "%s",
            internal.peer_headers[i].name);
        snprintf(out->peer_headers[i].value,
            sizeof(out->peer_headers[i].value), "%s",
            internal.peer_headers[i].value);
    }
    return 0;
}

void
netins_udp_v2_transport_h3_request_close_async(
    netins_udp_v2_transport_request_t *request)
{
    netins_udp_client_h3_request_close_async(request);
}

void
netins_udp_v2_transport_h3_request_close(
    netins_udp_v2_transport_request_t *request,
    uint32_t timeout_ms)
{
    if (request == NULL) {
        return;
    }
    if (netins_udp_stream_close(request, timeout_ms)
        != NETINS_UDP_OK)
    {
        netins_udp_client_h3_request_abandon(request);
    }
}
