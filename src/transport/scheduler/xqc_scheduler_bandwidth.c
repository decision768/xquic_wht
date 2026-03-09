#include "src/transport/scheduler/xqc_scheduler_bandwidth.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_send_ctl.h"

#define XQC_BW_FILL_RATIO_NUM  9
#define XQC_BW_FILL_RATIO_DEN  10

//以下为另一个修改
#define XQC_BW_CAP_SHIFT       10
#define XQC_BW_BETA_NUM        13  /* 1.3x total budget */
#define XQC_BW_BETA_DEN        10
//以上为另一个修改

typedef struct {
    xqc_bool_t  has_last_path;
    uint64_t    last_path_id;
} xqc_bandwidth_scheduler_t;


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
        s->has_last_path = XQC_FALSE;
        s->last_path_id = 0;
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

static inline void
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
xqc_bandwidth_scheduler_compute_scores(xqc_path_ctx_t **paths,
    uint64_t *cwnd, uint64_t *inflight, uint64_t *srtt, int64_t *score, size_t count)
{
    if (count == 0) {
        return;
    }

    __uint128_t sum_cwnd = 0;
    __uint128_t sum_cap = 0;

    /* cap_i = (cwnd_i << SHIFT) / srtt_i */
    for (size_t i = 0; i < count; i++) {
        uint64_t rtt = srtt[i] ? srtt[i] : 1;
        sum_cwnd += cwnd[i];
        sum_cap += (((__uint128_t)cwnd[i]) << XQC_BW_CAP_SHIFT) / rtt;
    }

    if (sum_cap == 0) {
        /* fallback: equal cap */
        sum_cap = count;
    }

    /* total_budget = beta * fill_ratio * sum(cwnd) */
    __uint128_t total_budget = sum_cwnd;
    total_budget = (total_budget * XQC_BW_FILL_RATIO_NUM) / XQC_BW_FILL_RATIO_DEN;
    total_budget = (total_budget * XQC_BW_BETA_NUM) / XQC_BW_BETA_DEN;

    for (size_t i = 0; i < count; i++) {
        uint64_t rtt = srtt[i] ? srtt[i] : 1;
        __uint128_t cap_i = (((__uint128_t)cwnd[i]) << XQC_BW_CAP_SHIFT) / rtt;
        if (cap_i == 0) {
            cap_i = 1;
        }

        /* target_i = total_budget * cap_i / sum_cap */
        __uint128_t target_i = (total_budget * cap_i) / sum_cap;

        int64_t deficit = (int64_t)target_i - (int64_t)inflight[i];
        if (deficit <= 0) {
            deficit = 1;
        }

        score[i] = deficit;
    }
}


static xqc_path_ctx_t *
xqc_bandwidth_scheduler_pick_path(xqc_bandwidth_scheduler_t *scheduler,
    xqc_path_ctx_t **candidates, uint64_t *available_bytes, uint64_t *srtt,int64_t  *score, size_t count,
    uint64_t *best_available,int64_t *best_score)
{
    if (count == 0) {
        if (best_available) {
            *best_available = 0;
        }
        if (best_score) {
            *best_score = 0;
        }
        return NULL;
    }
    //改
    int64_t max_score = score[0];
    for (size_t i = 1; i < count; i++) {
        if (score[i] > max_score) {
            max_score = score[i];
        }
    }

    if (max_score == 0) {
        size_t best_idx = 0;
        uint64_t best_bytes = available_bytes[0];
        uint64_t best_srtt = srtt[0];

        for (size_t i = 1; i < count; i++) {
            uint64_t b = available_bytes[i];
            uint64_t r = srtt[i];

            if (b > best_bytes
                || (b == best_bytes && r < best_srtt))
            {
                best_idx = i;
                best_bytes = b;
                best_srtt = r;
            }
        }

        if (scheduler) {
            scheduler->last_path_id = candidates[best_idx]->path_id;
            scheduler->has_last_path = XQC_TRUE;
        }

        if (best_available) {
            *best_available = best_bytes;
        }
        if (best_score) {
            *best_score = 0;
        }
        return candidates[best_idx];
    }
    //改

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
    uint64_t selected_bytes = available_bytes[start];
    uint64_t best_srtt = srtt[start];
    int64_t  selected_score = score[start];

    for (size_t offset = 0; offset < count; offset++) {
        size_t idx = (start + offset) % count;
        uint64_t candidate_bytes = available_bytes[idx];
        uint64_t candidate_srtt = srtt[idx];
        int64_t  candidate_score = score[idx];

        if (offset == 0) {
            best_idx = idx;
            selected_bytes = candidate_bytes;
            best_srtt = candidate_srtt;
            selected_score = candidate_score;
            continue;
        }

        /*
         * Aggregation-oriented selection:
         *   - Prefer the path with larger "deficit" score (i.e. more need to be filled).
         *   - Tie-break by smaller RTT, then smaller path_id.
         */
        if (candidate_score > selected_score) {
            best_idx = idx;
            selected_score = candidate_score;
            selected_bytes = candidate_bytes;
            best_srtt = candidate_srtt;

        } else if (candidate_score == selected_score) {
            if (candidate_bytes > selected_bytes
                || (candidate_bytes == selected_bytes
                    && candidate_srtt < best_srtt))
            {
                best_idx = idx;
                selected_score = candidate_score;
                selected_bytes = candidate_bytes;
                best_srtt = candidate_srtt;
            }
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
    int64_t  available_score[XQC_MAX_PATHS_COUNT];
    //改
    uint64_t available_cwnd[XQC_MAX_PATHS_COUNT];
    uint64_t available_inflight[XQC_MAX_PATHS_COUNT];
    uint64_t standby_cwnd[XQC_MAX_PATHS_COUNT];
    uint64_t standby_inflight[XQC_MAX_PATHS_COUNT];
    //改

    size_t available_cnt = 0;

    xqc_path_ctx_t *standby_paths[XQC_MAX_PATHS_COUNT];
    uint64_t standby_bytes[XQC_MAX_PATHS_COUNT];
    uint64_t standby_srtt[XQC_MAX_PATHS_COUNT];
    int64_t  standby_score[XQC_MAX_PATHS_COUNT];
    size_t standby_cnt = 0;

    xqc_path_ctx_t *original_path = NULL;

    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;
    uint64_t path_srtt = 0;
    uint64_t avail_bytes = 0;
    uint64_t cwnd = 0;
    uint64_t bytes_on_path = 0;
    //int64_t  score = 0;
    xqc_bool_t has_active_path = XQC_FALSE;
    xqc_bool_t has_sendable_path = XQC_FALSE;

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

        if (!xqc_scheduler_check_path_can_send(path, packet_out, check_cwnd)) {
            continue;
        }

        has_sendable_path = XQC_TRUE;

        if (reinject && packet_out->po_path_id == path->path_id) {
            original_path = path;
            continue;
        }

        /* compute stats and aggregation score */
        xqc_bandwidth_scheduler_get_path_stats(path, &cwnd, &bytes_on_path, &path_srtt);
        avail_bytes = xqc_bandwidth_scheduler_available_bytes(path);

        /*
         * "Fill" deficit score:
         *   target_inflight = cwnd * ratio
         *   need = target_inflight - bytes_on_path
         *   score = need / srtt
         */
        //去掉原来的score算法
        // uint64_t target = (cwnd * XQC_BW_FILL_RATIO_NUM) / XQC_BW_FILL_RATIO_DEN;
        // int64_t need = (int64_t)target - (int64_t)bytes_on_path;
        // if (need <= 0) {
        //     need = 1; /* still eligible, but low priority */
        // }
        // score = (need << 10) / (int64_t)path_srtt;
        //去掉原来的score算法

        if (path->app_path_status == XQC_APP_PATH_STATUS_AVAILABLE) {
            if (available_cnt < XQC_MAX_PATHS_COUNT) {
                available_paths[available_cnt] = path;
                available_bytes[available_cnt] = avail_bytes;
                available_srtt[available_cnt] = path_srtt;
                available_cwnd[available_cnt] = cwnd;
                available_inflight[available_cnt] = bytes_on_path;
                //available_score[available_cnt] = score;
                available_cnt++;
            }

        } else if (path->app_path_status == XQC_APP_PATH_STATUS_STANDBY) {
            if (standby_cnt < XQC_MAX_PATHS_COUNT) {
                standby_paths[standby_cnt] = path;
                standby_bytes[standby_cnt] = avail_bytes;
                standby_srtt[standby_cnt] = path_srtt;
                standby_cwnd[standby_cnt] = cwnd;
                standby_inflight[standby_cnt] = bytes_on_path;
                //standby_score[standby_cnt] = score;
                standby_cnt++;
            }
        }

        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|bandwidth scheduler|conn:%p|path:%ui|status:%d|srtt:%ui|avail:%ui|",
                conn, path->path_id, path->app_path_status, path_srtt, avail_bytes);
    }

    if (cc_blocked) {
        /* blocked iff there is at least one active path, but none is sendable */
        *cc_blocked = (has_active_path && !has_sendable_path) ? XQC_TRUE : XQC_FALSE;
    }

    //改算法
    xqc_bandwidth_scheduler_compute_scores(available_paths,
        available_cwnd, available_inflight, available_srtt, available_score, available_cnt);
    xqc_bandwidth_scheduler_compute_scores(standby_paths,
        standby_cwnd, standby_inflight, standby_srtt, standby_score, standby_cnt);
    //改算法

    xqc_path_ctx_t *best_path = NULL;
    uint64_t best_available = 0;
    int64_t best_score = 0;

    best_path = xqc_bandwidth_scheduler_pick_path(bandwidth, available_paths,
                                                  available_bytes, available_srtt,
                                                  available_score,
                                                  available_cnt, &best_available,
                                                  &best_score);

    if (best_path == NULL) {
        best_path = xqc_bandwidth_scheduler_pick_path(bandwidth, standby_paths,
                                                      standby_bytes, standby_srtt,
                                                      standby_score,
                                                      standby_cnt, &best_available,
                                                      &best_score);
    }

    if (best_path == NULL && original_path != NULL
        && !(packet_out->po_flag & XQC_POF_REINJECT_DIFF_PATH))
    {
        best_path = original_path;
    }

    if (best_path == NULL) {
        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|bandwidth scheduler|No available paths to schedule|conn:%p|", conn);
        return NULL;
    }

    xqc_log(conn->log, XQC_LOG_DEBUG,
            "|bandwidth scheduler|best path:%ui|frame_type:%s|available:%ui|score:%ld|",
            best_path->path_id,
            xqc_frame_type_2_str(conn->engine, packet_out->po_frame_types),
            best_available, (long)best_score);

    return best_path;
}

const xqc_scheduler_callback_t xqc_bandwidth_scheduler_cb = {
    .xqc_scheduler_size             = xqc_bandwidth_scheduler_size,
    .xqc_scheduler_init             = xqc_bandwidth_scheduler_init,
    .xqc_scheduler_get_path         = xqc_bandwidth_scheduler_get_path,
};