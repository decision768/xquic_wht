#include "src/transport/scheduler/xqc_scheduler_fastprobe.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_send_ctl.h"

#include <string.h>

#define XQC_FASTPROBE_CLOSE_RATIO_PCT       80.0
#define XQC_FASTPROBE_MID_RATIO_PCT         50.0
#define XQC_FASTPROBE_BAD_RATIO_PCT         35.0
#define XQC_FASTPROBE_EMA_ALPHA             0.20
#define XQC_FASTPROBE_MIN_CAPACITY_BPS      1024.0
#define XQC_FASTPROBE_CREDIT_CAP            4.0
#define XQC_FASTPROBE_DEFAULT_PROBE_PCT     10.0

typedef struct {
    xqc_bool_t  in_use;
    uint64_t    path_id;
    double      cap_ema;
    double      credit;
} xqc_fastprobe_path_state_t;

typedef struct {
    xqc_path_ctx_t                *path;
    xqc_fastprobe_path_state_t    *state;
    uint64_t                       srtt;
    uint64_t                       cwnd;
    uint64_t                       bytes_on_path;
    uint64_t                       avail_bytes;
    double                         cap;
    double                         target_pct;
} xqc_fastprobe_candidate_t;

typedef struct {
    xqc_fastprobe_path_state_t     states[XQC_MAX_PATHS_COUNT];
    double                         max_probe_pct;
    uint64_t                       pick_count;
    const char                    *profile;
} xqc_fastprobe_scheduler_t;

static size_t
xqc_fastprobe_scheduler_size()
{
    return sizeof(xqc_fastprobe_scheduler_t);
}

static void
xqc_fastprobe_scheduler_init_profile(void *scheduler, double max_probe_pct,
    const char *profile)
{
    xqc_fastprobe_scheduler_t *fp = (xqc_fastprobe_scheduler_t *)scheduler;

    if (fp == NULL) {
        return;
    }

    memset(fp, 0, sizeof(*fp));
    fp->max_probe_pct = max_probe_pct;
    fp->profile = profile;
}

static void
xqc_fastprobe_scheduler_init(void *scheduler, xqc_log_t *log,
    xqc_scheduler_params_t *params)
{
    (void)log;
    (void)params;
    xqc_fastprobe_scheduler_init_profile(scheduler,
        XQC_FASTPROBE_DEFAULT_PROBE_PCT, "fastprobe10");
}

static void
xqc_fastprobe5_scheduler_init(void *scheduler, xqc_log_t *log,
    xqc_scheduler_params_t *params)
{
    (void)log;
    (void)params;
    xqc_fastprobe_scheduler_init_profile(scheduler, 5.0, "fastprobe5");
}

static void
xqc_fastprobe10_scheduler_init(void *scheduler, xqc_log_t *log,
    xqc_scheduler_params_t *params)
{
    (void)log;
    (void)params;
    xqc_fastprobe_scheduler_init_profile(scheduler, 10.0, "fastprobe10");
}

static void
xqc_fastprobe20_scheduler_init(void *scheduler, xqc_log_t *log,
    xqc_scheduler_params_t *params)
{
    (void)log;
    (void)params;
    xqc_fastprobe_scheduler_init_profile(scheduler, 20.0, "fastprobe20");
}

static void
xqc_fastonly_scheduler_init(void *scheduler, xqc_log_t *log,
    xqc_scheduler_params_t *params)
{
    (void)log;
    (void)params;
    xqc_fastprobe_scheduler_init_profile(scheduler, 0.0, "fastonly");
}

static xqc_fastprobe_path_state_t *
xqc_fastprobe_get_state(xqc_fastprobe_scheduler_t *fp, uint64_t path_id)
{
    xqc_fastprobe_path_state_t *free_slot = NULL;

    if (fp == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        xqc_fastprobe_path_state_t *state = &fp->states[i];

        if (state->in_use && state->path_id == path_id) {
            return state;
        }

        if (!state->in_use && free_slot == NULL) {
            free_slot = state;
        }
    }

    if (free_slot == NULL) {
        return NULL;
    }

    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->in_use = XQC_TRUE;
    free_slot->path_id = path_id;

    return free_slot;
}

static uint64_t
xqc_fastprobe_available_bytes(xqc_path_ctx_t *path)
{
    xqc_send_ctl_t *ctl = path->path_send_ctl;
    uint64_t bytes_on_path;
    uint64_t cwnd;

    if (ctl == NULL || ctl->ctl_cong_callback == NULL
        || ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd == NULL)
    {
        return 0;
    }

    bytes_on_path = path->path_schedule_bytes + ctl->ctl_bytes_in_flight;
    cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(ctl->ctl_cong);

    if (cwnd <= bytes_on_path) {
        return 0;
    }

    return cwnd - bytes_on_path;
}

static void
xqc_fastprobe_get_path_stats(xqc_path_ctx_t *path, uint64_t *cwnd,
    uint64_t *bytes_on_path, uint64_t *srtt)
{
    xqc_send_ctl_t *ctl = path->path_send_ctl;

    *cwnd = 0;
    *bytes_on_path = 0;
    *srtt = 1;

    if (ctl == NULL) {
        return;
    }

    *bytes_on_path = path->path_schedule_bytes + ctl->ctl_bytes_in_flight;
    *srtt = xqc_send_ctl_get_srtt(ctl);
    if (*srtt == 0) {
        *srtt = ctl->ctl_conn ? ctl->ctl_conn->conn_settings.initial_rtt : 1;
    }
    if (*srtt == 0) {
        *srtt = 1;
    }

    if (ctl->ctl_cong_callback
        && ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd)
    {
        *cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(ctl->ctl_cong);
    }
}

static double
xqc_fastprobe_sample_capacity(xqc_path_ctx_t *path, uint64_t cwnd,
    uint64_t srtt)
{
    xqc_send_ctl_t *ctl = path->path_send_ctl;
    uint64_t est_bw = 0;
    double sample = 0;
    double loss_rate = 0;

    if (ctl != NULL) {
        est_bw = xqc_send_ctl_get_est_bw(ctl);
    }

    if (est_bw > 0) {
        sample = (double)est_bw;

    } else if (cwnd > 0) {
        if (srtt == 0) {
            srtt = 1;
        }
        sample = ((double)cwnd * 1000000.0) / (double)srtt;
    }

    if (sample < XQC_FASTPROBE_MIN_CAPACITY_BPS) {
        sample = XQC_FASTPROBE_MIN_CAPACITY_BPS;
    }

    loss_rate = xqc_path_recent_loss_rate(path);
    if (loss_rate > 0) {
        sample = sample * 100.0 / (100.0 + 4.0 * loss_rate);
    }

    return sample;
}

static double
xqc_fastprobe_refresh_capacity(xqc_fastprobe_path_state_t *state,
    double sample)
{
    if (state == NULL || sample <= 0) {
        return sample;
    }

    if (state->cap_ema <= 0) {
        state->cap_ema = sample;

    } else {
        state->cap_ema = (1.0 - XQC_FASTPROBE_EMA_ALPHA) * state->cap_ema
                         + XQC_FASTPROBE_EMA_ALPHA * sample;
    }

    return state->cap_ema;
}

static void
xqc_fastprobe_compute_targets(xqc_fastprobe_scheduler_t *fp,
    xqc_fastprobe_candidate_t *candidates, size_t count)
{
    size_t best_idx = 0;
    double best_cap = 0;
    double sum_cap = 0;
    double other_total = 0;

    if (count == 0) {
        return;
    }

    if (count == 1) {
        candidates[0].target_pct = 100.0;
        return;
    }

    for (size_t i = 0; i < count; i++) {
        double cap = candidates[i].cap;
        if (cap < XQC_FASTPROBE_MIN_CAPACITY_BPS) {
            cap = XQC_FASTPROBE_MIN_CAPACITY_BPS;
        }

        sum_cap += cap;
        if (cap > best_cap) {
            best_cap = cap;
            best_idx = i;
        }
    }

    if (sum_cap <= 0 || best_cap <= 0) {
        for (size_t i = 0; i < count; i++) {
            candidates[i].target_pct = 100.0 / (double)count;
        }
        return;
    }

    for (size_t i = 0; i < count; i++) {
        double natural = candidates[i].cap * 100.0 / sum_cap;
        double ratio = candidates[i].cap * 100.0 / best_cap;
        double target = natural;

        if (i == best_idx) {
            candidates[i].target_pct = 0;
            continue;
        }

        if (fp->max_probe_pct <= 0) {
            target = 0;

        } else if (ratio >= XQC_FASTPROBE_CLOSE_RATIO_PCT) {
            target = natural;

        } else if (ratio >= XQC_FASTPROBE_MID_RATIO_PCT) {
            target = xqc_min(natural, fp->max_probe_pct * 2.0);

        } else if (ratio >= XQC_FASTPROBE_BAD_RATIO_PCT) {
            target = xqc_min(natural, fp->max_probe_pct);

        } else {
            target = xqc_min(natural, fp->max_probe_pct * 0.5);
        }

        if (target < 0) {
            target = 0;
        }
        candidates[i].target_pct = target;
        other_total += target;
    }

    if (other_total > 95.0) {
        double scale = 95.0 / other_total;
        other_total = 0;
        for (size_t i = 0; i < count; i++) {
            if (i == best_idx) {
                continue;
            }
            candidates[i].target_pct *= scale;
            other_total += candidates[i].target_pct;
        }
    }

    candidates[best_idx].target_pct = 100.0 - other_total;
}

static xqc_path_ctx_t *
xqc_fastprobe_pick_by_credit(xqc_fastprobe_scheduler_t *fp,
    xqc_fastprobe_candidate_t *candidates, size_t count,
    uint64_t *best_score, double *best_target)
{
    size_t best_idx = 0;
    double best_credit = -1.0;
    double best_cap = -1.0;

    if (count == 0) {
        if (best_score) {
            *best_score = 0;
        }
        if (best_target) {
            *best_target = 0;
        }
        return NULL;
    }

    xqc_fastprobe_compute_targets(fp, candidates, count);

    for (size_t i = 0; i < count; i++) {
        xqc_fastprobe_path_state_t *state = candidates[i].state;

        if (state == NULL) {
            continue;
        }

        if (candidates[i].target_pct <= 0) {
            state->credit = 0;

        } else {
            state->credit += candidates[i].target_pct / 100.0;
            if (state->credit > XQC_FASTPROBE_CREDIT_CAP) {
                state->credit = XQC_FASTPROBE_CREDIT_CAP;
            }
        }

        if (state->credit > best_credit + 1e-9
            || (state->credit >= best_credit - 1e-9
                && candidates[i].cap > best_cap)
            || (state->credit >= best_credit - 1e-9
                && candidates[i].cap >= best_cap - 1e-9
                && candidates[i].srtt < candidates[best_idx].srtt))
        {
            best_idx = i;
            best_credit = state->credit;
            best_cap = candidates[i].cap;
        }
    }

    if (candidates[best_idx].state) {
        candidates[best_idx].state->credit -= 1.0;
        if (candidates[best_idx].state->credit < 0) {
            candidates[best_idx].state->credit = 0;
        }
    }

    if (best_score) {
        *best_score = (uint64_t)(best_credit > 0 ? best_credit * 1000.0 : 0);
    }
    if (best_target) {
        *best_target = candidates[best_idx].target_pct;
    }

    return candidates[best_idx].path;
}

static xqc_path_ctx_t *
xqc_fastprobe_pick_group(xqc_fastprobe_scheduler_t *fp,
    xqc_fastprobe_candidate_t *candidates, size_t count,
    uint64_t *best_score, double *best_target)
{
    if (count == 0) {
        return NULL;
    }

    return xqc_fastprobe_pick_by_credit(fp, candidates, count,
        best_score, best_target);
}

xqc_path_ctx_t *
xqc_fastprobe_scheduler_get_path(void *scheduler, xqc_connection_t *conn,
    xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_bool_t *cc_blocked)
{
    xqc_fastprobe_scheduler_t *fp = (xqc_fastprobe_scheduler_t *)scheduler;
    xqc_fastprobe_candidate_t available[XQC_MAX_PATHS_COUNT];
    xqc_fastprobe_candidate_t standby[XQC_MAX_PATHS_COUNT];
    size_t available_cnt = 0;
    size_t standby_cnt = 0;
    xqc_path_ctx_t *original_path = NULL;
    xqc_path_ctx_t *best_path = NULL;
    uint64_t best_score = 0;
    double best_target = 0;
    xqc_bool_t has_active_path = XQC_FALSE;
    xqc_bool_t has_sendable_path = XQC_FALSE;
    xqc_list_head_t *pos, *next;

    if (cc_blocked) {
        *cc_blocked = XQC_FALSE;
    }

    if (fp == NULL) {
        return NULL;
    }

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        xqc_path_ctx_t *path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        xqc_fastprobe_candidate_t candidate;
        xqc_fastprobe_path_state_t *state;
        double sample;

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

        if (reinject && packet_out && packet_out->po_path_id == path->path_id) {
            original_path = path;
            continue;
        }

        state = xqc_fastprobe_get_state(fp, path->path_id);
        if (state == NULL) {
            continue;
        }

        memset(&candidate, 0, sizeof(candidate));
        candidate.path = path;
        candidate.state = state;
        xqc_fastprobe_get_path_stats(path, &candidate.cwnd,
            &candidate.bytes_on_path, &candidate.srtt);
        candidate.avail_bytes = xqc_fastprobe_available_bytes(path);
        sample = xqc_fastprobe_sample_capacity(path, candidate.cwnd,
            candidate.srtt);
        candidate.cap = xqc_fastprobe_refresh_capacity(state, sample);

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
                "|fastprobe scheduler|profile:%s|conn:%p|path:%ui|status:%d|"
                "srtt:%ui|cwnd:%ui|inflight:%ui|avail:%ui|cap:%f|credit:%f|",
                fp->profile, conn, path->path_id, path->app_path_status,
                candidate.srtt, candidate.cwnd, candidate.bytes_on_path,
                candidate.avail_bytes, candidate.cap, state->credit);
    }

    if (cc_blocked) {
        *cc_blocked = (has_active_path && !has_sendable_path) ? XQC_TRUE : XQC_FALSE;
    }

    best_path = xqc_fastprobe_pick_group(fp, available, available_cnt,
                                         &best_score, &best_target);

    if (best_path == NULL) {
        best_path = xqc_fastprobe_pick_group(fp, standby, standby_cnt,
                                             &best_score, &best_target);
    }

    if (best_path == NULL && original_path != NULL
        && packet_out && !(packet_out->po_flag & XQC_POF_REINJECT_DIFF_PATH))
    {
        best_path = original_path;
    }

    if (best_path == NULL) {
        xqc_log(conn->log, XQC_LOG_DEBUG,
                "|fastprobe scheduler|No available paths|conn:%p|", conn);
        return NULL;
    }

    fp->pick_count++;
    xqc_log(conn->log, XQC_LOG_DEBUG,
            "|fastprobe scheduler|profile:%s|best path:%ui|frame_type:%s|"
            "target_pct:%f|score:%ui|pick_cnt:%ui|",
            fp->profile, best_path->path_id,
            packet_out ? xqc_frame_type_2_str(conn->engine, packet_out->po_frame_types) : "unknown",
            best_target, best_score, fp->pick_count);

    return best_path;
}

const xqc_scheduler_callback_t xqc_fastprobe_scheduler_cb = {
    .xqc_scheduler_size             = xqc_fastprobe_scheduler_size,
    .xqc_scheduler_init             = xqc_fastprobe_scheduler_init,
    .xqc_scheduler_get_path         = xqc_fastprobe_scheduler_get_path,
};

const xqc_scheduler_callback_t xqc_fastprobe5_scheduler_cb = {
    .xqc_scheduler_size             = xqc_fastprobe_scheduler_size,
    .xqc_scheduler_init             = xqc_fastprobe5_scheduler_init,
    .xqc_scheduler_get_path         = xqc_fastprobe_scheduler_get_path,
};

const xqc_scheduler_callback_t xqc_fastprobe10_scheduler_cb = {
    .xqc_scheduler_size             = xqc_fastprobe_scheduler_size,
    .xqc_scheduler_init             = xqc_fastprobe10_scheduler_init,
    .xqc_scheduler_get_path         = xqc_fastprobe_scheduler_get_path,
};

const xqc_scheduler_callback_t xqc_fastprobe20_scheduler_cb = {
    .xqc_scheduler_size             = xqc_fastprobe_scheduler_size,
    .xqc_scheduler_init             = xqc_fastprobe20_scheduler_init,
    .xqc_scheduler_get_path         = xqc_fastprobe_scheduler_get_path,
};

const xqc_scheduler_callback_t xqc_fastonly_scheduler_cb = {
    .xqc_scheduler_size             = xqc_fastprobe_scheduler_size,
    .xqc_scheduler_init             = xqc_fastonly_scheduler_init,
    .xqc_scheduler_get_path         = xqc_fastprobe_scheduler_get_path,
};
