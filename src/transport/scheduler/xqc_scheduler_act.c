#include "src/transport/scheduler/xqc_scheduler_act.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_multipath.h"

#include <string.h>

#define XQC_ACT_SMOOTHING_ALPHA 0.2
#define XQC_ACT_MIN_CAPACITY    1024.0
#define XQC_ACT_MAX_DEFICIT     4.0

typedef struct {
    xqc_bool_t  in_use;
    uint64_t    path_id;
    double      cap_est;
    double      weight;
    double      deficit;
} xqc_act_path_state_t;

typedef struct {
    xqc_path_ctx_t           *path;
    xqc_act_path_state_t     *state;
    uint64_t                  srtt;
    double                    normalized_cap;
} xqc_act_candidate_t;

typedef struct {
    xqc_act_path_state_t  states[XQC_MAX_PATHS_COUNT];
    double                alpha;
    double                min_capacity;
    double                max_deficit;
} xqc_act_scheduler_t;

static size_t
xqc_act_scheduler_size()
{
    return sizeof(xqc_act_scheduler_t);
}

static void
xqc_act_scheduler_init(void *scheduler, xqc_log_t *log, xqc_scheduler_params_t *params)
{
    xqc_act_scheduler_t *act = (xqc_act_scheduler_t *)scheduler;
    memset(act, 0, sizeof(*act));
    act->alpha = XQC_ACT_SMOOTHING_ALPHA;
    act->min_capacity = XQC_ACT_MIN_CAPACITY;
    act->max_deficit = XQC_ACT_MAX_DEFICIT;
    (void)log;
    (void)params;
}

static xqc_act_path_state_t *
xqc_act_scheduler_get_state(xqc_act_scheduler_t *act, uint64_t path_id)
{
    xqc_act_path_state_t *free_slot = NULL;

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        xqc_act_path_state_t *state = &act->states[i];
        if (state->in_use && state->path_id == path_id) {
            return state;
        }

        if (!state->in_use && free_slot == NULL) {
            free_slot = state;
        }
    }

    if (free_slot) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->in_use = XQC_TRUE;
        free_slot->path_id = path_id;
        free_slot->cap_est = 0;
        free_slot->weight = 0;
        free_slot->deficit = 0;
        return free_slot;
    }

    return NULL;
}

static uint64_t
xqc_act_scheduler_available_bytes(xqc_path_ctx_t *path)
{
    xqc_send_ctl_t *ctl = path->path_send_ctl;
    if (ctl == NULL || ctl->ctl_cong_callback == NULL
        || ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd == NULL)
    {
        return 0;
    }

    uint64_t bytes_on_path = path->path_schedule_bytes + ctl->ctl_bytes_in_flight;
    uint64_t cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(ctl->ctl_cong);

    if (cwnd <= bytes_on_path) {
        return 0;
    }

    return cwnd - bytes_on_path;
}

static double
xqc_act_scheduler_sample_capacity(xqc_path_ctx_t *path, uint64_t srtt)
{
    xqc_send_ctl_t *ctl = path->path_send_ctl;
    if (ctl == NULL) {
        return 0;
    }

    double sample = 0;
    if (ctl->sampler.delivery_rate > 0) {
        sample = (double)ctl->sampler.delivery_rate;
    }

    if (sample <= 0 && ctl->ctl_cong_callback
        && ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd)
    {
        uint64_t cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(ctl->ctl_cong);
        if (srtt == 0) {
            srtt = 1;
        }
        if (cwnd > 0) {
            sample = ((double)cwnd * 1000000.0) / (double)srtt;
        }
    }

    return sample;
}

static void
xqc_act_scheduler_refresh_state(xqc_act_scheduler_t *act,
    xqc_act_path_state_t *state, double sample)
{
    if (state == NULL || sample <= 0) {
        return;
    }

    if (state->cap_est <= 0) {
        state->cap_est = sample;
        return;
    }

    state->cap_est = (1.0 - act->alpha) * state->cap_est + act->alpha * sample;
}

static void
xqc_act_scheduler_apply_weights(xqc_act_scheduler_t *act,
    xqc_act_candidate_t *candidates, size_t count)
{
    if (count == 0) {
        return;
    }

    double total = 0;
    for (size_t i = 0; i < count; i++) {
        double cap = candidates[i].state->cap_est;
        if (cap < act->min_capacity) {
            cap = act->min_capacity;
        }
        candidates[i].normalized_cap = cap;
        total += cap;
    }

    if (total <= 0) {
        total = act->min_capacity * (double)count;
    }

    for (size_t i = 0; i < count; i++) {
        xqc_act_path_state_t *state = candidates[i].state;
        double weight = candidates[i].normalized_cap / total;
        state->weight = weight;
        state->deficit += weight;
        if (state->deficit > act->max_deficit) {
            state->deficit = act->max_deficit;
        }
    }
}

static xqc_path_ctx_t *
xqc_act_scheduler_pick_path(xqc_act_scheduler_t *act,
    xqc_act_candidate_t *candidates, size_t count,
    xqc_packet_out_t *packet_out, xqc_bool_t reinject,
    xqc_path_ctx_t *original_path, uint64_t *best_score)
{
    xqc_path_ctx_t *best_path = NULL;
    xqc_act_path_state_t *best_state = NULL;
    double best_deficit = -1;
    uint64_t best_srtt = 0;
    xqc_bool_t has_fast_lane = XQC_FALSE;
    uint64_t min_srtt = 0;

    if (packet_out && (packet_out->po_frame_types & (XQC_FRAME_BIT_STREAM | XQC_FRAME_BIT_DATAGRAM))) {
        for (size_t i = 0; i < count; i++) {
            if (reinject && original_path && candidates[i].path->path_id == original_path->path_id) {
                continue;
            }
            uint64_t srtt = candidates[i].srtt ? candidates[i].srtt : 1;
            if (min_srtt == 0 || srtt < min_srtt) {
                min_srtt = srtt;
            }
        }

        if (min_srtt > 0) {
            for (size_t i = 0; i < count; i++) {
                if (reinject && original_path && candidates[i].path->path_id == original_path->path_id) {
                    continue;
                }
                uint64_t srtt = candidates[i].srtt ? candidates[i].srtt : 1;
                if (!(srtt > min_srtt && (srtt > (min_srtt << 1) || srtt - min_srtt > 20000))) {
                    has_fast_lane = XQC_TRUE;
                    break;
                }
            }
        }
    }


    for (size_t i = 0; i < count; i++) {
        xqc_path_ctx_t *path = candidates[i].path;
        xqc_act_path_state_t *state = candidates[i].state;

        if (reinject && original_path && path->path_id == original_path->path_id) {
            continue;
        }
        if (has_fast_lane && min_srtt > 0) {
            uint64_t srtt = candidates[i].srtt ? candidates[i].srtt : 1;
            if (srtt > min_srtt && (srtt > (min_srtt << 1) || srtt - min_srtt > 20000)) {
                continue;
            }
        }


        double deficit = state->deficit;
        if (!best_path || deficit > best_deficit + 1e-6
            || (deficit >= best_deficit - 1e-6 && candidates[i].srtt < best_srtt))
        {
            best_path = path;
            best_state = state;
            best_deficit = deficit;
            best_srtt = candidates[i].srtt;
        }
    }

    if (best_path && best_state) {
        best_state->deficit -= 1.0;
        if (best_state->deficit < 0) {
            best_state->deficit = 0;
        }
        if (best_score) {
            *best_score = (uint64_t)(best_deficit * 1000);
        }
    }

    return best_path;
}

xqc_path_ctx_t *
xqc_act_scheduler_get_path(void *scheduler, xqc_connection_t *conn,
    xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_bool_t *cc_blocked)
{
    xqc_act_scheduler_t *act = (xqc_act_scheduler_t *)scheduler;

    xqc_act_candidate_t available[XQC_MAX_PATHS_COUNT];
    xqc_act_candidate_t standby[XQC_MAX_PATHS_COUNT];
    size_t available_cnt = 0;
    size_t standby_cnt = 0;

    xqc_path_ctx_t *original_path = NULL;
    xqc_bool_t reached_cwnd_check = XQC_FALSE;

    if (cc_blocked) {
        *cc_blocked = XQC_FALSE;
    }

    xqc_list_head_t *pos, *next;
    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        xqc_path_ctx_t *path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);

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

        if (reinject && packet_out && packet_out->po_path_id == path->path_id) {
            original_path = path;
            continue;
        }

        xqc_act_path_state_t *state = xqc_act_scheduler_get_state(act, path->path_id);
        if (state == NULL) {
            continue;
        }

        uint64_t path_srtt = xqc_send_ctl_get_srtt(path->path_send_ctl);
        if (path_srtt == 0) {
            path_srtt = 1;
        }

        double sample = xqc_act_scheduler_sample_capacity(path, path_srtt);
        xqc_act_scheduler_refresh_state(act, state, sample);

        uint64_t avail_bytes = xqc_act_scheduler_available_bytes(path);
        if (avail_bytes == 0 && state->cap_est > act->min_capacity) {
            state->cap_est *= 0.5;
        }

        xqc_act_candidate_t candidate = {0};
        candidate.path = path;
        candidate.state = state;
        candidate.srtt = path_srtt;
        candidate.normalized_cap = state->cap_est;

        if (path->app_path_status == XQC_APP_PATH_STATUS_AVAILABLE) {
            if (available_cnt < XQC_MAX_PATHS_COUNT) {
                available[available_cnt++] = candidate;
            }
        } else if (path->app_path_status == XQC_APP_PATH_STATUS_STANDBY) {
            if (standby_cnt < XQC_MAX_PATHS_COUNT) {
                standby[standby_cnt++] = candidate;
            }
        }

        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|act scheduler|conn:%p|path:%ui|status:%d|srtt:%ui|cap:%f|avail:%ui|",
                conn, path->path_id, path->app_path_status, path_srtt,
                state->cap_est, avail_bytes);
    }

    xqc_act_scheduler_apply_weights(act, available, available_cnt);
    xqc_act_scheduler_apply_weights(act, standby, standby_cnt);

    xqc_path_ctx_t *best_path = NULL;
    uint64_t best_score = 0;

    best_path = xqc_act_scheduler_pick_path(act, available, available_cnt,
                                            packet_out, reinject, original_path, &best_score);

    if (best_path == NULL) {
        best_path = xqc_act_scheduler_pick_path(act, standby, standby_cnt,
                                                packet_out, reinject, original_path, &best_score);
    }

    if (best_path == NULL && original_path != NULL
        && packet_out && !(packet_out->po_flag & XQC_POF_REINJECT_DIFF_PATH))
    {
        best_path = original_path;
    }

    if (best_path == NULL) {
        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|act scheduler|No available paths|conn:%p|", conn);
        return NULL;
    }

    xqc_log(conn->log, XQC_LOG_DEBUG,
            "|act scheduler|best path:%ui|frame_type:%s|score:%ui|",
            best_path->path_id,
            packet_out ? xqc_frame_type_2_str(conn->engine, packet_out->po_frame_types) : "unknown",
            best_score);

    return best_path;
}

const xqc_scheduler_callback_t xqc_act_scheduler_cb = {
    .xqc_scheduler_size             = xqc_act_scheduler_size,
    .xqc_scheduler_init             = xqc_act_scheduler_init,
    .xqc_scheduler_get_path         = xqc_act_scheduler_get_path,
};