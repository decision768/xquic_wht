#include "src/transport/scheduler/xqc_scheduler_balanced.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_packet_out.h"


typedef struct {
    xqc_bool_t  has_last_path;
    uint64_t    last_path_id;
    uint64_t    pick_count;
} xqc_balanced_scheduler_t;

#define XQC_BALANCED_SCORE_SHIFT          10
#define XQC_BALANCED_UTIL_SCALE           1024
#define XQC_BALANCED_UTIL_BOOST_CEIL      1536
#define XQC_BALANCED_HIGH_SRTT_FACTOR     4
#define XQC_BALANCED_GUARD_INTERVAL       16
#define XQC_BALANCED_SKEW_RATIO           8
#define XQC_BALANCED_SKEW_MIN_BYTES       (64 * 1024)


static size_t
xqc_balanced_scheduler_size()
{
    return sizeof(xqc_balanced_scheduler_t);
}

static void
xqc_balanced_scheduler_init(void *scheduler, xqc_log_t *log, xqc_scheduler_params_t *params)
{
    xqc_balanced_scheduler_t *balanced = (xqc_balanced_scheduler_t *) scheduler;
    if (balanced == NULL) {
        return;
    }

    balanced->has_last_path = XQC_FALSE;
    balanced->last_path_id = 0;
    balanced->pick_count = 0;

    return;
}

static uint64_t
xqc_balanced_scheduler_available_bytes(xqc_path_ctx_t *path)
{
    xqc_send_ctl_t *ctl = path->path_send_ctl;
    uint64_t bytes_on_path = path->path_schedule_bytes + ctl->ctl_bytes_in_flight;
    uint64_t cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(ctl->ctl_cong);

    if (cwnd <= bytes_on_path) {
        return 0;
    }

    return cwnd - bytes_on_path;
}
static uint64_t
xqc_balanced_scheduler_get_effective_srtt(xqc_send_ctl_t *ctl)
{
    uint64_t srtt = xqc_send_ctl_get_srtt(ctl);
    uint64_t min_rtt = ctl->ctl_minrtt;

    if (srtt == 0) {
        srtt = ctl->ctl_conn->conn_settings.initial_rtt;
    }

    if (min_rtt != 0 && min_rtt != XQC_MAX_UINT32_VALUE
        && srtt > min_rtt * XQC_BALANCED_HIGH_SRTT_FACTOR)
    {
        srtt = min_rtt * XQC_BALANCED_HIGH_SRTT_FACTOR;
    }

    return srtt ? srtt : 1;
}

static uint64_t
xqc_balanced_scheduler_compute_score(xqc_path_ctx_t *path, uint64_t avail_bytes,
    uint64_t effective_srtt)
{
    xqc_send_ctl_t *ctl = path->path_send_ctl;
    uint64_t bytes_on_path = path->path_schedule_bytes + ctl->ctl_bytes_in_flight;
    uint64_t cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(ctl->ctl_cong);
    uint64_t utilization = XQC_BALANCED_UTIL_SCALE;
    __uint128_t capacity = 0;
    __uint128_t score = 0;

    if (cwnd > 0) {
        utilization = (bytes_on_path << XQC_BALANCED_SCORE_SHIFT) / cwnd;
    }

    utilization = xqc_min(utilization, XQC_BALANCED_UTIL_BOOST_CEIL);

    capacity = ((__uint128_t)avail_bytes << XQC_BALANCED_SCORE_SHIFT) / effective_srtt;
    score = (capacity * (2 * XQC_BALANCED_UTIL_SCALE - utilization))
            / XQC_BALANCED_UTIL_SCALE;

    if (score == 0 && avail_bytes > 0) {
        score = 1;
    }

    return (uint64_t)score;
}

static xqc_path_ctx_t *
xqc_balanced_scheduler_pick_guard_path(xqc_balanced_scheduler_t *scheduler,
    xqc_path_ctx_t **candidates, uint64_t *scores, size_t count)
{
    if (scheduler == NULL || count < 2) {
        return NULL;
    }

    if ((scheduler->pick_count % XQC_BALANCED_GUARD_INTERVAL)
        != (XQC_BALANCED_GUARD_INTERVAL - 1))
    {
        return NULL;
    }

    size_t min_idx = 0;
    size_t max_idx = 0;
    uint64_t min_sent = candidates[0]->path_send_ctl->ctl_app_bytes_send;
    uint64_t max_sent = min_sent;

    for (size_t i = 1; i < count; i++) {
        uint64_t sent = candidates[i]->path_send_ctl->ctl_app_bytes_send;

        if (sent < min_sent) {
            min_sent = sent;
            min_idx = i;
        }

        if (sent > max_sent) {
            max_sent = sent;
            max_idx = i;
        }
    }

    if (max_idx == min_idx || max_sent < XQC_BALANCED_SKEW_MIN_BYTES) {
        return NULL;
    }

    if (min_sent != 0 && max_sent / min_sent < XQC_BALANCED_SKEW_RATIO) {
        return NULL;
    }

    if (min_sent == 0 && max_sent < XQC_BALANCED_SKEW_MIN_BYTES * XQC_BALANCED_SKEW_RATIO) {
        return NULL;
    }

    if (scores[min_idx] == 0) {
        return NULL;
    }

    scheduler->last_path_id = candidates[min_idx]->path_id;
    scheduler->has_last_path = XQC_TRUE;
    return candidates[min_idx];
}


static xqc_path_ctx_t *
xqc_balanced_scheduler_pick_path(xqc_balanced_scheduler_t *scheduler,
    xqc_path_ctx_t **candidates, uint64_t *scores, uint64_t *srtt, size_t count,
    uint64_t *best_score)
{
    if (count == 0) {
        if (best_score) {
            *best_score = 0;
        }
        return NULL;
    }

    size_t start = 0;
    if (scheduler && scheduler->has_last_path) {
        for (size_t i = 0; i < count; i++) {
            if (candidates[i]->path_id == scheduler->last_path_id) {
                start = (i + 1) % count;
                break;
            }
        }
    }

    size_t best_idx = start;
    uint64_t selected_score = scores[start];
    uint64_t best_srtt = srtt[start];

    for (size_t offset = 0; offset < count; offset++) {
        size_t idx = (start + offset) % count;
        uint64_t candidate_score = scores[idx];
        uint64_t candidate_srtt = srtt[idx];

        if (offset == 0) {
            best_idx = idx;
            selected_score = candidate_score;
            best_srtt = candidate_srtt;
            continue;
        }

        if (candidate_score > selected_score) {
            best_idx = idx;
            selected_score = candidate_score;
            best_srtt = candidate_srtt;

        } else if (candidate_score == selected_score) {
            if (candidate_srtt < best_srtt)
            {
                best_idx = idx;
                selected_score = candidate_score;
                best_srtt = candidate_srtt;
            }
        }
    }

    if (scheduler) {
        scheduler->last_path_id = candidates[best_idx]->path_id;
        scheduler->has_last_path = XQC_TRUE;
    }

    if (best_score) {
        *best_score = selected_score;
    }

    return candidates[best_idx];
}

xqc_path_ctx_t *
xqc_balanced_scheduler_get_path(void *scheduler,
    xqc_connection_t *conn, xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_bool_t *cc_blocked)
{
    xqc_balanced_scheduler_t *balanced = (xqc_balanced_scheduler_t *)scheduler;

    xqc_path_ctx_t *available_paths[XQC_MAX_PATHS_COUNT];
    uint64_t available_scores[XQC_MAX_PATHS_COUNT];
    uint64_t available_srtt[XQC_MAX_PATHS_COUNT];
    size_t available_cnt = 0;

    xqc_path_ctx_t *standby_paths[XQC_MAX_PATHS_COUNT];
    uint64_t standby_scores[XQC_MAX_PATHS_COUNT];
    uint64_t standby_srtt[XQC_MAX_PATHS_COUNT];
    size_t standby_cnt = 0;

    xqc_path_ctx_t *original_path = NULL;

    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;
    uint64_t path_srtt = 0;
    uint64_t score = 0;
    uint64_t avail_bytes = 0;
    xqc_bool_t reached_cwnd_check = XQC_FALSE;

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

        if (!reached_cwnd_check) {
            reached_cwnd_check = XQC_TRUE;
            if (cc_blocked) {
                *cc_blocked = XQC_TRUE;
            }
        }

        if (!xqc_scheduler_check_path_can_send(path, packet_out, check_cwnd)) {
            continue;
        }

        if (cc_blocked) {
            *cc_blocked = XQC_FALSE;
        }

        if (reinject && packet_out->po_path_id == path->path_id) {
            original_path = path;
            continue;
        }

        path_srtt = xqc_balanced_scheduler_get_effective_srtt(path->path_send_ctl);

        avail_bytes = xqc_balanced_scheduler_available_bytes(path);
        score = xqc_balanced_scheduler_compute_score(path, avail_bytes, path_srtt);
        if (path->app_path_status == XQC_APP_PATH_STATUS_AVAILABLE) {
            if (available_cnt < XQC_MAX_PATHS_COUNT) {
                available_paths[available_cnt] = path;
                available_scores[available_cnt] = score;
                available_srtt[available_cnt] = path_srtt;
                available_cnt++;
            }

        } else if (path->app_path_status == XQC_APP_PATH_STATUS_STANDBY) {
            if (standby_cnt < XQC_MAX_PATHS_COUNT) {
                standby_paths[standby_cnt] = path;
                standby_scores[standby_cnt] = score;
                standby_srtt[standby_cnt] = path_srtt;
                standby_cnt++;
            }
        }

        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|balanced scheduler|conn:%p|path:%ui|status:%d|srtt:%ui|score:%ui|avail:%ui|",
                conn, path->path_id, path->app_path_status, path_srtt, score, avail_bytes);
    }

    xqc_path_ctx_t *best_path = NULL;
    uint64_t best_score = 0;

    best_path = xqc_balanced_scheduler_pick_guard_path(balanced, available_paths,
                                                       available_scores, available_cnt);

    if (best_path != NULL) {
        best_score = 0;
        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|balanced scheduler|guard pick path:%ui|pick_cnt:%ui|",
                best_path->path_id, balanced->pick_count + 1);
    }

    if (best_path == NULL) {
        best_path = xqc_balanced_scheduler_pick_path(balanced, available_paths,
                                                     available_scores, available_srtt, available_cnt,
                                                     &best_score);
    }

    if (best_path == NULL) {
        best_path = xqc_balanced_scheduler_pick_path(balanced, standby_paths,
                                                     standby_scores, standby_srtt, standby_cnt,
                                                     &best_score);
    }

    if (best_path == NULL && original_path != NULL
        && !(packet_out->po_flag & XQC_POF_REINJECT_DIFF_PATH))
    {
        best_path = original_path;
    }

    if (best_path == NULL) {
        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|balanced scheduler|No available paths to schedule|conn:%p|", conn);
        return NULL;
    }

    xqc_log(conn->log, XQC_LOG_DEBUG,
            "|balanced scheduler|best path:%ui|frame_type:%s|score:%ui|pick_cnt:%ui|",
            best_path->path_id,
            xqc_frame_type_2_str(conn->engine, packet_out->po_frame_types),
            best_score, ++balanced->pick_count);

    return best_path;
}

const xqc_scheduler_callback_t xqc_balanced_scheduler_cb = {
    .xqc_scheduler_size             = xqc_balanced_scheduler_size,
    .xqc_scheduler_init             = xqc_balanced_scheduler_init,
    .xqc_scheduler_get_path         = xqc_balanced_scheduler_get_path,
};