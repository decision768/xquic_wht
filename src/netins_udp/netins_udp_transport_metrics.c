#include "netins_udp_transport_metrics.h"

#include <string.h>

#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_engine.h"
#include "src/transport/xqc_send_ctl.h"

static uint64_t
netins_udp_transport_cwnd(const xqc_send_ctl_t *send_ctl)
{
    if (send_ctl == NULL || send_ctl->ctl_cong_callback == NULL
        || send_ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd == NULL
        || send_ctl->ctl_cong == NULL)
    {
        return 0;
    }
    return send_ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(
        send_ctl->ctl_cong);
}

static netins_udp_path_state_t
netins_udp_transport_path_state(xqc_path_state_t state)
{
    switch (state) {
    case XQC_PATH_STATE_INIT:
        return NETINS_UDP_PATH_INIT;
    case XQC_PATH_STATE_VALIDATING:
        return NETINS_UDP_PATH_VALIDATING;
    case XQC_PATH_STATE_ACTIVE:
        return NETINS_UDP_PATH_ACTIVE;
    case XQC_PATH_STATE_CLOSING:
        return NETINS_UDP_PATH_CLOSING;
    case XQC_PATH_STATE_CLOSED:
        return NETINS_UDP_PATH_CLOSED;
    default:
        return NETINS_UDP_PATH_CLOSED;
    }
}

static netins_udp_app_path_status_t
netins_udp_transport_app_path_status(xqc_app_path_status_t status)
{
    switch (status) {
    case XQC_APP_PATH_STATUS_STANDBY:
        return NETINS_UDP_APP_PATH_STANDBY;
    case XQC_APP_PATH_STATUS_AVAILABLE:
        return NETINS_UDP_APP_PATH_AVAILABLE;
    case XQC_APP_PATH_STATUS_FROZEN:
        return NETINS_UDP_APP_PATH_FROZEN;
    case XQC_APP_PATH_STATUS_NONE:
    default:
        return NETINS_UDP_APP_PATH_NONE;
    }
}

int
netins_udp_transport_payload_metrics_set_enabled(xqc_engine_t *engine,
    const xqc_cid_t *cid, int enabled)
{
    if (engine == NULL || cid == NULL) {
        return -1;
    }
    xqc_connection_t *conn = xqc_engine_conns_hash_find(engine, cid, 's');
    if (conn == NULL) {
        return -1;
    }
    conn->netins_udp_payload_metrics_enabled = enabled != 0;
    return 0;
}

int
netins_udp_transport_metrics_collect(xqc_engine_t *engine,
    const xqc_cid_t *cid, netins_udp_transport_metrics_t *metrics)
{
    if (engine == NULL || cid == NULL || metrics == NULL) {
        return -1;
    }

    memset(metrics, 0, sizeof(*metrics));
    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; ++i) {
        metrics->legacy.paths_info[i].path_id = UINT64_MAX;
        metrics->paths[i].path_id = UINT64_MAX;
    }

    xqc_connection_t *conn = xqc_engine_conns_hash_find(engine, cid, 's');
    if (conn == NULL) {
        return -1;
    }

    /*
     * Keep legacy connection counters and text formatting in one place.  The
     * extended path data below never changes the public stats structure.
     */
    xqc_conn_get_stats_internal(conn, &metrics->legacy);

    xqc_list_head_t *pos;
    xqc_list_head_t *next;
    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        if (metrics->path_count >= XQC_MAX_PATHS_COUNT) {
            break;
        }

        xqc_path_ctx_t *path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        if (path == NULL || path->path_send_ctl == NULL) {
            continue;
        }

        xqc_send_ctl_t *send_ctl = path->path_send_ctl;
        netins_udp_transport_path_metrics_t *dst =
            &metrics->paths[metrics->path_count++];

        dst->path_id = path->path_id;
        dst->path_state = netins_udp_transport_path_state(path->path_state);
        dst->app_path_status =
            netins_udp_transport_app_path_status(path->app_path_status);
        dst->packets_sent = send_ctl->ctl_send_count;
        dst->packets_recv = send_ctl->ctl_recv_count;
        dst->app_packet_bytes_sent = send_ctl->ctl_app_bytes_send;
        dst->wire_bytes_sent = send_ctl->ctl_bytes_send;
        dst->wire_bytes_recv = send_ctl->ctl_bytes_recv;
        dst->delivered_bytes = send_ctl->ctl_delivered;
        dst->reinjected_bytes_sent = send_ctl->ctl_reinjected_bytes_send;
        dst->retransmitted_bytes_sent = send_ctl->ctl_retrans_bytes_send;
        dst->app_packet_bytes_recv = send_ctl->ctl_app_bytes_recv;
        dst->reinjected_bytes_recv = send_ctl->ctl_reinjected_bytes_recv;
        dst->stream_payload_bytes_sent_unique =
            send_ctl->ctl_app_payload_bytes_send_unique;
        dst->stream_payload_bytes_recv_unique =
            send_ctl->ctl_app_payload_bytes_recv_unique;
        dst->stream_payload_bytes_recv_unique_reinject =
            send_ctl->ctl_app_payload_bytes_recv_unique_reinject;

        if (send_ctl->ctl_first_rtt_sample_time != 0) {
            dst->srtt_us = send_ctl->ctl_srtt;
            dst->latest_rtt_us = send_ctl->ctl_latest_rtt;
            dst->min_rtt_us = send_ctl->ctl_minrtt;
        }
        dst->cwnd_bytes = netins_udp_transport_cwnd(send_ctl);
        dst->inflight_bytes = send_ctl->ctl_bytes_in_flight;
        dst->cwnd_limited = send_ctl->ctl_is_cwnd_limited;
        dst->cwnd_block_count = send_ctl->diag.send_cwnd_block_count;
        dst->cwnd_block_duration_us =
            send_ctl->diag.send_cwnd_block_duration_us;
        dst->lost_packets = send_ctl->ctl_lost_count;
        dst->last_sent_time_us = send_ctl->ctl_last_inflight_pkt_sent_time;
        for (size_t pns = 0; pns < XQC_PNS_N; ++pns) {
            if (send_ctl->ctl_largest_recv_time[pns]
                > dst->last_received_time_us)
            {
                dst->last_received_time_us =
                    send_ctl->ctl_largest_recv_time[pns];
            }
            if (send_ctl->ctl_time_of_last_sent_ack_eliciting_packet[pns]
                > dst->last_sent_time_us)
            {
                dst->last_sent_time_us =
                    send_ctl
                        ->ctl_time_of_last_sent_ack_eliciting_packet[pns];
            }
        }
    }

    return 0;
}
