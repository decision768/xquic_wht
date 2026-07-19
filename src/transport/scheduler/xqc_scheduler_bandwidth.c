#include "src/transport/scheduler/xqc_scheduler_bandwidth.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_send_ctl.h"

#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define XQC_BW_FILL_RATIO_NUM  9
#define XQC_BW_FILL_RATIO_DEN  10
#define XQC_BW_CAP_SHIFT       10
#define XQC_BW_BETA_NUM        13  /* 1.3x total budget */
#define XQC_BW_BETA_DEN        10

typedef struct {
    xqc_bool_t in_use;
    uint64_t path_id;
    uint64_t samples;
    int64_t score_sum;
    uint64_t available_bytes_sum;
    int64_t last_score;
    uint64_t last_available_bytes;
} xqc_bandwidth_path_diag_t;

typedef struct {
    xqc_bool_t  has_last_path;
    uint64_t    last_path_id;
    xqc_sched_diag_t diag;
    uint64_t selected_available;
    uint64_t selected_standby;
    uint64_t selected_original_reinject;
    int64_t selected_score_sum;
    uint64_t selected_available_bytes_sum;
    xqc_bandwidth_path_diag_t path_diag[XQC_MAX_PATHS_COUNT];
} xqc_bandwidth_scheduler_t;

static void
xqc_bandwidth_scheduler_record_path(xqc_bandwidth_scheduler_t *scheduler,
    xqc_path_ctx_t *path, int64_t score, uint64_t available_bytes)
{
    if (!scheduler || !scheduler->diag.enabled || !path) {
        return;
    }
    xqc_bandwidth_path_diag_t *free_slot = NULL;
    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (scheduler->path_diag[i].in_use
            && scheduler->path_diag[i].path_id == path->path_id)
        {
            free_slot = &scheduler->path_diag[i];
            break;
        }
        if (!scheduler->path_diag[i].in_use && free_slot == NULL) {
            free_slot = &scheduler->path_diag[i];
        }
    }
    if (!free_slot) {
        return;
    }
    if (!free_slot->in_use) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->in_use = XQC_TRUE;
        free_slot->path_id = path->path_id;
    }
    free_slot->samples++;
    free_slot->score_sum += score;
    free_slot->available_bytes_sum += available_bytes;
    free_slot->last_score = score;
    free_slot->last_available_bytes = available_bytes;
}

static size_t
xqc_bandwidth_scheduler_size()
{
    return sizeof(xqc_bandwidth_scheduler_t);
}

static void
xqc_bandwidth_scheduler_init(void *scheduler, xqc_log_t *log, xqc_scheduler_params_t *params)
{
    xqc_bandwidth_scheduler_t *s = (xqc_bandwidth_scheduler_t *)scheduler;
    if (s) {
        memset(s, 0, sizeof(*s));
        xqc_scheduler_diag_init(&s->diag, "bw", params);
    }
}

static uint64_t
xqc_bandwidth_scheduler_available_bytes(xqc_path_ctx_t *path)
{
    xqc_send_ctl_t *ctl = path->path_send_ctl;
    uint64_t bytes_on_path = path->path_schedule_bytes + ctl->ctl_bytes_in_flight;
    uint64_t cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(ctl->ctl_cong);

    if (cwnd <= bytes_on_path) {
        return 0;
    }

    return cwnd - bytes_on_path;
}

static void
xqc_bandwidth_scheduler_get_path_stats(xqc_path_ctx_t *path,
    uint64_t *cwnd, uint64_t *bytes_on_path, uint64_t *srtt)
{
    xqc_send_ctl_t *ctl = path->path_send_ctl;

    *bytes_on_path = path->path_schedule_bytes + ctl->ctl_bytes_in_flight;
    *cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(ctl->ctl_cong);
    *srtt = xqc_send_ctl_get_srtt(ctl);

    if (*srtt == 0) {
        *srtt = 1;
    }
}

static void
xqc_bandwidth_scheduler_compute_scores(uint64_t *cwnd, uint64_t *inflight,
    uint64_t *srtt, int64_t *score, size_t count)
{
    __uint128_t sum_cwnd = 0;
    __uint128_t sum_cap = 0;

    if (count == 0) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        uint64_t rtt = srtt[i] ? srtt[i] : 1;
        sum_cwnd += cwnd[i];
        sum_cap += (((__uint128_t)cwnd[i]) << XQC_BW_CAP_SHIFT) / rtt;
    }

    if (sum_cap == 0) {
        sum_cap = count;
    }

    __uint128_t total_budget = sum_cwnd;
    total_budget = (total_budget * XQC_BW_FILL_RATIO_NUM) / XQC_BW_FILL_RATIO_DEN;
    total_budget = (total_budget * XQC_BW_BETA_NUM) / XQC_BW_BETA_DEN;

    for (size_t i = 0; i < count; i++) {
        uint64_t rtt = srtt[i] ? srtt[i] : 1;
        __uint128_t cap = (((__uint128_t)cwnd[i]) << XQC_BW_CAP_SHIFT) / rtt;
        if (cap == 0) {
            cap = 1;
        }

        __uint128_t target = (total_budget * cap) / sum_cap;
        __uint128_t deficit = target > inflight[i] ? target - inflight[i] : 1;

        score[i] = deficit > INT64_MAX ? INT64_MAX : (int64_t)deficit;
    }
}

static xqc_path_ctx_t *
xqc_bandwidth_scheduler_pick_path(xqc_bandwidth_scheduler_t *scheduler,
    xqc_path_ctx_t **candidates, uint64_t *available_bytes, uint64_t *srtt,
    int64_t *score, size_t count, uint64_t *best_available, int64_t *best_score)
{
    size_t start = 0;
    size_t best_idx;
    uint64_t selected_bytes;
    uint64_t selected_srtt;
    int64_t selected_score;

    if (count == 0) {
        if (best_available) {
            *best_available = 0;
        }
        if (best_score) {
            *best_score = 0;
        }
        return NULL;
    }

    if (scheduler && scheduler->has_last_path) {
        for (size_t i = 0; i < count; i++) {
            if (candidates[i]->path_id == scheduler->last_path_id) {
                start = (i + 1) % count;
                break;
            }
        }
    }

    best_idx = start;
    selected_bytes = available_bytes[start];
    selected_srtt = srtt[start];
    selected_score = score[start];

    for (size_t offset = 1; offset < count; offset++) {
        size_t idx = (start + offset) % count;
        uint64_t candidate_bytes = available_bytes[idx];
        uint64_t candidate_srtt = srtt[idx];
        int64_t candidate_score = score[idx];

        if (candidate_score > selected_score
            || (candidate_score == selected_score && candidate_bytes > selected_bytes)
            || (candidate_score == selected_score && candidate_bytes == selected_bytes
                && candidate_srtt < selected_srtt)
            || (candidate_score == selected_score && candidate_bytes == selected_bytes
                && candidate_srtt == selected_srtt
                && candidates[idx]->path_id < candidates[best_idx]->path_id))
        {
            best_idx = idx;
            selected_score = candidate_score;
            selected_bytes = candidate_bytes;
            selected_srtt = candidate_srtt;
        }
    }

    if (scheduler) {
        scheduler->last_path_id = candidates[best_idx]->path_id;
        scheduler->has_last_path = XQC_TRUE;
    }

    if (best_available) {
        *best_available = selected_bytes;
    }
    if (best_score) {
        *best_score = selected_score;
    }

    return candidates[best_idx];
}

xqc_path_ctx_t *
xqc_bandwidth_scheduler_get_path(void *scheduler,
    xqc_connection_t *conn, xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_bool_t *cc_blocked)
{
    xqc_bandwidth_scheduler_t *bandwidth = (xqc_bandwidth_scheduler_t *)scheduler;

    xqc_path_ctx_t *available_paths[XQC_MAX_PATHS_COUNT];
    uint64_t available_bytes[XQC_MAX_PATHS_COUNT];
    uint64_t available_srtt[XQC_MAX_PATHS_COUNT];
    uint64_t available_cwnd[XQC_MAX_PATHS_COUNT];
    uint64_t available_inflight[XQC_MAX_PATHS_COUNT];
    int64_t available_score[XQC_MAX_PATHS_COUNT];
    size_t available_cnt = 0;

    xqc_path_ctx_t *standby_paths[XQC_MAX_PATHS_COUNT];
    uint64_t standby_bytes[XQC_MAX_PATHS_COUNT];
    uint64_t standby_srtt[XQC_MAX_PATHS_COUNT];
    uint64_t standby_cwnd[XQC_MAX_PATHS_COUNT];
    uint64_t standby_inflight[XQC_MAX_PATHS_COUNT];
    int64_t standby_score[XQC_MAX_PATHS_COUNT];
    size_t standby_cnt = 0;

    xqc_path_ctx_t *original_path = NULL;
    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;
    uint64_t path_srtt = 0;
    uint64_t avail_bytes = 0;
    uint64_t cwnd = 0;
    uint64_t bytes_on_path = 0;
    xqc_bool_t has_active_path = XQC_FALSE;
    xqc_bool_t has_sendable_path = XQC_FALSE;
    xqc_bool_t selected_from_available = XQC_FALSE;
    xqc_bool_t selected_from_standby = XQC_FALSE;
    xqc_bool_t selected_original = XQC_FALSE;

    if (cc_blocked) {
        *cc_blocked = XQC_FALSE;
    }

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);

        if (path->path_state != XQC_PATH_STATE_ACTIVE
            || path->app_path_status == XQC_APP_PATH_STATUS_FROZEN)
        {
            continue;
        }

        has_active_path = XQC_TRUE;

        xqc_sched_path_eval_t evaluation;
        if (!xqc_scheduler_evaluate_path(path, packet_out, check_cwnd,
            &evaluation))
        {
            xqc_scheduler_diag_record_evaluation(
                bandwidth ? &bandwidth->diag : NULL, path, &evaluation);
            continue;
        }
        xqc_scheduler_diag_record_evaluation(
            bandwidth ? &bandwidth->diag : NULL, path, &evaluation);

        has_sendable_path = XQC_TRUE;

        if (reinject && packet_out->po_path_id == path->path_id) {
            original_path = path;
            continue;
        }

        xqc_bandwidth_scheduler_get_path_stats(path, &cwnd, &bytes_on_path, &path_srtt);
        avail_bytes = xqc_bandwidth_scheduler_available_bytes(path);

        if (path->app_path_status == XQC_APP_PATH_STATUS_AVAILABLE) {
            if (available_cnt < XQC_MAX_PATHS_COUNT) {
                available_paths[available_cnt] = path;
                available_bytes[available_cnt] = avail_bytes;
                available_srtt[available_cnt] = path_srtt;
                available_cwnd[available_cnt] = cwnd;
                available_inflight[available_cnt] = bytes_on_path;
                available_cnt++;
            }

        } else if (path->app_path_status == XQC_APP_PATH_STATUS_STANDBY) {
            if (standby_cnt < XQC_MAX_PATHS_COUNT) {
                standby_paths[standby_cnt] = path;
                standby_bytes[standby_cnt] = avail_bytes;
                standby_srtt[standby_cnt] = path_srtt;
                standby_cwnd[standby_cnt] = cwnd;
                standby_inflight[standby_cnt] = bytes_on_path;
                standby_cnt++;
            }
        }

        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|bandwidth scheduler|conn:%p|path:%ui|status:%d|srtt:%ui|"
                "cwnd:%ui|inflight:%ui|avail:%ui|",
                conn, path->path_id, path->app_path_status, path_srtt,
                cwnd, bytes_on_path, avail_bytes);
    }

    if (cc_blocked) {
        *cc_blocked = (has_active_path && !has_sendable_path) ? XQC_TRUE : XQC_FALSE;
    }

    xqc_bandwidth_scheduler_compute_scores(available_cwnd, available_inflight,
        available_srtt, available_score, available_cnt);
    xqc_bandwidth_scheduler_compute_scores(standby_cwnd, standby_inflight,
        standby_srtt, standby_score, standby_cnt);
    for (size_t i = 0; i < available_cnt; i++) {
        xqc_bandwidth_scheduler_record_path(bandwidth, available_paths[i],
            available_score[i], available_bytes[i]);
    }
    for (size_t i = 0; i < standby_cnt; i++) {
        xqc_bandwidth_scheduler_record_path(bandwidth, standby_paths[i],
            standby_score[i], standby_bytes[i]);
    }

    xqc_path_ctx_t *best_path = NULL;
    uint64_t best_available = 0;
    int64_t best_score = 0;

    best_path = xqc_bandwidth_scheduler_pick_path(bandwidth, available_paths,
        available_bytes, available_srtt, available_score, available_cnt,
        &best_available, &best_score);
    selected_from_available = best_path != NULL;

    if (best_path == NULL) {
        best_path = xqc_bandwidth_scheduler_pick_path(bandwidth, standby_paths,
            standby_bytes, standby_srtt, standby_score, standby_cnt,
            &best_available, &best_score);
        selected_from_standby = best_path != NULL;
    }

    if (best_path == NULL && original_path != NULL
        && !(packet_out->po_flag & XQC_POF_REINJECT_DIFF_PATH))
    {
        best_path = original_path;
        selected_original = XQC_TRUE;
    }

    if (best_path == NULL) {
        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|bandwidth scheduler|No available paths to schedule|conn:%p|", conn);
        xqc_scheduler_diag_record_decision(
            bandwidth ? &bandwidth->diag : NULL, NULL,
            packet_out ? packet_out->po_used_size : 0,
            has_active_path && !has_sendable_path);
        xqc_scheduler_diag_maybe_tick(
            bandwidth ? &bandwidth->diag : NULL, conn, 0);
        return NULL;
    }

    xqc_log(conn->log, XQC_LOG_DEBUG,
            "|bandwidth scheduler|best path:%ui|frame_type:%s|available:%ui|score:%ld|",
            best_path->path_id,
            xqc_frame_type_2_str(conn->engine, packet_out->po_frame_types),
            best_available, (long)best_score);

    if (bandwidth && bandwidth->diag.enabled) {
        bandwidth->selected_available += selected_from_available;
        bandwidth->selected_standby += selected_from_standby;
        bandwidth->selected_original_reinject += selected_original;
        bandwidth->selected_score_sum += best_score;
        bandwidth->selected_available_bytes_sum += best_available;
    }
    xqc_scheduler_diag_record_decision(
        bandwidth ? &bandwidth->diag : NULL, best_path,
        packet_out ? packet_out->po_used_size : 0, XQC_FALSE);
    xqc_scheduler_diag_maybe_tick(
        bandwidth ? &bandwidth->diag : NULL, conn, 0);

    return best_path;
}

static void
xqc_bandwidth_scheduler_conn_close_diag(void *scheduler,
    xqc_connection_t *conn)
{
    xqc_bandwidth_scheduler_t *bandwidth = scheduler;
    if (!bandwidth || !bandwidth->diag.enabled
        || bandwidth->diag.summary_emitted)
    {
        return;
    }
    printf("[diag_bw_summary] selected_available=%" PRIu64
           " selected_standby=%" PRIu64
           " selected_original_reinject=%" PRIu64
           " selected_score_sum=%" PRId64
           " selected_available_bytes_sum=%" PRIu64 "\n",
           bandwidth->selected_available, bandwidth->selected_standby,
           bandwidth->selected_original_reinject,
           bandwidth->selected_score_sum,
           bandwidth->selected_available_bytes_sum);
    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        xqc_bandwidth_path_diag_t *path = &bandwidth->path_diag[i];
        if (!path->in_use) {
            continue;
        }
        printf("[diag_bw_path] id=%" PRIu64 " samples=%" PRIu64
               " score_avg=%" PRId64 " score_last=%" PRId64
               " available_bytes_avg=%" PRIu64
               " available_bytes_last=%" PRIu64 "\n", path->path_id,
               path->samples,
               path->samples ? path->score_sum / (int64_t)path->samples : 0,
               path->last_score,
               path->samples
                   ? path->available_bytes_sum / path->samples : 0,
               path->last_available_bytes);
    }
    xqc_scheduler_diag_close(&bandwidth->diag, conn);
}

const xqc_scheduler_callback_t xqc_bandwidth_scheduler_cb = {
    .xqc_scheduler_size             = xqc_bandwidth_scheduler_size,
    .xqc_scheduler_init             = xqc_bandwidth_scheduler_init,
    .xqc_scheduler_get_path         = xqc_bandwidth_scheduler_get_path,
    .xqc_scheduler_conn_close_diag  = xqc_bandwidth_scheduler_conn_close_diag,
};
