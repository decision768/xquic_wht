#include "netins_udp_v2_transport.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "netins_udp_legacy.h"
#include <xquic/xquic.h>

#include "netins_udp_client_internal.h"

_Static_assert(NETINS_UDP_MAX_PATHS
        == NETINS_UDP_V2_TRANSPORT_MAX_PATHS,
    "compatibility client and transport path limits must match");
_Static_assert(NETINS_UDP_SERVER_ADDRESS_LEN
        == NETINS_UDP_V2_TRANSPORT_HOST_LEN,
    "compatibility client and transport host limits must match");
_Static_assert(NETINS_UDP_SERVER_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_SERVER_NAME_LEN,
    "compatibility client and transport server-name limits must match");
_Static_assert(NETINS_UDP_INTERFACE_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_INTERFACE_NAME_LEN,
    "compatibility client and transport interface-name limits must match");
_Static_assert(NETINS_UDP_METHOD_LEN
        == NETINS_UDP_V2_TRANSPORT_METHOD_LEN,
    "compatibility client and transport method limits must match");
_Static_assert(NETINS_UDP_REQUEST_PATH_LEN
        == NETINS_UDP_V2_TRANSPORT_REQUEST_PATH_LEN,
    "compatibility client and transport request-path limits must match");
_Static_assert(NETINS_UDP_CONTENT_TYPE_LEN
        == NETINS_UDP_V2_TRANSPORT_CONTENT_TYPE_LEN,
    "compatibility client and transport content-type limits must match");
_Static_assert(NETINS_UDP_CLIENT_H3_MAX_HEADERS
        == NETINS_UDP_V2_TRANSPORT_MAX_H3_HEADERS,
    "compatibility client and transport H3 header counts must match");
_Static_assert(NETINS_UDP_CLIENT_H3_HEADER_NAME_LEN
        == NETINS_UDP_V2_TRANSPORT_H3_HEADER_NAME_LEN,
    "compatibility client and transport H3 header-name limits must match");
_Static_assert(NETINS_UDP_CLIENT_H3_HEADER_VALUE_LEN
        == NETINS_UDP_V2_TRANSPORT_H3_HEADER_VALUE_LEN,
    "compatibility client and transport H3 header-value limits must match");

static int
netins_udp_v2_transport_legacy_state(int state)
{
    switch (state) {
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

static int
netins_udp_v2_transport_map_private_snapshot(
    const netins_udp_observer_private_snapshot_t *source,
    netins_udp_v2_transport_snapshot_t *out)
{
    if (source == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    const netins_udp_snapshot_t *legacy = &source->legacy;
    const netins_udp_observer_extended_snapshot_t *extended =
        &source->extended;
    memset(out, 0, sizeof(*out));
    out->connection_id = legacy->info.connection_id;
    out->state = netins_udp_v2_transport_legacy_state(
        legacy->info.connection_state);
    out->handshake_finished = legacy->info.handshake_finished;
    out->multipath_mode = legacy->info.multipath_mode;
    out->multipath_active = legacy->info.multipath_mode != 0
        && (legacy->info.mp_state == 1
            || legacy->info.active_path_count > 1);
    out->connection_error = legacy->info.conn_error;
    out->path_count = legacy->path_count;
    out->active_path_count = legacy->info.active_path_count;
    out->validating_path_count = legacy->info.validating_path_count;
    out->path_table_truncated = extended->path_table_truncated;
    snprintf(out->alpn, sizeof(out->alpn), "%s", legacy->info.alpn);

    out->snapshot_time_us = legacy->statis.snapshot_time_us;
    out->sample_interval_us = legacy->statis.sample_interval_us;
    out->bytes_wire_sent = legacy->statis.bytes_wire_sent;
    out->bytes_acked = legacy->statis.bytes_delivered;
    out->bytes_inflight = legacy->statis.inflight_bytes;
    out->acked_mbps = legacy->statis.delivery_mbps;
    out->loss_rate = legacy->statis.loss_rate;

    uint32_t count = out->path_count;
    if (count > NETINS_UDP_V2_TRANSPORT_MAX_PATHS) {
        count = NETINS_UDP_V2_TRANSPORT_MAX_PATHS;
        out->path_table_truncated = 1;
    }
    out->path_count = count;
    for (uint32_t i = 0; i < count; ++i) {
        const netins_udp_path_statis_t *legacy_path = &legacy->paths[i];
        const netins_udp_observer_extended_path_t *extended_path =
            &extended->paths[i];
        netins_udp_v2_transport_path_snapshot_t *path = &out->paths[i];
        path->path_id = legacy_path->path_id;
        path->path_index = i;
        path->path_state = legacy_path->transport_state;
        path->app_status = legacy_path->app_status;
        path->cwnd_blocked = extended_path->cwnd_limited;
        snprintf(path->interface_name, sizeof(path->interface_name), "%s",
            legacy_path->interface_name);
        snprintf(path->local_address, sizeof(path->local_address), "%s",
            legacy_path->local_address);
        snprintf(path->peer_address, sizeof(path->peer_address), "%s",
            legacy_path->peer_address);
        path->bytes_sent =
            extended_path->stream_payload_bytes_sent_unique;
        path->bytes_received =
            extended_path->stream_payload_bytes_recv_unique;
        path->bytes_wire_sent = legacy_path->bytes_wire_sent;
        path->bytes_wire_received = extended_path->wire_bytes_recv;
        path->bytes_acked = legacy_path->bytes_delivered;
        /*
         * XQUIC does not retain an exact lost-byte counter.  Encoded bytes
         * retransmitted after loss are the stable byte-volume proxy; the
         * exact cumulative loss ratio below remains packet based.
         */
        path->bytes_lost = extended_path->retransmitted_bytes_sent;
        path->bytes_inflight = extended_path->inflight_bytes;
        path->cwnd_bytes = extended_path->cwnd_bytes;
        path->srtt_us = legacy_path->srtt_us;
        path->latest_rtt_us = extended_path->latest_rtt_us;
        path->min_rtt_us = extended_path->min_rtt_us;
        path->last_received_time_us =
            extended_path->last_received_time_us;
        path->last_sent_time_us = extended_path->last_sent_time_us;
        path->send_mbps = extended_path->unique_send_mbps;
        path->receive_mbps = extended_path->unique_recv_mbps;
        path->acked_mbps = legacy_path->delivery_mbps;
        path->loss_rate = legacy_path->packets_sent > 0
            ? (double)extended_path->lost_packets
                / (double)legacy_path->packets_sent
            : 0.0;

        out->bytes_sent += path->bytes_sent;
        out->bytes_received += path->bytes_received;
        out->bytes_wire_received += path->bytes_wire_received;
        out->bytes_lost += path->bytes_lost;
        out->send_mbps += path->send_mbps;
        out->receive_mbps += path->receive_mbps;
        if (out->local_address[0] == '\0'
            && path->local_address[0] != '\0')
        {
            snprintf(out->local_address, sizeof(out->local_address), "%s",
                path->local_address);
        }
        if (out->peer_address[0] == '\0'
            && path->peer_address[0] != '\0')
        {
            snprintf(out->peer_address, sizeof(out->peer_address), "%s",
                path->peer_address);
        }
    }
    return 0;
}

int
netins_udp_v2_transport_snapshot_from_observer(
    struct netins_udp_observer_s *observer,
    netins_udp_v2_transport_snapshot_t *out)
{
    netins_udp_observer_private_snapshot_t source;
    if (observer == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (netins_udp_observer_get_private_snapshot(observer, &source)
        != NETINS_UDP_OK)
    {
        errno = EIO;
        return -1;
    }
    return netins_udp_v2_transport_map_private_snapshot(&source, out);
}

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
        if (errno == 0) {
            errno = EIO;
        }
        return -1;
    }
    netins_udp_client_set_notify(client, config->notify,
        config->notify_user_data);
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

int
netins_udp_v2_transport_client_get_snapshot(
    netins_udp_v2_transport_client_t *client,
    netins_udp_v2_transport_snapshot_t *out)
{
    netins_udp_observer_private_snapshot_t source;
    if (client == NULL || out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (netins_udp_client_get_private_snapshot(client, &source)
        != NETINS_UDP_OK)
    {
        errno = EIO;
        return -1;
    }
    return netins_udp_v2_transport_map_private_snapshot(&source, out);
}

int
netins_udp_v2_transport_client_get_events(
    netins_udp_v2_transport_client_t *client, uint32_t *out_events)
{
    if (netins_udp_client_get_events(client, out_events)
        != NETINS_UDP_OK)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void
netins_udp_v2_transport_client_set_notify(
    netins_udp_v2_transport_client_t *client,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    netins_udp_client_set_notify(client, notify, user_data);
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
netins_udp_v2_transport_client_request_shutdown(
    netins_udp_v2_transport_client_t *client, uint64_t application_error)
{
    netins_udp_client_request_shutdown(client, application_error);
}

void
netins_udp_v2_transport_client_close(
    netins_udp_v2_transport_client_t *client)
{
    netins_udp_client_close(client);
}

int
netins_udp_v2_transport_has_pending_resolvers(void)
{
    return netins_udp_client_has_pending_resolvers();
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
        if (errno == 0) {
            errno = EIO;
        }
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
        netins_udp_client_h3_info_t info;
        memset(&info, 0, sizeof(info));
        errno = netins_udp_client_h3_request_get_info(request, &info)
                == NETINS_UDP_OK
                && info.stream_system_error != 0
            ? info.stream_system_error : EIO;
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
        netins_udp_client_h3_info_t info;
        memset(&info, 0, sizeof(info));
        errno = netins_udp_client_h3_request_get_info(request, &info)
                == NETINS_UDP_OK
                && info.stream_system_error != 0
            ? info.stream_system_error : EIO;
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
    out->stream_system_error = internal.stream_system_error;
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

int
netins_udp_v2_transport_h3_request_get_events(
    netins_udp_v2_transport_request_t *request, uint32_t *out_events)
{
    if (netins_udp_client_h3_request_get_events(request, out_events)
        != NETINS_UDP_OK)
    {
        errno = EINVAL;
        return -1;
    }
    return 0;
}

void
netins_udp_v2_transport_h3_request_set_notify(
    netins_udp_v2_transport_request_t *request,
    netins_udp_v2_transport_notify_cb notify, void *user_data)
{
    netins_udp_client_h3_request_set_notify(request, notify, user_data);
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
