#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/common/xqc_time.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define XQC_MIN_PATH_RTT_PROBE_TIMEOUT_US 200000
#define XQC_BOOTSTRAP_PATH_RTT_PROBE_TIMEOUT_US 50000

xqc_sched_block_reason_t
xqc_scheduler_classify_cwnd_block(uint64_t effective_cwnd,
    uint64_t inflight, uint64_t scheduled, uint32_t packet_size,
    int check_cwnd)
{
    if (!check_cwnd) {
        return XQC_SCHED_BLOCK_NONE;
    }
    if (inflight > effective_cwnd
        || packet_size > effective_cwnd - inflight)
    {
        return XQC_SCHED_BLOCK_REAL_INFLIGHT;
    }
    uint64_t after_inflight = effective_cwnd - inflight - packet_size;
    if (scheduled > after_inflight) {
        return XQC_SCHED_BLOCK_SCHEDULED_RESERVATION;
    }
    return XQC_SCHED_BLOCK_NONE;
}

xqc_bool_t
xqc_scheduler_evaluate_path(xqc_path_ctx_t *path,
    xqc_packet_out_t *packet_out, int check_cwnd,
    xqc_sched_path_eval_t *evaluation)
{
    xqc_sched_path_eval_t local;
    memset(&local, 0, sizeof(local));
    xqc_send_ctl_t *ctl = path ? path->path_send_ctl : NULL;
    if (ctl && ctl->ctl_cong_callback
        && ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd)
    {
        local.raw_cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(
            ctl->ctl_cong);
    }
    local.effective_cwnd = xqc_send_ctl_calc_effective_cwnd(local.raw_cwnd,
        ctl && ctl->ctl_conn && ctl->ctl_conn->conn_settings.so_sndbuf > 0
            ? (uint64_t)ctl->ctl_conn->conn_settings.so_sndbuf : 0);
    local.bytes_in_flight = ctl ? ctl->ctl_bytes_in_flight : 0;
    local.path_schedule_bytes = path ? path->path_schedule_bytes : 0;
    local.packet_size = packet_out ? packet_out->po_used_size : 0;
    uint64_t occupied = local.bytes_in_flight + local.path_schedule_bytes;
    if (occupied >= local.bytes_in_flight
        && local.effective_cwnd > occupied)
    {
        local.headroom = local.effective_cwnd - occupied;
    }
    if (!check_cwnd || (packet_out
        && !XQC_CAN_IN_FLIGHT(packet_out->po_frame_types)))
    {
        local.block_reason = XQC_SCHED_BLOCK_NONE;
        local.can_send = XQC_TRUE;

    } else {
        local.block_reason = xqc_scheduler_classify_cwnd_block(
            local.effective_cwnd, local.bytes_in_flight,
            local.path_schedule_bytes, local.packet_size, check_cwnd);
        local.can_send = ctl && packet_out
            && xqc_send_ctl_can_send(ctl, packet_out,
                path ? path->path_schedule_bytes : 0);
        if (!local.can_send && ctl && ctl->ctl_conn) {
            xqc_log(ctl->ctl_conn->log, XQC_LOG_DEBUG,
                "|path:%ui|blocked by cwnd|", path->path_id);
        }
    }
    if (evaluation) {
        *evaluation = local;
    }
    return local.can_send;
}

static xqc_sched_diag_path_t *
xqc_scheduler_diag_get_path(xqc_sched_diag_t *diag, uint64_t path_id)
{
    xqc_sched_diag_path_t *free_slot = NULL;
    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (diag->paths[i].in_use && diag->paths[i].path_id == path_id) {
            return &diag->paths[i];
        }
        if (!diag->paths[i].in_use && free_slot == NULL) {
            free_slot = &diag->paths[i];
        }
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->in_use = XQC_TRUE;
        free_slot->path_id = path_id;
    }
    return free_slot;
}

void
xqc_scheduler_diag_init(xqc_sched_diag_t *diag, const char *sched_name,
    const xqc_scheduler_params_t *params)
{
    memset(diag, 0, sizeof(*diag));
    diag->sched_name = sched_name;
    diag->enabled = params && params->mp_diag_enabled;
    uint32_t interval_ms = params && params->mp_diag_interval_ms
        ? params->mp_diag_interval_ms : 250;
    diag->interval_us = (xqc_usec_t)interval_ms * 1000;
}

xqc_sched_diag_path_t *
xqc_scheduler_diag_record_evaluation(xqc_sched_diag_t *diag,
    xqc_path_ctx_t *path, const xqc_sched_path_eval_t *evaluation)
{
    if (!diag || !diag->enabled || !path || !evaluation) {
        return NULL;
    }
    xqc_sched_diag_path_t *item = xqc_scheduler_diag_get_path(diag,
        path->path_id);
    if (!item) {
        return NULL;
    }
    item->checks++;
    item->last_eval = *evaluation;
    if (evaluation->can_send) {
        item->sendable++;
    } else if (evaluation->block_reason == XQC_SCHED_BLOCK_REAL_INFLIGHT) {
        item->blocked_real++;
    } else if (evaluation->block_reason == XQC_SCHED_BLOCK_SCHEDULED_RESERVATION) {
        item->blocked_scheduled++;
    }
    return item;
}

void
xqc_scheduler_diag_record_decision(xqc_sched_diag_t *diag,
    xqc_path_ctx_t *selected, size_t packet_size, xqc_bool_t no_sendable)
{
    if (!diag || !diag->enabled) {
        return;
    }
    diag->decisions++;
    if (no_sendable) {
        diag->active_no_sendable++;
    }
    if (selected) {
        xqc_sched_diag_path_t *item = xqc_scheduler_diag_get_path(diag,
            selected->path_id);
        if (item) {
            item->selected_packets++;
            item->selected_bytes += packet_size;
        }
    }
}

void
xqc_scheduler_diag_maybe_tick(xqc_sched_diag_t *diag,
    xqc_connection_t *conn, xqc_usec_t now)
{
    if (!diag || !diag->enabled || !conn) {
        return;
    }
    if (now == 0) {
        diag->tick_poll_count++;
        if (diag->start_us != 0 && (diag->tick_poll_count & 63) != 0) {
            return;
        }
        now = xqc_monotonic_timestamp();
    }
    if (diag->start_us == 0) {
        diag->start_us = now;
        diag->last_tick_us = now;
        return;
    }
    if (now - diag->last_tick_us < diag->interval_us) {
        return;
    }
    uint64_t elapsed_ms = (now - diag->start_us) / 1000;
    xqc_list_head_t *pos, *next;
    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        xqc_path_ctx_t *path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        xqc_send_ctl_t *ctl = path->path_send_ctl;
        if (!ctl) {
            continue;
        }
        xqc_sched_diag_path_t *item = xqc_scheduler_diag_get_path(diag,
            path->path_id);
        if (!item) {
            continue;
        }
        uint64_t est_bw = xqc_send_ctl_get_est_bw(ctl);
        uint64_t pacing = xqc_send_ctl_get_pacing_rate(ctl);
        printf("[diag_sched_tick] sched=%s elapsed_ms=%" PRIu64
               " path=%" PRIu64 " active=%d selected_delta=%" PRIu64
               " cwnd=%" PRIu64 " effective_cwnd=%" PRIu64
               " inflight=%u scheduled=%u headroom=%" PRIu64
               " est_bw_Bps=%" PRIu64 " pacing_Bps=%" PRIu64
               " srtt_us=%" PRIu64 " app_limited=%" PRIu64
               " cwnd_limited=%u\n",
               diag->sched_name, elapsed_ms, path->path_id,
               path->path_state == XQC_PATH_STATE_ACTIVE
                   && path->app_path_status != XQC_APP_PATH_STATUS_FROZEN,
               item->selected_packets - item->tick_selected_base,
               item->last_eval.raw_cwnd, item->last_eval.effective_cwnd,
               ctl->ctl_bytes_in_flight, path->path_schedule_bytes,
               item->last_eval.headroom, est_bw, pacing,
               xqc_send_ctl_get_srtt(ctl), ctl->ctl_app_limited,
               ctl->ctl_is_cwnd_limited);
        item->tick_selected_base = item->selected_packets;
    }
    diag->last_tick_us = now;
}

void
xqc_scheduler_diag_close(xqc_sched_diag_t *diag, xqc_connection_t *conn)
{
    if (!diag || !diag->enabled || diag->summary_emitted || !conn) {
        return;
    }
    diag->summary_emitted = XQC_TRUE;
    uint64_t packets = 0, bytes = 0;
    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (diag->paths[i].in_use) {
            packets += diag->paths[i].selected_packets;
            bytes += diag->paths[i].selected_bytes;
        }
    }
    printf("[diag_sched_summary] sched=%s decisions=%" PRIu64
           " active_no_sendable=%" PRIu64 " selected_packets=%" PRIu64
           " selected_bytes=%" PRIu64 "\n", diag->sched_name,
           diag->decisions, diag->active_no_sendable, packets, bytes);
    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        xqc_sched_diag_path_t *item = &diag->paths[i];
        if (!item->in_use) {
            continue;
        }
        printf("[diag_sched_path] sched=%s id=%" PRIu64
               " checks=%" PRIu64 " sendable=%" PRIu64
               " blocked_real=%" PRIu64 " blocked_scheduled=%" PRIu64
               " selected_packets=%" PRIu64 " selected_bytes=%" PRIu64
               "\n", diag->sched_name, item->path_id, item->checks,
               item->sendable, item->blocked_real, item->blocked_scheduled,
               item->selected_packets, item->selected_bytes);
    }
}

xqc_bool_t
xqc_scheduler_check_path_can_send(xqc_path_ctx_t *path, xqc_packet_out_t *packet_out, int check_cwnd)
{
    return xqc_scheduler_evaluate_path(path, packet_out, check_cwnd, NULL);
}

static xqc_usec_t
xqc_scheduler_get_path_probe_timeout(xqc_path_ctx_t *path)
{
    xqc_connection_t *conn = path->parent_conn;
    xqc_send_ctl_t *send_ctl = path->path_send_ctl;

    if (send_ctl->ctl_first_rtt_sample_time == 0) {
        return XQC_BOOTSTRAP_PATH_RTT_PROBE_TIMEOUT_US;
    }

    xqc_usec_t config_timeout = conn->conn_settings.standby_path_probe_timeout * 1000;
    xqc_usec_t srtt = send_ctl->ctl_srtt ? send_ctl->ctl_srtt : conn->conn_settings.initial_rtt;
    xqc_usec_t active_probe_timeout = xqc_max(4 * srtt, (xqc_usec_t)XQC_MIN_PATH_RTT_PROBE_TIMEOUT_US);

    return xqc_min(config_timeout, active_probe_timeout);
}

void
xqc_scheduler_handle_path_probe(xqc_connection_t *conn)
{
    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;
    xqc_send_ctl_t *send_ctl;
    xqc_usec_t now, probe_timeout, last_send_time, probe_deadline;

    if (!conn->enable_multipath
        || conn->conn_state != XQC_CONN_STATE_ESTABED)
    {
        return;
    }

    now = xqc_monotonic_timestamp();

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);

        if (path->path_state != XQC_PATH_STATE_ACTIVE
            || path->app_path_status == XQC_APP_PATH_STATUS_FROZEN)
        {
            continue;
        }

        if (conn->conn_settings.standby_path_probe_timeout == 0
            && path->path_send_ctl->ctl_first_rtt_sample_time != 0)
        {
            continue;
        }

        send_ctl = path->path_send_ctl;
        probe_timeout = xqc_scheduler_get_path_probe_timeout(path);
        last_send_time = send_ctl->ctl_time_of_last_sent_ack_eliciting_packet[XQC_PNS_APP_DATA];
        probe_deadline = xqc_max(last_send_time, path->path_last_rtt_probe_time) + probe_timeout;

        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|path_rtt_probe|path:%ui|deadline:%ui|now:%ui|timeout:%ui|"
                "last_send:%ui|last_probe:%ui|status:%d|",
                path->path_id, probe_deadline, now, probe_timeout,
                last_send_time, path->path_last_rtt_probe_time,
                path->app_path_status);

        if (now < probe_deadline) {
            continue;
        }

        if (xqc_path_send_ping_to_probe(path, XQC_PNS_APP_DATA, XQC_PATH_SPECIFIED_BY_PQP) == XQC_OK) {
            path->path_last_rtt_probe_time = now;
            if (path->app_path_status == XQC_APP_PATH_STATUS_STANDBY) {
                path->standby_probe_count++;
            }
        }
    }
}
