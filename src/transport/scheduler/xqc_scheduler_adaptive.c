#include <xquic/xqc_observer_clock.h>
#include "src/transport/scheduler/xqc_scheduler_adaptive.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/common/xqc_time.h"
#include "src/congestion_control/xqc_bbr_common.h"

#include <inttypes.h>
#include "src/transport/xqc_observer.h"
#include <stdio.h>
#include <string.h>

#define XQC_ADAPTIVE_CAP_EMA_ALPHA              0.20
#define XQC_ADAPTIVE_RATE_EMA_ALPHA             0.25
#define XQC_ADAPTIVE_MIN_CAPACITY_BPS           1024.0
#define XQC_ADAPTIVE_WARMUP_BYTES_PER_PATH      (256 * 1024)
#define XQC_ADAPTIVE_WARMUP_DELIVERED_MIN       (64 * 1024)
#define XQC_ADAPTIVE_WARMUP_MIN_US              100000
#define XQC_ADAPTIVE_WARMUP_MAX_US              800000
#define XQC_ADAPTIVE_ADJUST_INTERVAL_US          500000
#define XQC_ADAPTIVE_PRIMARY_HOLD_US             1000000
#define XQC_ADAPTIVE_SWITCH_MARGIN               1.20
#define XQC_ADAPTIVE_SWITCH_STREAK               3
#define XQC_ADAPTIVE_PROBE_MIN_PCT               0.0
#define XQC_ADAPTIVE_PROBE_FLOOR_PCT             5.0
#define XQC_ADAPTIVE_PROBE_MAX_PCT               45.0
#define XQC_ADAPTIVE_PROBE_STEP_PCT              5.0
#define XQC_ADAPTIVE_WINDOW_MAX_BYTES            (1024 * 1024)
#define XQC_ADAPTIVE_WINDOW_MAX_US               500000
#define XQC_ADAPTIVE_QUEUE_BAD_RATIO             1.75

typedef enum {
    XQC_ADAPTIVE_PHASE_WAIT_PATHS = 0,
    XQC_ADAPTIVE_PHASE_WARMUP,
    XQC_ADAPTIVE_PHASE_BULK,
} xqc_adaptive_phase_t;

typedef enum {
    XQC_ADAPTIVE_PICK_NONE = 0,
    XQC_ADAPTIVE_PICK_WARMUP,
    XQC_ADAPTIVE_PICK_PRIMARY,
    XQC_ADAPTIVE_PICK_PROBE,
    XQC_ADAPTIVE_PICK_CWND_FALLBACK,
    XQC_ADAPTIVE_PICK_REINJECT_FALLBACK,
    XQC_ADAPTIVE_PICK_BEST_FALLBACK,
    XQC_ADAPTIVE_PICK_SINGLE,
    XQC_ADAPTIVE_PICK_ORIGINAL,
} xqc_adaptive_pick_reason_t;

typedef struct {
    xqc_bool_t  in_use;
    uint64_t    path_id;
    uint64_t    min_srtt;
    uint64_t    init_delivered;
    uint64_t    last_delivered;
    xqc_usec_t  last_delivered_time;
    uint64_t    warmup_sent_base;
    uint64_t    warmup_sent_final;
    uint64_t    warmup_delivered_final;
    uint64_t    window_sent_base;
    uint64_t    adjust_sent_base;
    uint64_t    adjust_delivered_base;
    double      delivery_rate_ema;
    double      capacity_ema;
    double      score;
    uint64_t    blocked_count;
    xqc_bool_t  diag_base_valid;
    uint64_t    diag_sent_base;
    uint64_t    diag_app_sent_base;
    uint64_t    diag_delivered_base;
    uint64_t    diag_reinject_base;
    uint64_t    diag_retrans_base;
    unsigned    diag_lost_base;
    uint64_t    observer_evaluation_us, observer_packet_bytes;
    int         observer_block_reason, observer_check_cwnd, observer_reinject_excluded;
    uint64_t    observer_reason_count[9];
    uint64_t    observer_reason_bytes[9];
    uint64_t    selected_count;
    uint64_t    selected_bytes;
    uint64_t    primary_pick_count;
    uint64_t    probe_pick_count;
    uint64_t    cwnd_fallback_pick_count;
    uint64_t    reinject_fallback_pick_count;
    uint64_t    warmup_pick_count;
    uint64_t    single_pick_count;
    uint64_t    best_fallback_pick_count;
    uint64_t    original_pick_count;
    uint64_t    reinject_pick_count;
    uint64_t    reinject_selected_bytes;
} xqc_adaptive_path_state_t;

typedef struct {
    xqc_path_ctx_t              *path;
    xqc_adaptive_path_state_t   *state;
    xqc_send_ctl_t              *ctl;
    uint64_t                     path_id;
    uint64_t                     srtt;
    uint64_t                     cwnd;
    uint64_t                     inflight;
    uint64_t                     sent_total;
    uint64_t                     delivered_total;
    uint64_t                     warmup_sent;
    uint64_t                     window_sent;
    double                       capacity;
    double                       score;
    double                       queue_ratio;
    xqc_bool_t                   can_send;
    xqc_bool_t                   excluded_by_reinject;
    xqc_sched_path_eval_t        cwnd_eval;
} xqc_adaptive_candidate_t;

typedef struct {
    xqc_adaptive_path_state_t    states[XQC_MAX_PATHS_COUNT];
    xqc_adaptive_phase_t         phase;
    xqc_usec_t                   phase_start_us;
    xqc_usec_t                   warmup_elapsed_us;
    xqc_usec_t                   window_start_us;
    xqc_usec_t                   adjust_start_us;
    uint64_t                     primary_path_id;
    xqc_bool_t                   primary_valid;
    xqc_usec_t                   primary_since_us;
    uint64_t                     challenger_path_id;
    unsigned                     challenger_streak;
    double                       probe_pct;
    double                       primary_baseline_rate;
    xqc_bool_t                   probe_trial_active;
    double                       probe_before_trial;
    double                       probe_trial_reference_rate;
    unsigned                     probe_cooldown;
    uint64_t                     pick_count;
    xqc_usec_t                   diag_start_us;
    xqc_bool_t                   diag_emitted;
    uint64_t                     primary_pick_count;
    uint64_t                     probe_pick_count;
    uint64_t                     cwnd_fallback_pick_count;
    uint64_t                     reinject_fallback_pick_count;
    uint64_t                     warmup_pick_count;
    uint64_t                     single_pick_count;
    uint64_t                     best_fallback_pick_count;
    uint64_t                     original_pick_count;
    uint64_t                     reinject_pick_count;
    uint64_t                     no_sendable_count;
    uint64_t                     primary_select_count;
    uint64_t                     primary_switch_count;
    uint64_t                     adjust_count;
    uint64_t                     probe_increase_count;
    uint64_t                     probe_decrease_count;
    uint64_t                     trial_keep_count;
    uint64_t                     trial_revert_count;
    uint64_t                     primary_queue_shift_count;
    uint64_t                     secondary_queue_reduce_count;
    double                       min_probe_pct;
    double                       max_probe_pct;
    int                          observer_enabled;
    uint64_t                     observer_reason_count[9];
    uint64_t                     observer_reason_bytes[9];
    uint64_t                     observer_no_sendable_calls;
    uint64_t                     observer_reinject_bytes;
    uint64_t                     observer_real_bytes;
    uint64_t                     observer_reserved_bytes;
    uint64_t                     observer_secondary_id;
    xqc_sched_diag_t             sched_diag;
    uint64_t                     both_sendable;
    uint64_t                     primary_only;
    uint64_t                     secondary_only;
    uint64_t                     neither_sendable;
    uint64_t                     probe_considered;
    uint64_t                     probe_selected;
    uint64_t                     probe_denied_secondary_cwnd;
    uint64_t                     probe_denied_secondary_reinject;
    uint64_t                     probe_denied_secondary_other;
    uint64_t                     probe_denied_share_limit;
    uint64_t                     probe_denied_no_secondary;
    uint64_t                     fallback_real_inflight;
    uint64_t                     fallback_scheduled_reservation;
} xqc_adaptive_scheduler_t;

static size_t
xqc_adaptive_scheduler_size(void)
{
    return sizeof(xqc_adaptive_scheduler_t);
}

static void
xqc_adaptive_scheduler_init(void *scheduler, xqc_log_t *log,
    xqc_scheduler_params_t *params)
{
    (void)log;
    xqc_adaptive_scheduler_t *adaptive = scheduler;
    if (adaptive == NULL) {
        return;
    }

    memset(adaptive, 0, sizeof(*adaptive));
    adaptive->phase = XQC_ADAPTIVE_PHASE_WAIT_PATHS;
    adaptive->primary_path_id = UINT64_MAX;
    adaptive->challenger_path_id = UINT64_MAX;
    adaptive->probe_pct = XQC_ADAPTIVE_PROBE_FLOOR_PCT;
    xqc_scheduler_diag_init(&adaptive->sched_diag, "adaptive", params);
}

static double
xqc_adaptive_clamp(double value, double low, double high)
{
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

static uint64_t
xqc_adaptive_delta_u64(uint64_t value, uint64_t base)
{
    return value >= base ? value - base : 0;
}

static size_t
xqc_adaptive_packet_size(xqc_packet_out_t *packet_out)
{
    return packet_out && packet_out->po_used_size > 0
        ? packet_out->po_used_size : 1200;
}

static xqc_adaptive_path_state_t *
xqc_adaptive_find_state(xqc_adaptive_scheduler_t *adaptive, uint64_t path_id)
{
    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (adaptive->states[i].in_use
            && adaptive->states[i].path_id == path_id)
        {
            return &adaptive->states[i];
        }
    }
    return NULL;
}

static void
xqc_adaptive_diag_reset(xqc_adaptive_scheduler_t *adaptive,
    xqc_adaptive_candidate_t *candidates, size_t count, xqc_usec_t now)
{
    adaptive->diag_start_us = now;
    adaptive->diag_emitted = XQC_FALSE;
    adaptive->pick_count = 0;
    adaptive->primary_pick_count = 0;
    adaptive->probe_pick_count = 0;
    adaptive->cwnd_fallback_pick_count = 0;
    adaptive->reinject_fallback_pick_count = 0;
    adaptive->warmup_pick_count = 0;
    adaptive->single_pick_count = 0;
    adaptive->best_fallback_pick_count = 0;
    adaptive->original_pick_count = 0;
    adaptive->reinject_pick_count = 0;
    adaptive->no_sendable_count = 0;
    adaptive->primary_select_count = 0;
    adaptive->primary_switch_count = 0;
    adaptive->adjust_count = 0;
    adaptive->probe_increase_count = 0;
    adaptive->probe_decrease_count = 0;
    adaptive->trial_keep_count = 0;
    adaptive->trial_revert_count = 0;
    adaptive->primary_queue_shift_count = 0;
    adaptive->secondary_queue_reduce_count = 0;
    adaptive->min_probe_pct = adaptive->probe_pct;
    adaptive->max_probe_pct = adaptive->probe_pct;

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        xqc_adaptive_path_state_t *state = &adaptive->states[i];
        state->diag_base_valid = XQC_FALSE;
        state->blocked_count = 0;
        state->warmup_sent_final = 0;
        state->warmup_delivered_final = 0;
        state->selected_count = 0;
        state->selected_bytes = 0;
        state->primary_pick_count = 0;
        state->probe_pick_count = 0;
        state->cwnd_fallback_pick_count = 0;
        state->reinject_fallback_pick_count = 0;
        state->warmup_pick_count = 0;
        state->single_pick_count = 0;
        state->best_fallback_pick_count = 0;
        state->original_pick_count = 0;
        state->reinject_pick_count = 0;
        state->reinject_selected_bytes = 0;
    }

    for (size_t i = 0; i < count; i++) {
        xqc_adaptive_path_state_t *state = candidates[i].state;
        xqc_send_ctl_t *ctl = candidates[i].ctl;
        if (state == NULL || ctl == NULL) {
            continue;
        }
        state->diag_base_valid = XQC_TRUE;
        state->diag_sent_base = ctl->ctl_bytes_send;
        state->diag_app_sent_base = ctl->ctl_app_bytes_send;
        state->diag_delivered_base = ctl->ctl_delivered;
        state->diag_reinject_base = ctl->ctl_reinjected_bytes_send;
        state->diag_retrans_base = ctl->ctl_retrans_bytes_send;
        state->diag_lost_base = ctl->ctl_lost_count;
    }
}

static void
xqc_adaptive_diag_record_pick(xqc_adaptive_scheduler_t *adaptive,
    xqc_adaptive_candidate_t *selected, size_t packet_size,
    xqc_adaptive_pick_reason_t reason, int reinject)
{
    if (adaptive == NULL || selected == NULL || selected->state == NULL) {
        return;
    }

    xqc_adaptive_path_state_t *state = selected->state;
    adaptive->pick_count++;
    if (adaptive->observer_enabled && (unsigned)reason < 9) {
        adaptive->observer_reason_count[reason]++;
        state->observer_reason_count[reason]++;
        state->observer_reason_bytes[reason] += packet_size;
        adaptive->observer_reason_bytes[reason] += packet_size;
    }
    if (adaptive->observer_enabled && reinject) { adaptive->observer_reinject_bytes += packet_size; }
    state->selected_count++;
    state->selected_bytes += packet_size;

    switch (reason) {
    case XQC_ADAPTIVE_PICK_WARMUP:
        adaptive->warmup_pick_count++;
        state->warmup_pick_count++;
        break;
    case XQC_ADAPTIVE_PICK_PRIMARY:
        adaptive->primary_pick_count++;
        state->primary_pick_count++;
        break;
    case XQC_ADAPTIVE_PICK_PROBE:
        adaptive->probe_pick_count++;
        state->probe_pick_count++;
        break;
    case XQC_ADAPTIVE_PICK_CWND_FALLBACK:
        adaptive->cwnd_fallback_pick_count++;
        state->cwnd_fallback_pick_count++;
        break;
    case XQC_ADAPTIVE_PICK_REINJECT_FALLBACK:
        adaptive->reinject_fallback_pick_count++;
        state->reinject_fallback_pick_count++;
        break;
    case XQC_ADAPTIVE_PICK_BEST_FALLBACK:
        adaptive->best_fallback_pick_count++;
        state->best_fallback_pick_count++;
        break;
    case XQC_ADAPTIVE_PICK_SINGLE:
        adaptive->single_pick_count++;
        state->single_pick_count++;
        break;
    case XQC_ADAPTIVE_PICK_ORIGINAL:
        adaptive->original_pick_count++;
        state->original_pick_count++;
        break;
    default:
        break;
    }

    if (reinject) {
        adaptive->reinject_pick_count++;
        state->reinject_pick_count++;
        state->reinject_selected_bytes += packet_size;
    }
}

static xqc_adaptive_path_state_t *
xqc_adaptive_get_state(xqc_adaptive_scheduler_t *adaptive,
    xqc_path_ctx_t *path, xqc_usec_t now)
{
    xqc_adaptive_path_state_t *free_slot = NULL;

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        xqc_adaptive_path_state_t *state = &adaptive->states[i];
        if (state->in_use && state->path_id == path->path_id) {
            return state;
        }
        if (!state->in_use && free_slot == NULL) {
            free_slot = state;
        }
    }

    if (free_slot == NULL || path->path_send_ctl == NULL) {
        return NULL;
    }

    xqc_send_ctl_t *ctl = path->path_send_ctl;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->in_use = XQC_TRUE;
    free_slot->path_id = path->path_id;
    free_slot->min_srtt = UINT64_MAX;
    free_slot->init_delivered = ctl->ctl_delivered;
    free_slot->last_delivered = ctl->ctl_delivered;
    free_slot->last_delivered_time = ctl->ctl_delivered_time > 0
        ? ctl->ctl_delivered_time : now;
    free_slot->warmup_sent_base = ctl->ctl_bytes_send;
    free_slot->window_sent_base = ctl->ctl_bytes_send;
    free_slot->adjust_sent_base = ctl->ctl_bytes_send;
    free_slot->adjust_delivered_base = ctl->ctl_delivered;
    if (adaptive->diag_start_us > 0) {
        free_slot->diag_base_valid = XQC_TRUE;
        free_slot->diag_sent_base = ctl->ctl_bytes_send;
        free_slot->diag_app_sent_base = ctl->ctl_app_bytes_send;
        free_slot->diag_delivered_base = ctl->ctl_delivered;
        free_slot->diag_reinject_base = ctl->ctl_reinjected_bytes_send;
        free_slot->diag_retrans_base = ctl->ctl_retrans_bytes_send;
        free_slot->diag_lost_base = ctl->ctl_lost_count;
    }
    return free_slot;
}

static void
xqc_adaptive_reclaim_stale_states(xqc_adaptive_scheduler_t *adaptive,
    xqc_connection_t *conn)
{
    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        xqc_adaptive_path_state_t *state = &adaptive->states[i];
        if (!state->in_use) {
            continue;
        }

        xqc_bool_t found = XQC_FALSE;
        xqc_list_head_t *pos, *next;
        xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
            xqc_path_ctx_t *path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
            if (path->path_id == state->path_id
                && path->path_state == XQC_PATH_STATE_ACTIVE)
            {
                found = XQC_TRUE;
                break;
            }
        }

        if (!found) {
            memset(state, 0, sizeof(*state));
        }
    }
}

static void
xqc_adaptive_refresh_candidate(xqc_adaptive_candidate_t *candidate,
    xqc_usec_t now)
{
    xqc_send_ctl_t *ctl = candidate->ctl;
    xqc_adaptive_path_state_t *state = candidate->state;
    uint64_t est_bw = xqc_send_ctl_get_est_bw(ctl);
    double sample = 0;
    double loss_rate;

    candidate->srtt = xqc_send_ctl_get_srtt(ctl);
    if (candidate->srtt == 0) {
        candidate->srtt = ctl->ctl_conn
            ? ctl->ctl_conn->conn_settings.initial_rtt : 1;
    }
    if (candidate->srtt == 0) {
        candidate->srtt = 1;
    }

    candidate->cwnd = 0;
    if (ctl->ctl_cong_callback
        && ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd)
    {
        candidate->cwnd =
            ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(ctl->ctl_cong);
    }
    candidate->inflight = ctl->ctl_bytes_in_flight
                        + candidate->path->path_schedule_bytes;
    candidate->sent_total = ctl->ctl_bytes_send;
    candidate->delivered_total = ctl->ctl_delivered;

    xqc_usec_t delivered_time = ctl->ctl_delivered_time > 0
        ? ctl->ctl_delivered_time : now;
    if (ctl->ctl_delivered > state->last_delivered
        && delivered_time > state->last_delivered_time)
    {
        double delivered_rate =
            (double)(ctl->ctl_delivered - state->last_delivered) * 1000000.0
            / (double)(delivered_time - state->last_delivered_time);
        if (state->delivery_rate_ema <= 0) {
            state->delivery_rate_ema = delivered_rate;
        } else {
            state->delivery_rate_ema =
                (1.0 - XQC_ADAPTIVE_RATE_EMA_ALPHA) * state->delivery_rate_ema
                + XQC_ADAPTIVE_RATE_EMA_ALPHA * delivered_rate;
        }
        state->last_delivered = ctl->ctl_delivered;
        state->last_delivered_time = delivered_time;
    }

    if (state->delivery_rate_ema > 0 && est_bw > 0) {
        sample = 0.5 * state->delivery_rate_ema + 0.5 * (double)est_bw;
    } else if (state->delivery_rate_ema > 0) {
        sample = state->delivery_rate_ema;
    } else if (est_bw > 0) {
        sample = (double)est_bw;
    } else if (candidate->cwnd > 0) {
        sample = (double)candidate->cwnd * 1000000.0
               / (double)candidate->srtt;
    }

    if (sample < XQC_ADAPTIVE_MIN_CAPACITY_BPS) {
        sample = XQC_ADAPTIVE_MIN_CAPACITY_BPS;
    }
    if (state->capacity_ema <= 0) {
        state->capacity_ema = sample;
    } else {
        state->capacity_ema =
            (1.0 - XQC_ADAPTIVE_CAP_EMA_ALPHA) * state->capacity_ema
            + XQC_ADAPTIVE_CAP_EMA_ALPHA * sample;
    }

    if (candidate->srtt < state->min_srtt) {
        state->min_srtt = candidate->srtt;
    }
    candidate->queue_ratio = state->min_srtt == 0
        || state->min_srtt == UINT64_MAX ? 1.0
        : (double)candidate->srtt / (double)state->min_srtt;
    candidate->queue_ratio = xqc_adaptive_clamp(candidate->queue_ratio, 1.0, 4.0);

    loss_rate = xqc_path_recent_loss_rate(candidate->path);
    candidate->capacity = state->capacity_ema;
    candidate->score = candidate->capacity
        / (1.0 + 0.75 * (candidate->queue_ratio - 1.0));
    if (loss_rate > 0) {
        candidate->score = candidate->score * 100.0
                         / (100.0 + 4.0 * loss_rate);
    }
    state->score = candidate->score;
    candidate->warmup_sent = xqc_adaptive_delta_u64(candidate->sent_total,
                                                     state->warmup_sent_base);
    candidate->window_sent = xqc_adaptive_delta_u64(candidate->sent_total,
                                                     state->window_sent_base);
}

static size_t
xqc_adaptive_collect_candidates(xqc_adaptive_scheduler_t *adaptive,
    xqc_connection_t *conn, xqc_packet_out_t *packet_out, int check_cwnd,
    int reinject, xqc_adaptive_candidate_t *available,
    xqc_adaptive_candidate_t *standby, xqc_path_ctx_t **original_path,
    xqc_bool_t *has_active, xqc_bool_t *has_sendable, xqc_usec_t now)
{
    size_t available_cnt = 0;
    size_t standby_cnt = 0;
    xqc_list_head_t *pos, *next;

    *original_path = NULL;
    *has_active = XQC_FALSE;
    *has_sendable = XQC_FALSE;

    xqc_adaptive_reclaim_stale_states(adaptive, conn);

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        xqc_path_ctx_t *path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        if (path->path_state != XQC_PATH_STATE_ACTIVE
            || path->app_path_status == XQC_APP_PATH_STATUS_FROZEN)
        {
            continue;
        }

        *has_active = XQC_TRUE;
        xqc_adaptive_path_state_t *state =
            xqc_adaptive_get_state(adaptive, path, now);
        if (state == NULL || path->path_send_ctl == NULL) {
            continue;
        }

        xqc_adaptive_candidate_t candidate;
        memset(&candidate, 0, sizeof(candidate));
        candidate.path = path;
        candidate.path_id = path->path_id;
        candidate.state = state;
        candidate.ctl = path->path_send_ctl;
        candidate.can_send = xqc_scheduler_evaluate_path(path,
            packet_out, check_cwnd, &candidate.cwnd_eval);
        xqc_scheduler_diag_record_evaluation(&adaptive->sched_diag, path,
            &candidate.cwnd_eval);
        if (adaptive->observer_enabled) {
            state->observer_evaluation_us = xqc_observer_clock_us();
            if (!state->observer_evaluation_us) { conn->observer->invalid = 1; }
            state->observer_packet_bytes = candidate.cwnd_eval.packet_size;
            state->observer_block_reason = candidate.cwnd_eval.block_reason;
            state->observer_check_cwnd = check_cwnd;
            state->observer_reinject_excluded = reinject && packet_out && packet_out->po_path_id == path->path_id;
        }
        if (!candidate.can_send) {
            state->blocked_count++;
        } else {
            *has_sendable = XQC_TRUE;
        }

        if (reinject && packet_out && packet_out->po_path_id == path->path_id) {
            *original_path = path;
            candidate.can_send = XQC_FALSE;
            candidate.excluded_by_reinject = XQC_TRUE;
        }

        xqc_adaptive_refresh_candidate(&candidate, now);
        if (path->app_path_status == XQC_APP_PATH_STATUS_AVAILABLE) {
            if (available_cnt < XQC_MAX_PATHS_COUNT) {
                available[available_cnt++] = candidate;
            }
        } else if (path->app_path_status == XQC_APP_PATH_STATUS_STANDBY) {
            if (standby_cnt < XQC_MAX_PATHS_COUNT) {
                standby[standby_cnt++] = candidate;
            }
        }
    }

    if (available_cnt > 0) {
        return available_cnt;
    }
    if (standby_cnt > 0) {
        memcpy(available, standby,
               standby_cnt * sizeof(xqc_adaptive_candidate_t));
    }
    return standby_cnt;
}

static xqc_adaptive_candidate_t *
xqc_adaptive_find_candidate(xqc_adaptive_candidate_t *candidates,
    size_t count, uint64_t path_id)
{
    for (size_t i = 0; i < count; i++) {
        if (candidates[i].path_id == path_id) {
            return &candidates[i];
        }
    }
    return NULL;
}

static xqc_adaptive_candidate_t *
xqc_adaptive_best_candidate(xqc_adaptive_candidate_t *candidates,
    size_t count, xqc_bool_t require_sendable)
{
    xqc_adaptive_candidate_t *best = NULL;
    for (size_t i = 0; i < count; i++) {
        if (require_sendable && !candidates[i].can_send) {
            continue;
        }
        if (best == NULL || candidates[i].score > best->score
            || (candidates[i].score >= best->score
                && candidates[i].srtt < best->srtt))
        {
            best = &candidates[i];
        }
    }
    return best;
}

static void
xqc_adaptive_reset_window(xqc_adaptive_scheduler_t *adaptive,
    xqc_adaptive_candidate_t *candidates, size_t count, xqc_usec_t now)
{
    adaptive->window_start_us = now;
    for (size_t i = 0; i < count; i++) {
        candidates[i].state->window_sent_base = candidates[i].sent_total;
        candidates[i].window_sent = 0;
    }
}

static double
xqc_adaptive_initial_probe_pct(xqc_adaptive_candidate_t *primary,
    xqc_adaptive_candidate_t *secondary)
{
    if (primary == NULL || secondary == NULL || primary->score <= 0) {
        return XQC_ADAPTIVE_PROBE_FLOOR_PCT;
    }

    double ratio = secondary->score / primary->score;
    if (ratio >= 0.85) {
        return 40.0;
    }
    if (ratio >= 0.65) {
        return 20.0;
    }
    if (ratio >= 0.40) {
        return 10.0;
    }
    return XQC_ADAPTIVE_PROBE_FLOOR_PCT;
}

static void
xqc_adaptive_diag_print_primary_paths(xqc_adaptive_scheduler_t *adaptive,
    xqc_adaptive_candidate_t *candidates, size_t count, xqc_usec_t now,
    const char *reason)
{
    if (!adaptive->sched_diag.enabled) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        xqc_send_ctl_t *ctl = candidates[i].ctl;
        uint8_t bbr_mode = 255, full_bw = 0;
        xqc_bbr_info_interface_t *info = ctl->ctl_cong_callback
            ? ctl->ctl_cong_callback->xqc_cong_ctl_info_cb : NULL;
        if (info && info->mode && info->full_bw_reached) {
            bbr_mode = info->mode(ctl->ctl_cong);
            full_bw = info->full_bw_reached(ctl->ctl_cong);
        }
        printf("[diag_adaptive_primary_path] reason=%s path=%" PRIu64
               " warmup_elapsed=%" PRIu64 " warmup_sent=%" PRIu64
               " warmup_delivered=%" PRIu64 " est_bw=%" PRIu64
               " cwnd=%" PRIu64 " inflight=%u scheduled=%u"
               " srtt=%" PRIu64 " app_limited_samples=%" PRIu64
               " non_app_limited_samples=%" PRIu64
               " bbr_mode=%u full_bw_reached=%u\n", reason,
               candidates[i].path_id,
               adaptive->warmup_elapsed_us > 0
                   ? adaptive->warmup_elapsed_us
                   : (adaptive->phase_start_us > 0
                       ? now - adaptive->phase_start_us : 0),
               adaptive->warmup_elapsed_us > 0
                   ? candidates[i].state->warmup_sent_final
                   : candidates[i].warmup_sent,
               adaptive->warmup_elapsed_us > 0
                   ? candidates[i].state->warmup_delivered_final
                   : xqc_adaptive_delta_u64(candidates[i].delivered_total,
                       candidates[i].state->init_delivered),
               xqc_send_ctl_get_est_bw(ctl), candidates[i].cwnd,
               ctl->ctl_bytes_in_flight,
               candidates[i].path->path_schedule_bytes,
               candidates[i].srtt, ctl->diag.sample_app_limited,
               ctl->diag.sample_non_app_limited, bbr_mode, full_bw);
    }
}

static void
xqc_adaptive_select_primary(xqc_adaptive_scheduler_t *adaptive,
    xqc_connection_t *conn, xqc_adaptive_candidate_t *candidates,
    size_t count, xqc_usec_t now, const char *reason)
{
    xqc_adaptive_candidate_t *primary =
        xqc_adaptive_best_candidate(candidates, count, XQC_FALSE);
    if (primary == NULL) {
        return;
    }

    xqc_adaptive_candidate_t *secondary = NULL;
    for (size_t i = 0; i < count; i++) {
        if (candidates[i].path_id == primary->path_id) {
            continue;
        }
        if (secondary == NULL || candidates[i].score > secondary->score) {
            secondary = &candidates[i];
        }
    }

    xqc_bool_t primary_changed = adaptive->primary_valid
        && adaptive->primary_path_id != primary->path_id;
    adaptive->primary_path_id = primary->path_id;
    adaptive->primary_valid = XQC_TRUE;
    adaptive->primary_since_us = now;
    adaptive->challenger_path_id = UINT64_MAX;
    adaptive->challenger_streak = 0;
    adaptive->probe_pct = xqc_adaptive_initial_probe_pct(primary, secondary);
    /* Warmup ACK bursts are not a stable baseline. Calibrate on a full window. */
    adaptive->primary_baseline_rate = 0;
    adaptive->probe_trial_active = XQC_FALSE;
    adaptive->probe_before_trial = adaptive->probe_pct;
    adaptive->probe_trial_reference_rate = 0;
    adaptive->probe_cooldown = 0;
    adaptive->adjust_start_us = now;
    adaptive->primary_select_count++;
    if (primary_changed) {
        adaptive->primary_switch_count++;
    }
    if (adaptive->diag_start_us > 0) {
        adaptive->min_probe_pct = xqc_min(adaptive->min_probe_pct,
                                          adaptive->probe_pct);
        adaptive->max_probe_pct = xqc_max(adaptive->max_probe_pct,
                                          adaptive->probe_pct);
    }
    for (size_t i = 0; i < count; i++) {
        candidates[i].state->adjust_sent_base = candidates[i].sent_total;
        candidates[i].state->adjust_delivered_base =
            candidates[i].delivered_total;
    }
    xqc_adaptive_reset_window(adaptive, candidates, count, now);

    printf("[diag_adaptive] conn=%p event=primary_select reason=%s primary=%" PRIu64
           " score=%.0f srtt=%" PRIu64 " probe=%.1f secondary=%" PRIu64
           " secondary_score=%.0f\n",
           (void *)conn, reason, primary->path_id, primary->score, primary->srtt,
           adaptive->probe_pct,
           secondary ? secondary->path_id : UINT64_MAX,
           secondary ? secondary->score : 0.0);
    xqc_adaptive_diag_print_primary_paths(adaptive, candidates, count, now,
        reason);
}

static void
xqc_adaptive_start_warmup(xqc_adaptive_scheduler_t *adaptive,
    xqc_connection_t *conn, xqc_adaptive_candidate_t *candidates,
    size_t count, xqc_usec_t now)
{
    adaptive->phase = XQC_ADAPTIVE_PHASE_WARMUP;
    adaptive->phase_start_us = now;
    adaptive->warmup_elapsed_us = 0;
    xqc_adaptive_diag_reset(adaptive, candidates, count, now);
    for (size_t i = 0; i < count; i++) {
        candidates[i].state->warmup_sent_base = candidates[i].sent_total;
        candidates[i].state->warmup_sent_final = 0;
        candidates[i].state->warmup_delivered_final = 0;
        candidates[i].state->init_delivered = candidates[i].delivered_total;
        candidates[i].warmup_sent = 0;
    }
    printf("[diag_adaptive] conn=%p event=warmup_start paths=%zu bytes_per_path=%u\n",
           (void *)conn, count, (unsigned)XQC_ADAPTIVE_WARMUP_BYTES_PER_PATH);
}

static xqc_bool_t
xqc_adaptive_warmup_complete(xqc_adaptive_candidate_t *candidates,
    size_t count, xqc_usec_t elapsed)
{
    xqc_bool_t all_sent = XQC_TRUE;
    xqc_bool_t all_delivered = XQC_TRUE;

    for (size_t i = 0; i < count; i++) {
        if (candidates[i].warmup_sent < XQC_ADAPTIVE_WARMUP_BYTES_PER_PATH) {
            all_sent = XQC_FALSE;
        }
        if (xqc_adaptive_delta_u64(candidates[i].delivered_total,
                candidates[i].state->init_delivered)
            < XQC_ADAPTIVE_WARMUP_DELIVERED_MIN)
        {
            all_delivered = XQC_FALSE;
        }
    }

    if (elapsed >= XQC_ADAPTIVE_WARMUP_MAX_US) {
        return XQC_TRUE;
    }
    return all_sent && all_delivered
        && elapsed >= XQC_ADAPTIVE_WARMUP_MIN_US;
}

static xqc_adaptive_candidate_t *
xqc_adaptive_pick_warmup(xqc_adaptive_candidate_t *candidates,
    size_t count)
{
    xqc_adaptive_candidate_t *selected = NULL;
    xqc_bool_t need_more = XQC_FALSE;

    for (size_t i = 0; i < count; i++) {
        if (candidates[i].can_send
            && candidates[i].warmup_sent < XQC_ADAPTIVE_WARMUP_BYTES_PER_PATH)
        {
            need_more = XQC_TRUE;
            if (selected == NULL
                || candidates[i].warmup_sent < selected->warmup_sent
                || (candidates[i].warmup_sent == selected->warmup_sent
                    && candidates[i].score > selected->score))
            {
                selected = &candidates[i];
            }
        }
    }

    if (need_more) {
        return selected;
    }
    return xqc_adaptive_best_candidate(candidates, count, XQC_TRUE);
}

static void
xqc_adaptive_adjust(xqc_adaptive_scheduler_t *adaptive,
    xqc_connection_t *conn, xqc_adaptive_candidate_t *candidates,
    size_t count, xqc_usec_t now)
{
    if (adaptive->adjust_start_us == 0
        || now - adaptive->adjust_start_us < XQC_ADAPTIVE_ADJUST_INTERVAL_US)
    {
        return;
    }

    double interval = (double)(now - adaptive->adjust_start_us);
    double rates[XQC_MAX_PATHS_COUNT] = {0};
    double total_rate = 0;
    xqc_adaptive_candidate_t *primary = xqc_adaptive_find_candidate(
        candidates, count, adaptive->primary_path_id);
    xqc_adaptive_candidate_t *challenger =
        xqc_adaptive_best_candidate(candidates, count, XQC_FALSE);
    double primary_rate = 0;

    for (size_t i = 0; i < count; i++) {
        rates[i] = (double)xqc_adaptive_delta_u64(
            candidates[i].delivered_total,
            candidates[i].state->adjust_delivered_base) * 1000000.0 / interval;
        total_rate += rates[i];
        if (candidates[i].path_id == adaptive->primary_path_id) {
            primary_rate = rates[i];
        }
    }

    if (primary == NULL) {
        xqc_adaptive_select_primary(adaptive, conn, candidates, count, now,
                                    "primary_missing");
        return;
    }

    xqc_bool_t baseline_calibrating = XQC_FALSE;
    if (primary_rate > 0) {
        if (adaptive->primary_baseline_rate <= 0) {
            adaptive->primary_baseline_rate = primary_rate;
            baseline_calibrating = XQC_TRUE;
        } else {
            double bounded_rate = xqc_adaptive_clamp(primary_rate,
                adaptive->primary_baseline_rate * 0.50,
                adaptive->primary_baseline_rate * 1.50);
            adaptive->primary_baseline_rate =
                0.80 * adaptive->primary_baseline_rate + 0.20 * bounded_rate;
        }
    }

    if (challenger != NULL && challenger->path_id != primary->path_id
        && challenger->score > primary->score * XQC_ADAPTIVE_SWITCH_MARGIN
        && now - adaptive->primary_since_us >= XQC_ADAPTIVE_PRIMARY_HOLD_US)
    {
        if (adaptive->challenger_path_id == challenger->path_id) {
            adaptive->challenger_streak++;
        } else {
            adaptive->challenger_path_id = challenger->path_id;
            adaptive->challenger_streak = 1;
        }
        if (adaptive->challenger_streak >= XQC_ADAPTIVE_SWITCH_STREAK) {
            xqc_adaptive_select_primary(adaptive, conn, candidates, count, now,
                                        "challenger_stable");
            return;
        }
    } else {
        adaptive->challenger_path_id = UINT64_MAX;
        adaptive->challenger_streak = 0;
    }

    xqc_adaptive_candidate_t *secondary = NULL;
    for (size_t i = 0; i < count; i++) {
        if (candidates[i].path_id != primary->path_id
            && (secondary == NULL || candidates[i].score > secondary->score))
        {
            secondary = &candidates[i];
        }
    }

    double old_probe = adaptive->probe_pct;
    double next_probe = old_probe;
    double max_probe = secondary == NULL ? 0 : XQC_ADAPTIVE_PROBE_MAX_PCT;
    const char *adjust_reason = "hold";
    xqc_bool_t primary_more_queued = secondary != NULL
        && primary->queue_ratio >= 1.50
        && primary->queue_ratio > secondary->queue_ratio + 0.25;
    xqc_bool_t secondary_more_queued = secondary != NULL
        && secondary->queue_ratio >= XQC_ADAPTIVE_QUEUE_BAD_RATIO
        && secondary->queue_ratio > primary->queue_ratio + 0.25;

    if (secondary == NULL) {
        next_probe = 0;
        adaptive->probe_trial_active = XQC_FALSE;
        adaptive->probe_cooldown = 0;
        adjust_reason = "single_path";

    } else if (baseline_calibrating) {
        adjust_reason = "baseline_calibrate";

    } else if (adaptive->probe_trial_active) {
        xqc_bool_t trial_kept = adaptive->probe_trial_reference_rate <= 0
            || total_rate >= adaptive->probe_trial_reference_rate * 0.98;
        if (trial_kept && !secondary_more_queued) {
            adaptive->probe_trial_active = XQC_FALSE;
            adaptive->probe_cooldown = 0;
            adaptive->trial_keep_count++;
            adjust_reason = "trial_keep";
        } else {
            next_probe = adaptive->probe_before_trial;
            adaptive->probe_trial_active = XQC_FALSE;
            adaptive->probe_cooldown = 2;
            adaptive->trial_revert_count++;
            adjust_reason = "trial_revert";
        }

    } else if (secondary_more_queued) {
        next_probe = old_probe - XQC_ADAPTIVE_PROBE_STEP_PCT;
        adaptive->probe_cooldown = 1;
        adaptive->secondary_queue_reduce_count++;
        adjust_reason = "secondary_queue";

    } else if (adaptive->probe_cooldown > 0) {
        adaptive->probe_cooldown--;
        adjust_reason = "cooldown";

    } else if (old_probe + XQC_ADAPTIVE_PROBE_STEP_PCT <= max_probe) {
        adaptive->probe_trial_active = XQC_TRUE;
        adaptive->probe_before_trial = old_probe;
        adaptive->probe_trial_reference_rate = total_rate;
        next_probe = old_probe + XQC_ADAPTIVE_PROBE_STEP_PCT;
        if (primary_more_queued) {
            adaptive->primary_queue_shift_count++;
        }
        adjust_reason = primary_more_queued ? "primary_queue_shift" : "explore_up";
    }

    adaptive->probe_pct = xqc_adaptive_clamp(next_probe,
        XQC_ADAPTIVE_PROBE_MIN_PCT, max_probe);
    adaptive->adjust_count++;
    if (adaptive->probe_pct > old_probe) {
        adaptive->probe_increase_count++;
    } else if (adaptive->probe_pct < old_probe) {
        adaptive->probe_decrease_count++;
    }
    adaptive->min_probe_pct = xqc_min(adaptive->min_probe_pct,
                                      adaptive->probe_pct);
    adaptive->max_probe_pct = xqc_max(adaptive->max_probe_pct,
                                      adaptive->probe_pct);

    /* Periodic adjustment state remains in memory; transfer hot paths do not print it. */
    (void)adjust_reason;

    adaptive->adjust_start_us = now;
    for (size_t i = 0; i < count; i++) {
        candidates[i].state->adjust_sent_base = candidates[i].sent_total;
        candidates[i].state->adjust_delivered_base =
            candidates[i].delivered_total;
    }
    xqc_adaptive_reset_window(adaptive, candidates, count, now);
}

static xqc_adaptive_candidate_t *
xqc_adaptive_pick_bulk(xqc_adaptive_scheduler_t *adaptive,
    xqc_adaptive_candidate_t *candidates, size_t count,
    size_t packet_size, xqc_bool_t *intentionally_blocked,
    xqc_adaptive_pick_reason_t *pick_reason)
{
    xqc_adaptive_candidate_t *primary = xqc_adaptive_find_candidate(
        candidates, count, adaptive->primary_path_id);
    xqc_adaptive_candidate_t *secondary = NULL;
    uint64_t window_total = 0;

    *intentionally_blocked = XQC_FALSE;
    *pick_reason = XQC_ADAPTIVE_PICK_NONE;
    for (size_t i = 0; i < count; i++) {
        window_total += candidates[i].window_sent;
        if (candidates[i].path_id != adaptive->primary_path_id
            && (secondary == NULL || candidates[i].score > secondary->score))
        {
            secondary = &candidates[i];
        }
    }

    if (primary == NULL) {
        *pick_reason = XQC_ADAPTIVE_PICK_BEST_FALLBACK;
        return xqc_adaptive_best_candidate(candidates, count, XQC_TRUE);
    }
    if (secondary == NULL) {
        if (adaptive->sched_diag.enabled || adaptive->observer_enabled) {
            adaptive->probe_denied_no_secondary++;
        }
        if (primary->can_send) {
            *pick_reason = XQC_ADAPTIVE_PICK_PRIMARY;
        }
        return primary->can_send ? primary : NULL;
    }
    if (!secondary->can_send) {
        if (adaptive->sched_diag.enabled || adaptive->observer_enabled) {
            adaptive->probe_considered++;
            if (secondary->excluded_by_reinject) {
                adaptive->probe_denied_secondary_reinject++;
            } else if (secondary->cwnd_eval.block_reason
                       != XQC_SCHED_BLOCK_NONE)
            {
                adaptive->probe_denied_secondary_cwnd++;
            } else {
                adaptive->probe_denied_secondary_other++;
            }
        }
        if (primary->can_send) {
            *pick_reason = XQC_ADAPTIVE_PICK_PRIMARY;
        }
        return primary->can_send ? primary : NULL;
    }

    if (adaptive->sched_diag.enabled || adaptive->observer_enabled) {
        adaptive->probe_considered++;
    }

    xqc_bool_t probe_allowed = XQC_FALSE;
    if (window_total > 0) {
        double allowed = (double)(window_total + packet_size)
                       * adaptive->probe_pct / 100.0 + (double)packet_size;
        probe_allowed = (double)(secondary->window_sent + packet_size) <= allowed;
    }

    if (primary->can_send) {
        if (adaptive->sched_diag.enabled || adaptive->observer_enabled) {
            if (probe_allowed) {
                adaptive->probe_selected++;
            } else {
                adaptive->probe_denied_share_limit++;
            }
        }
        *pick_reason = probe_allowed
            ? XQC_ADAPTIVE_PICK_PROBE : XQC_ADAPTIVE_PICK_PRIMARY;
        return probe_allowed ? secondary : primary;
    }

    /* Do not idle the connection merely to preserve a target byte share. */
    if (adaptive->sched_diag.enabled || adaptive->observer_enabled) {
        if (primary->cwnd_eval.block_reason == XQC_SCHED_BLOCK_REAL_INFLIGHT) {
            adaptive->fallback_real_inflight++;
            adaptive->observer_real_bytes += packet_size;
        } else if (primary->cwnd_eval.block_reason
                   == XQC_SCHED_BLOCK_SCHEDULED_RESERVATION)
        {
            adaptive->fallback_scheduled_reservation++;
            adaptive->observer_reserved_bytes += packet_size;
        }
    }
    *pick_reason = primary->excluded_by_reinject
        ? XQC_ADAPTIVE_PICK_REINJECT_FALLBACK
        : XQC_ADAPTIVE_PICK_CWND_FALLBACK;
    return secondary;
}

static xqc_path_ctx_t *
xqc_adaptive_diag_return(xqc_adaptive_scheduler_t *adaptive,
    xqc_connection_t *conn, xqc_packet_out_t *packet_out,
    xqc_path_ctx_t *selected, xqc_bool_t no_sendable, xqc_usec_t now)
{
    if (conn->observer) {
    if (!selected) { adaptive->observer_no_sendable_calls++; }
    uint64_t secondary = UINT64_MAX;
    double secondary_score = -1.0;
    for (unsigned i = 0; i < XQC_MAX_PATHS_COUNT; ++i) {
        if (adaptive->states[i].in_use
            && adaptive->states[i].path_id != adaptive->primary_path_id) {
            xqc_path_ctx_t *candidate = xqc_conn_find_path_by_path_id(conn, adaptive->states[i].path_id);
            if (candidate && candidate->path_state == XQC_PATH_STATE_ACTIVE
                && adaptive->states[i].score > secondary_score) {
                secondary = adaptive->states[i].path_id; secondary_score = adaptive->states[i].score;
            }
        }
    }
    if (!adaptive->primary_valid) { secondary = UINT64_MAX; }
    adaptive->observer_secondary_id = secondary;
    xqc_observer_role(conn, adaptive->primary_valid, adaptive->primary_path_id,
        secondary, adaptive->probe_pct, adaptive->phase);
    }
    xqc_scheduler_diag_record_decision(&adaptive->sched_diag, selected,
        packet_out ? packet_out->po_used_size : 0, no_sendable);
    xqc_scheduler_diag_maybe_tick(&adaptive->sched_diag, conn, now);
    return selected;
}

xqc_path_ctx_t *
xqc_adaptive_scheduler_get_path(void *scheduler, xqc_connection_t *conn,
    xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_bool_t *cc_blocked)
{
    xqc_adaptive_scheduler_t *adaptive = scheduler;
    xqc_adaptive_candidate_t candidates[XQC_MAX_PATHS_COUNT];
    xqc_adaptive_candidate_t standby[XQC_MAX_PATHS_COUNT];
    xqc_path_ctx_t *original_path = NULL;
    xqc_bool_t has_active = XQC_FALSE;
    xqc_bool_t has_sendable = XQC_FALSE;
    xqc_bool_t intentionally_blocked = XQC_FALSE;
    xqc_adaptive_pick_reason_t pick_reason = XQC_ADAPTIVE_PICK_NONE;
    xqc_usec_t now = xqc_monotonic_timestamp();
    size_t packet_size = xqc_adaptive_packet_size(packet_out);
    size_t count;

    if (cc_blocked) {
        *cc_blocked = XQC_FALSE;
    }
    if (adaptive == NULL) {
        return NULL;
    }

    adaptive->observer_enabled = conn->observer != NULL;
    memset(candidates, 0, sizeof(candidates));
    memset(standby, 0, sizeof(standby));
    count = xqc_adaptive_collect_candidates(adaptive, conn, packet_out,
        check_cwnd, reinject, candidates, standby, &original_path,
        &has_active, &has_sendable, now);

    if ((adaptive->sched_diag.enabled || adaptive->observer_enabled) && count >= 2
        && adaptive->primary_valid)
    {
        xqc_adaptive_candidate_t *primary = xqc_adaptive_find_candidate(
            candidates, count, adaptive->primary_path_id);
        xqc_adaptive_candidate_t *secondary = NULL;
        for (size_t i = 0; i < count; i++) {
            if (candidates[i].path_id != adaptive->primary_path_id) {
                secondary = &candidates[i];
                break;
            }
        }
        if (primary && secondary) {
            adaptive->both_sendable += primary->can_send && secondary->can_send;
            adaptive->primary_only += primary->can_send && !secondary->can_send;
            adaptive->secondary_only += !primary->can_send && secondary->can_send;
            adaptive->neither_sendable += !primary->can_send && !secondary->can_send;
        }
    }

    if (count == 0) {
        if (has_active && !has_sendable) {
            adaptive->no_sendable_count++;
        }
        if (cc_blocked) {
            *cc_blocked = has_active && !has_sendable;
        }
        if (original_path != NULL && packet_out
            && !(packet_out->po_flag & XQC_POF_REINJECT_DIFF_PATH))
        {
            if (adaptive->observer_enabled) {
                adaptive->observer_reason_count[XQC_ADAPTIVE_PICK_ORIGINAL]++;
                adaptive->observer_reason_bytes[XQC_ADAPTIVE_PICK_ORIGINAL] += packet_size;
                if (reinject) { adaptive->observer_reinject_bytes += packet_size; }
                for (unsigned i = 0; i < XQC_MAX_PATHS_COUNT; ++i) {
                    if (adaptive->states[i].in_use && adaptive->states[i].path_id == original_path->path_id) {
                        adaptive->states[i].observer_reason_count[XQC_ADAPTIVE_PICK_ORIGINAL]++;
                        adaptive->states[i].observer_reason_bytes[XQC_ADAPTIVE_PICK_ORIGINAL] += packet_size;
                        break;
                    }
                }
            }
            return xqc_adaptive_diag_return(adaptive, conn, packet_out,
                original_path, XQC_FALSE, now);
        }
        return xqc_adaptive_diag_return(adaptive, conn, packet_out, NULL,
            has_active && !has_sendable, now);
    }

    if (count == 1) {
        if (adaptive->diag_start_us == 0) {
            xqc_adaptive_diag_reset(adaptive, candidates, count, now);
        }
        if (!candidates[0].can_send) {
            if (original_path != NULL && packet_out
                && !(packet_out->po_flag & XQC_POF_REINJECT_DIFF_PATH))
            {
                xqc_adaptive_diag_record_pick(adaptive, &candidates[0],
                    packet_size, XQC_ADAPTIVE_PICK_ORIGINAL, reinject);
                return xqc_adaptive_diag_return(adaptive, conn, packet_out,
                    original_path, XQC_FALSE, now);
            }
            adaptive->no_sendable_count++;
            if (cc_blocked) {
                *cc_blocked = XQC_TRUE;
            }
            return xqc_adaptive_diag_return(adaptive, conn, packet_out, NULL,
                XQC_TRUE, now);
        }
        if (adaptive->phase == XQC_ADAPTIVE_PHASE_BULK
            && (!adaptive->primary_valid
                || adaptive->primary_path_id != candidates[0].path_id))
        {
            xqc_bool_t primary_changed = adaptive->primary_valid
                && adaptive->primary_path_id != candidates[0].path_id;
            adaptive->primary_valid = XQC_TRUE;
            adaptive->primary_path_id = candidates[0].path_id;
            adaptive->primary_since_us = now;
            adaptive->challenger_path_id = UINT64_MAX;
            adaptive->challenger_streak = 0;
            adaptive->probe_pct = XQC_ADAPTIVE_PROBE_MIN_PCT;
            adaptive->primary_baseline_rate = 0;
            adaptive->probe_trial_active = XQC_FALSE;
            adaptive->probe_before_trial = adaptive->probe_pct;
            adaptive->probe_trial_reference_rate = 0;
            adaptive->probe_cooldown = 0;
            adaptive->adjust_start_us = now;
            adaptive->primary_select_count++;
            if (primary_changed) {
                adaptive->primary_switch_count++;
            }
            adaptive->min_probe_pct = xqc_min(adaptive->min_probe_pct,
                                              adaptive->probe_pct);
            candidates[0].state->adjust_sent_base = candidates[0].sent_total;
            candidates[0].state->adjust_delivered_base =
                candidates[0].delivered_total;
            printf("[diag_adaptive] conn=%p event=failover_single primary=%" PRIu64 "\n",
                   (void *)conn, adaptive->primary_path_id);
            xqc_adaptive_diag_print_primary_paths(adaptive, candidates, 1,
                now, "failover_single");
        }
        xqc_adaptive_diag_record_pick(adaptive, &candidates[0], packet_size,
            XQC_ADAPTIVE_PICK_SINGLE, reinject);
        return xqc_adaptive_diag_return(adaptive, conn, packet_out,
            candidates[0].path, XQC_FALSE, now);
    }

    if (adaptive->phase == XQC_ADAPTIVE_PHASE_WAIT_PATHS) {
        xqc_adaptive_start_warmup(adaptive, conn, candidates, count, now);
    }

    xqc_adaptive_candidate_t *selected = NULL;
    if (adaptive->phase == XQC_ADAPTIVE_PHASE_WARMUP) {
        xqc_usec_t elapsed = now - adaptive->phase_start_us;
        if (xqc_adaptive_warmup_complete(candidates, count, elapsed)) {
            adaptive->warmup_elapsed_us = elapsed;
            for (size_t i = 0; i < count; i++) {
                candidates[i].state->warmup_sent_final =
                    candidates[i].warmup_sent;
                candidates[i].state->warmup_delivered_final =
                    xqc_adaptive_delta_u64(candidates[i].delivered_total,
                        candidates[i].state->init_delivered);
            }
            xqc_adaptive_select_primary(adaptive, conn, candidates, count,
                                        now, "warmup_complete");
            adaptive->phase = XQC_ADAPTIVE_PHASE_BULK;
        } else {
            selected = xqc_adaptive_pick_warmup(candidates, count);
            if (selected != NULL) {
                pick_reason = XQC_ADAPTIVE_PICK_WARMUP;
            }
        }
    }

    if (adaptive->phase == XQC_ADAPTIVE_PHASE_BULK) {
        xqc_adaptive_candidate_t *primary = xqc_adaptive_find_candidate(
            candidates, count, adaptive->primary_path_id);
        if (primary == NULL) {
            xqc_adaptive_select_primary(adaptive, conn, candidates, count,
                                        now, "path_failover");
        }

        uint64_t window_total = 0;
        for (size_t i = 0; i < count; i++) {
            window_total += candidates[i].window_sent;
        }
        if (adaptive->window_start_us == 0
            || now - adaptive->window_start_us >= XQC_ADAPTIVE_WINDOW_MAX_US
            || window_total >= XQC_ADAPTIVE_WINDOW_MAX_BYTES)
        {
            xqc_adaptive_reset_window(adaptive, candidates, count, now);
        }

        xqc_adaptive_adjust(adaptive, conn, candidates, count, now);
        selected = xqc_adaptive_pick_bulk(adaptive, candidates, count,
            packet_size, &intentionally_blocked, &pick_reason);
    }

    if (selected == NULL && intentionally_blocked) {
        adaptive->no_sendable_count++;
        if (cc_blocked) {
            *cc_blocked = XQC_TRUE;
        }
        return xqc_adaptive_diag_return(adaptive, conn, packet_out, NULL,
            XQC_TRUE, now);
    }
    if (selected == NULL) {
        selected = xqc_adaptive_best_candidate(candidates, count, XQC_TRUE);
        if (selected != NULL) {
            pick_reason = XQC_ADAPTIVE_PICK_BEST_FALLBACK;
        }
    }
    if (selected == NULL && original_path != NULL && packet_out
        && !(packet_out->po_flag & XQC_POF_REINJECT_DIFF_PATH))
    {
        xqc_adaptive_candidate_t *original = xqc_adaptive_find_candidate(
            candidates, count, original_path->path_id);
        xqc_adaptive_diag_record_pick(adaptive, original, packet_size,
            XQC_ADAPTIVE_PICK_ORIGINAL, reinject);
        return xqc_adaptive_diag_return(adaptive, conn, packet_out,
            original_path, XQC_FALSE, now);
    }

    if (selected != NULL) {
        xqc_adaptive_diag_record_pick(adaptive, selected, packet_size,
            pick_reason, reinject);
        return xqc_adaptive_diag_return(adaptive, conn, packet_out,
            selected->path, XQC_FALSE, now);
    }
    if (has_active && !has_sendable) {
        adaptive->no_sendable_count++;
    }
    if (cc_blocked) {
        *cc_blocked = has_active && !has_sendable;
    }
    return xqc_adaptive_diag_return(adaptive, conn, packet_out, NULL,
        has_active && !has_sendable, now);
}

static const char *
xqc_adaptive_phase_name(xqc_adaptive_phase_t phase)
{
    switch (phase) {
    case XQC_ADAPTIVE_PHASE_WAIT_PATHS:
        return "wait_paths";
    case XQC_ADAPTIVE_PHASE_WARMUP:
        return "warmup";
    case XQC_ADAPTIVE_PHASE_BULK:
        return "bulk";
    default:
        return "unknown";
    }
}

static void
xqc_adaptive_scheduler_conn_close_diag(void *scheduler,
    xqc_connection_t *conn)
{
    xqc_adaptive_scheduler_t *adaptive = scheduler;
    if (adaptive == NULL || conn == NULL || adaptive->diag_emitted)
    {
        return;
    }
    adaptive->diag_emitted = XQC_TRUE;

    if (adaptive->sched_diag.enabled || adaptive->observer_enabled) {
        printf("[diag_adaptive_probe] both_sendable=%" PRIu64
               " primary_only=%" PRIu64 " secondary_only=%" PRIu64
               " neither_sendable=%" PRIu64
               " probe_considered=%" PRIu64 " probe_selected=%" PRIu64
               " probe_denied_secondary_cwnd=%" PRIu64
               " probe_denied_secondary_reinject=%" PRIu64
               " probe_denied_secondary_other=%" PRIu64
               " probe_denied_share_limit=%" PRIu64
               " probe_denied_no_secondary=%" PRIu64
               " fallback_real_inflight=%" PRIu64
               " fallback_scheduled_reservation=%" PRIu64 "\n",
               adaptive->both_sendable, adaptive->primary_only,
               adaptive->secondary_only, adaptive->neither_sendable,
               adaptive->probe_considered, adaptive->probe_selected,
               adaptive->probe_denied_secondary_cwnd,
               adaptive->probe_denied_secondary_reinject,
               adaptive->probe_denied_secondary_other,
               adaptive->probe_denied_share_limit,
               adaptive->probe_denied_no_secondary,
               adaptive->fallback_real_inflight,
               adaptive->fallback_scheduled_reservation);
        xqc_scheduler_diag_close(&adaptive->sched_diag, conn);
    }

    xqc_usec_t now = xqc_monotonic_timestamp();
    xqc_usec_t start_us = adaptive->diag_start_us > 0
        ? adaptive->diag_start_us : conn->conn_create_time;
    xqc_usec_t end_us = 0;
    uint64_t total_selected = 0;
    uint64_t total_app_sent = 0;
    uint64_t total_wire_sent = 0;
    uint64_t total_delivered = 0;
    uint64_t total_reinject = 0;
    uint64_t total_retrans = 0;
    uint64_t total_lost = 0;
    xqc_list_head_t *pos, *next;

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        xqc_path_ctx_t *path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        xqc_send_ctl_t *ctl = path->path_send_ctl;
        if (ctl == NULL) {
            continue;
        }
        xqc_adaptive_path_state_t *state =
            xqc_adaptive_find_state(adaptive, path->path_id);
        uint64_t sent_base = state && state->diag_base_valid
            ? state->diag_sent_base : 0;
        uint64_t app_sent_base = state && state->diag_base_valid
            ? state->diag_app_sent_base : 0;
        uint64_t delivered_base = state && state->diag_base_valid
            ? state->diag_delivered_base : 0;
        uint64_t reinject_base = state && state->diag_base_valid
            ? state->diag_reinject_base : 0;
        uint64_t retrans_base = state && state->diag_base_valid
            ? state->diag_retrans_base : 0;
        unsigned lost_base = state && state->diag_base_valid
            ? state->diag_lost_base : 0;

        total_selected += state ? state->selected_bytes : 0;
        total_app_sent += xqc_adaptive_delta_u64(
            ctl->ctl_app_bytes_send, app_sent_base);
        total_wire_sent += xqc_adaptive_delta_u64(
            ctl->ctl_bytes_send, sent_base);
        total_delivered += xqc_adaptive_delta_u64(
            ctl->ctl_delivered, delivered_base);
        total_reinject += xqc_adaptive_delta_u64(
            ctl->ctl_reinjected_bytes_send, reinject_base);
        total_retrans += xqc_adaptive_delta_u64(
            ctl->ctl_retrans_bytes_send, retrans_base);
        total_lost += ctl->ctl_lost_count >= lost_base
            ? ctl->ctl_lost_count - lost_base : 0;
        if (ctl->ctl_delivered_time > end_us) {
            end_us = ctl->ctl_delivered_time;
        }
    }

    if (end_us <= start_us) {
        end_us = now;
    }
    uint64_t duration_us = end_us > start_us ? end_us - start_us : 1;
    double duration_ms = (double)duration_us / 1000.0;
    double delivered_mbps = (double)total_delivered * 8.0
                          / (double)duration_us;

    printf("[diag_adaptive_summary] event=conn_close phase=%s duration_ms=%.3f"
           " primary=%" PRIu64 " final_probe_pct=%.1f min_probe_pct=%.1f"
           " max_probe_pct=%.1f picks_total=%" PRIu64
           " picks_primary=%" PRIu64 " picks_probe=%" PRIu64
           " picks_cwnd_fallback=%" PRIu64
           " picks_reinject_fallback=%" PRIu64
           " picks_warmup=%" PRIu64
           " picks_single=%" PRIu64 " picks_best_fallback=%" PRIu64
           " picks_original=%" PRIu64 " picks_reinject=%" PRIu64
           " no_sendable_calls=%" PRIu64 " primary_selects=%" PRIu64
           " primary_switches=%" PRIu64 " adjustments=%" PRIu64
           " probe_up=%" PRIu64 " probe_down=%" PRIu64
           " trial_keep=%" PRIu64 " trial_revert=%" PRIu64
           " primary_queue_shift=%" PRIu64
           " secondary_queue_reduce=%" PRIu64 "\n",
           xqc_adaptive_phase_name(adaptive->phase), duration_ms,
           adaptive->primary_valid ? adaptive->primary_path_id : UINT64_MAX,
           adaptive->probe_pct, adaptive->min_probe_pct,
           adaptive->max_probe_pct, adaptive->pick_count,
           adaptive->primary_pick_count, adaptive->probe_pick_count,
           adaptive->cwnd_fallback_pick_count,
           adaptive->reinject_fallback_pick_count,
           adaptive->warmup_pick_count,
           adaptive->single_pick_count, adaptive->best_fallback_pick_count,
           adaptive->original_pick_count, adaptive->reinject_pick_count,
           adaptive->no_sendable_count, adaptive->primary_select_count,
           adaptive->primary_switch_count, adaptive->adjust_count,
           adaptive->probe_increase_count, adaptive->probe_decrease_count,
           adaptive->trial_keep_count, adaptive->trial_revert_count,
           adaptive->primary_queue_shift_count,
           adaptive->secondary_queue_reduce_count);

    printf("[diag_adaptive_totals] duration_ms=%.3f selected_bytes=%" PRIu64
           " app_sent_bytes=%" PRIu64 " wire_sent_bytes=%" PRIu64
           " delivered_bytes=%" PRIu64 " delivered_mbps=%.3f"
           " reinject_bytes=%" PRIu64 " retrans_bytes=%" PRIu64
           " lost_pkts=%" PRIu64 "\n",
           duration_ms, total_selected, total_app_sent, total_wire_sent,
           total_delivered, delivered_mbps, total_reinject, total_retrans,
           total_lost);

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        xqc_path_ctx_t *path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        xqc_send_ctl_t *ctl = path->path_send_ctl;
        if (ctl == NULL) {
            continue;
        }
        xqc_adaptive_path_state_t *state =
            xqc_adaptive_find_state(adaptive, path->path_id);
        uint64_t sent_base = state && state->diag_base_valid
            ? state->diag_sent_base : 0;
        uint64_t app_sent_base = state && state->diag_base_valid
            ? state->diag_app_sent_base : 0;
        uint64_t delivered_base = state && state->diag_base_valid
            ? state->diag_delivered_base : 0;
        uint64_t reinject_base = state && state->diag_base_valid
            ? state->diag_reinject_base : 0;
        uint64_t retrans_base = state && state->diag_base_valid
            ? state->diag_retrans_base : 0;
        unsigned lost_base = state && state->diag_base_valid
            ? state->diag_lost_base : 0;
        uint64_t selected_bytes = state ? state->selected_bytes : 0;
        uint64_t app_sent = xqc_adaptive_delta_u64(
            ctl->ctl_app_bytes_send, app_sent_base);
        uint64_t wire_sent = xqc_adaptive_delta_u64(
            ctl->ctl_bytes_send, sent_base);
        uint64_t delivered = xqc_adaptive_delta_u64(
            ctl->ctl_delivered, delivered_base);
        uint64_t reinject_bytes = xqc_adaptive_delta_u64(
            ctl->ctl_reinjected_bytes_send, reinject_base);
        uint64_t retrans_bytes = xqc_adaptive_delta_u64(
            ctl->ctl_retrans_bytes_send, retrans_base);
        uint64_t lost_pkts = ctl->ctl_lost_count >= lost_base
            ? ctl->ctl_lost_count - lost_base : 0;
        uint64_t srtt = xqc_send_ctl_get_srtt(ctl);
        uint64_t min_srtt = state && state->min_srtt != UINT64_MAX
            ? state->min_srtt : 0;
        double queue_ratio = min_srtt > 0
            ? (double)srtt / (double)min_srtt : 0.0;
        uint64_t cwnd = 0;
        if (ctl->ctl_cong_callback
            && ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd)
        {
            cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(
                ctl->ctl_cong);
        }
        double selected_share = total_selected > 0
            ? (double)selected_bytes * 100.0 / (double)total_selected : 0.0;
        double delivered_share = total_delivered > 0
            ? (double)delivered * 100.0 / (double)total_delivered : 0.0;
        double path_delivered_mbps = (double)delivered * 8.0
                                   / (double)duration_us;

        printf("[diag_adaptive_path] id=%" PRIu64
               " selected_picks=%" PRIu64 " selected_bytes=%" PRIu64
               " selected_share=%.2f%%"
               " app_sent_bytes=%" PRIu64 " wire_sent_bytes=%" PRIu64
               " delivered_bytes=%" PRIu64 " delivered_share=%.2f%%"
               " delivered_mbps=%.3f reinject_bytes=%" PRIu64
               " retrans_bytes=%" PRIu64 " lost_pkts=%" PRIu64
               " blocked_checks=%" PRIu64 " srtt_us=%" PRIu64
               " min_srtt_us=%" PRIu64 " queue_ratio=%.3f"
               " cwnd=%" PRIu64 " inflight=%u est_bw_Bps=%" PRIu64
               " delivery_ema_Bps=%.0f capacity_ema_Bps=%.0f score=%.0f"
               " picks_primary=%" PRIu64 " picks_probe=%" PRIu64
               " picks_cwnd_fallback=%" PRIu64
               " picks_reinject_fallback=%" PRIu64
               " picks_warmup=%" PRIu64
               " picks_single=%" PRIu64 " picks_reinject=%" PRIu64
               " reinject_selected_bytes=%" PRIu64 "\n",
               path->path_id, state ? state->selected_count : 0,
               selected_bytes, selected_share,
               app_sent, wire_sent, delivered, delivered_share,
               path_delivered_mbps, reinject_bytes, retrans_bytes,
               lost_pkts, state ? state->blocked_count : 0,
               srtt, min_srtt, queue_ratio, cwnd, ctl->ctl_bytes_in_flight,
               xqc_send_ctl_get_est_bw(ctl),
               state ? state->delivery_rate_ema : 0.0,
               state ? state->capacity_ema : 0.0,
               state ? state->score : 0.0,
               state ? state->primary_pick_count : 0,
               state ? state->probe_pick_count : 0,
               state ? state->cwnd_fallback_pick_count : 0,
               state ? state->reinject_fallback_pick_count : 0,
               state ? state->warmup_pick_count : 0,
               state ? state->single_pick_count : 0,
               state ? state->reinject_pick_count : 0,
               state ? state->reinject_selected_bytes : 0);
    }
}

const xqc_scheduler_callback_t xqc_adaptive_scheduler_cb = {
    .xqc_scheduler_size             = xqc_adaptive_scheduler_size,
    .xqc_scheduler_init             = xqc_adaptive_scheduler_init,
    .xqc_scheduler_get_path         = xqc_adaptive_scheduler_get_path,
    .xqc_scheduler_conn_close_diag  = xqc_adaptive_scheduler_conn_close_diag,
};

/* Pure diagnostic export; does not refresh candidates, rates or EMA. */
int
xqc_adaptive_observer_json(xqc_connection_t *conn, char *out, size_t cap)
{
    if (!conn->scheduler_callback || conn->scheduler_callback->xqc_scheduler_get_path != xqc_adaptive_scheduler_cb.xqc_scheduler_get_path || !conn->scheduler) { return 0; }
    const xqc_adaptive_scheduler_t *a = conn->scheduler;
    const char *names[] = {"none", "warmup", "primary", "probe", "cwnd_fallback",
        "reinject_fallback", "best_fallback", "single", "original"};
    char primary[32] = "null", secondary[32] = "null";
    if (a->primary_valid) { snprintf(primary, sizeof(primary), "%"PRIu64, a->primary_path_id); }
    if (a->primary_valid && a->observer_secondary_id != UINT64_MAX) {
        snprintf(secondary, sizeof(secondary), "%"PRIu64, a->observer_secondary_id);
    }
    int n = snprintf(out, cap,
        "\"scheduler_name\":\"adaptive\",\"adaptive_phase\":\"%s\","
        "\"primary_valid\":%s,\"primary_path_id\":%s,\"secondary_path_id\":%s,"
        "\"target_secondary_pct\":%.6f,\"role_epoch\":%"PRIu64","
        "\"both_sendable\":%"PRIu64",\"primary_only\":%"PRIu64","
        "\"secondary_only\":%"PRIu64",\"neither_sendable\":%"PRIu64","
        "\"no_sendable_calls\":%"PRIu64",\"fallback_real_inflight_count\":%"PRIu64","
        "\"fallback_scheduled_reservation_count\":%"PRIu64","
        "\"fallback_real_inflight_bytes\":%"PRIu64",\"fallback_scheduled_reservation_bytes\":%"PRIu64","
        "\"reinject_selected_bytes\":%"PRIu64,
        xqc_adaptive_phase_name(a->phase), a->primary_valid ? "true" : "false", primary, secondary,
        a->probe_pct, conn->observer ? conn->observer->role_epoch : 0,
        a->both_sendable, a->primary_only, a->secondary_only, a->neither_sendable,
        a->observer_no_sendable_calls, a->fallback_real_inflight, a->fallback_scheduled_reservation,
        a->observer_real_bytes, a->observer_reserved_bytes, a->observer_reinject_bytes);
    if (n < 0 || (size_t)n >= cap) { return 0; }
    size_t used = (size_t)n;
    for (unsigned i = 1; i < 9; ++i) {
        n = snprintf(out + used, cap - used,
            ",\"%s_selected_count\":%"PRIu64",\"%s_selected_bytes\":%"PRIu64,
            names[i], a->observer_reason_count[i], names[i], a->observer_reason_bytes[i]);
        if (n < 0 || (size_t)n >= cap - used) { return 0; }
        used += (size_t)n;
    }
    return 1;
}

int
xqc_adaptive_observer_path_json(xqc_connection_t *conn, uint64_t id, char *out, size_t cap)
{
    if (!conn->scheduler_callback || conn->scheduler_callback->xqc_scheduler_get_path != xqc_adaptive_scheduler_cb.xqc_scheduler_get_path || !conn->scheduler) { return 0; }
    const xqc_adaptive_scheduler_t *a = conn->scheduler;
    const xqc_adaptive_path_state_t *state = NULL;
    for (unsigned i = 0; i < XQC_MAX_PATHS_COUNT; ++i) {
        if (a->states[i].in_use && a->states[i].path_id == id) { state = &a->states[i]; break; }
    }
    if (!state) { return 0; }
    const char *names[] = {"none", "warmup", "primary", "probe", "cwnd_fallback",
        "reinject_fallback", "best_fallback", "single", "original"};
    int initial = snprintf(out, cap,
        ",\"last_evaluation_us\":%"PRIu64",\"evaluated_packet_bytes\":%"PRIu64","
        "\"evaluation_block_reason\":%d,\"evaluation_check_cwnd\":%d,\"evaluation_reinject_excluded\":%d",
        state->observer_evaluation_us, state->observer_packet_bytes, state->observer_block_reason,
        state->observer_check_cwnd, state->observer_reinject_excluded);
    if (initial < 0 || (size_t)initial >= cap) { return 0; }
    size_t used = (size_t)initial;
    for (unsigned i = 1; i < 9; ++i) {
        int n = snprintf(out + used, cap - used,
            ",\"path_%s_selected_count\":%"PRIu64",\"path_%s_selected_bytes\":%"PRIu64,
            names[i], state->observer_reason_count[i], names[i], state->observer_reason_bytes[i]);
        if (n < 0 || (size_t)n >= cap - used) { return 0; }
        used += (size_t)n;
    }
    return 1;
}
