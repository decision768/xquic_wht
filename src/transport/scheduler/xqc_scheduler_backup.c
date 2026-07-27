/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */


#include "src/transport/scheduler/xqc_scheduler_backup.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_send_ctl.h"
#include "src/common/xqc_time.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>


#define XQC_BACKUP_WARMUP_BYTES_PER_PATH      (256 * 1024)
#define XQC_BACKUP_WARMUP_DELIVERED_MIN       (128 * 1024)
#define XQC_BACKUP_WARMUP_MIN_US              500000
#define XQC_BACKUP_WARMUP_MAX_US              1200000
#define XQC_BACKUP_RATE_EMA_ALPHA              0.25
#define XQC_BACKUP_SCORE_TIE_RATIO             1.05

typedef struct xqc_backup_path_sample_s {
    xqc_bool_t in_use;
    uint64_t path_id;
    uint64_t delivered_base;
    uint64_t delivered_bytes;
    uint64_t last_delivered;
    xqc_usec_t last_delivered_time;
    uint64_t selected_bytes;
    uint64_t selected_picks;
    uint64_t blocked_checks;
    uint64_t est_bw_Bps;
    uint64_t srtt_us;
    uint64_t cwnd;
    uint64_t inflight;
    double delivery_rate_ema_Bps;
    double window_rate_Bps;
    double score_Bps;
} xqc_backup_path_sample_t;

typedef struct xqc_backup_candidate_s {
    xqc_path_ctx_t *path;
    xqc_backup_path_sample_t *sample;
    xqc_bool_t can_send;
    double fill_ratio;
} xqc_backup_candidate_t;

typedef struct xqc_backup_scheduler_s {
    xqc_backup_path_sample_t samples[XQC_MAX_PATHS_COUNT];
    xqc_bool_t diag_enabled;
    xqc_bool_t warmup_started;
    xqc_bool_t warmup_completed;
    xqc_bool_t warmup_timed_out;
    xqc_bool_t primary_locked;
    xqc_bool_t failover_active;
    xqc_usec_t warmup_start_us;
    xqc_usec_t warmup_elapsed_us;
    uint64_t primary_path_id;
    uint64_t primary_picks;
    uint64_t primary_selected_bytes;
    uint64_t primary_cwnd_waits;
    uint64_t primary_reinject_waits;
    uint64_t prelock_picks;
    uint64_t prelock_selected_bytes;
    uint64_t warmup_picks;
    uint64_t warmup_selected_bytes;
    uint64_t failover_events;
    uint64_t failover_picks;
    uint64_t failover_selected_bytes;
    uint64_t no_path_returns;
} xqc_backup_scheduler_t;


static size_t
xqc_backup_scheduler_size(void)
{
    return sizeof(xqc_backup_scheduler_t);
}


static void
xqc_backup_scheduler_init(void *scheduler, xqc_log_t *log,
    xqc_scheduler_params_t *param)
{
    xqc_backup_scheduler_t *backup = scheduler;

    (void)log;
    memset(backup, 0, sizeof(*backup));
    backup->diag_enabled = param && param->mp_diag_enabled;
    backup->primary_path_id = UINT64_MAX;
}


static xqc_bool_t
xqc_backup_scheduler_path_is_usable(xqc_path_ctx_t *path)
{
    return path != NULL
           && path->path_state == XQC_PATH_STATE_ACTIVE
           && path->app_path_status != XQC_APP_PATH_STATUS_FROZEN;
}


static size_t
xqc_backup_scheduler_usable_path_count(xqc_connection_t *conn)
{
    size_t count = 0;
    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        if (xqc_backup_scheduler_path_is_usable(path)) {
            count++;
        }
    }

    return count;
}


static xqc_path_ctx_t *
xqc_backup_scheduler_find_path(xqc_connection_t *conn, uint64_t path_id)
{
    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        if (path->path_id == path_id) {
            return path;
        }
    }

    return NULL;
}


static xqc_backup_path_sample_t *
xqc_backup_scheduler_find_sample(xqc_backup_scheduler_t *backup,
    uint64_t path_id)
{
    size_t i;

    for (i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (backup->samples[i].in_use
            && backup->samples[i].path_id == path_id)
        {
            return &backup->samples[i];
        }
    }

    return NULL;
}


static xqc_backup_path_sample_t *
xqc_backup_scheduler_add_sample(xqc_backup_scheduler_t *backup,
    xqc_path_ctx_t *path, xqc_usec_t now)
{
    size_t i;
    xqc_send_ctl_t *ctl;
    xqc_backup_path_sample_t *sample;

    sample = xqc_backup_scheduler_find_sample(backup, path->path_id);
    if (sample != NULL) {
        return sample;
    }

    ctl = path->path_send_ctl;
    if (ctl == NULL) {
        return NULL;
    }

    for (i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        sample = &backup->samples[i];
        if (!sample->in_use) {
            memset(sample, 0, sizeof(*sample));
            sample->in_use = XQC_TRUE;
            sample->path_id = path->path_id;
            sample->delivered_base = ctl->ctl_delivered;
            sample->last_delivered = ctl->ctl_delivered;
            sample->last_delivered_time = ctl->ctl_delivered_time > 0
                ? ctl->ctl_delivered_time : now;
            return sample;
        }
    }

    return NULL;
}


static void
xqc_backup_scheduler_start_warmup(xqc_backup_scheduler_t *backup,
    xqc_connection_t *conn, xqc_usec_t now)
{
    size_t path_count = 0;
    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;

    memset(backup->samples, 0, sizeof(backup->samples));
    backup->warmup_started = XQC_TRUE;
    backup->warmup_completed = XQC_FALSE;
    backup->warmup_timed_out = XQC_FALSE;
    backup->warmup_start_us = now;
    backup->warmup_elapsed_us = 0;

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        if (!xqc_backup_scheduler_path_is_usable(path)) {
            continue;
        }
        if (xqc_backup_scheduler_add_sample(backup, path, now) != NULL) {
            path_count++;
        }
    }

    if (backup->diag_enabled) {
        printf("[diag_strict_backup] conn=%p event=warmup_start paths=%zu"
               " min_ms=%.3f max_ms=%.3f bytes_per_path=%u"
               " delivered_min=%u\n",
               conn, path_count,
               (double)XQC_BACKUP_WARMUP_MIN_US / 1000.0,
               (double)XQC_BACKUP_WARMUP_MAX_US / 1000.0,
               (unsigned)XQC_BACKUP_WARMUP_BYTES_PER_PATH,
               (unsigned)XQC_BACKUP_WARMUP_DELIVERED_MIN);
    }
}


static void
xqc_backup_scheduler_refresh_sample(xqc_backup_path_sample_t *sample,
    xqc_path_ctx_t *path, xqc_usec_t now)
{
    xqc_send_ctl_t *ctl = path->path_send_ctl;
    xqc_usec_t delivered_time;

    if (ctl == NULL) {
        return;
    }

    sample->delivered_bytes = ctl->ctl_delivered >= sample->delivered_base
        ? ctl->ctl_delivered - sample->delivered_base : 0;
    sample->est_bw_Bps = xqc_send_ctl_get_est_bw(ctl);
    sample->srtt_us = xqc_send_ctl_get_srtt(ctl);
    sample->inflight = ctl->ctl_bytes_in_flight + path->path_schedule_bytes;
    sample->cwnd = 0;
    if (ctl->ctl_cong_callback
        && ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd)
    {
        sample->cwnd = ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(
            ctl->ctl_cong);
    }

    delivered_time = ctl->ctl_delivered_time > 0
        ? ctl->ctl_delivered_time : now;
    if (ctl->ctl_delivered > sample->last_delivered
        && delivered_time > sample->last_delivered_time)
    {
        double rate =
            (double)(ctl->ctl_delivered - sample->last_delivered) * 1000000.0
            / (double)(delivered_time - sample->last_delivered_time);
        if (sample->delivery_rate_ema_Bps <= 0) {
            sample->delivery_rate_ema_Bps = rate;

        } else {
            sample->delivery_rate_ema_Bps =
                (1.0 - XQC_BACKUP_RATE_EMA_ALPHA)
                * sample->delivery_rate_ema_Bps
                + XQC_BACKUP_RATE_EMA_ALPHA * rate;
        }
        sample->last_delivered = ctl->ctl_delivered;
        sample->last_delivered_time = delivered_time;
    }
}


static size_t
xqc_backup_scheduler_collect_warmup_candidates(
    xqc_backup_scheduler_t *backup, xqc_connection_t *conn,
    xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_backup_candidate_t *candidates, xqc_usec_t now,
    xqc_bool_t *has_cwnd_sendable)
{
    size_t count = 0;
    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;

    *has_cwnd_sendable = XQC_FALSE;
    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        xqc_backup_path_sample_t *sample;
        xqc_backup_candidate_t *candidate;
        xqc_bool_t cwnd_can_send;

        path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        if (!xqc_backup_scheduler_path_is_usable(path)
            || count >= XQC_MAX_PATHS_COUNT)
        {
            continue;
        }

        sample = xqc_backup_scheduler_add_sample(backup, path, now);
        if (sample == NULL || path->path_send_ctl == NULL) {
            continue;
        }
        xqc_backup_scheduler_refresh_sample(sample, path, now);

        candidate = &candidates[count++];
        memset(candidate, 0, sizeof(*candidate));
        candidate->path = path;
        candidate->sample = sample;
        cwnd_can_send = xqc_scheduler_check_path_can_send(
            path, packet_out, check_cwnd);
        candidate->can_send = cwnd_can_send;
        if (cwnd_can_send) {
            *has_cwnd_sendable = XQC_TRUE;

        } else {
            sample->blocked_checks++;
        }
        if (reinject && packet_out->po_path_id == path->path_id) {
            candidate->can_send = XQC_FALSE;
        }

        candidate->fill_ratio = sample->cwnd > 0
            ? (double)sample->inflight / (double)sample->cwnd : 1.0;
    }

    return count;
}


static xqc_bool_t
xqc_backup_scheduler_warmup_complete(xqc_backup_scheduler_t *backup,
    xqc_backup_candidate_t *candidates, size_t count, xqc_usec_t now)
{
    size_t i;
    xqc_bool_t all_selected = XQC_TRUE;
    xqc_bool_t all_delivered = XQC_TRUE;
    xqc_usec_t elapsed = now - backup->warmup_start_us;

    for (i = 0; i < count; i++) {
        if (candidates[i].sample->selected_bytes
            < XQC_BACKUP_WARMUP_BYTES_PER_PATH)
        {
            all_selected = XQC_FALSE;
        }
        if (candidates[i].sample->delivered_bytes
            < XQC_BACKUP_WARMUP_DELIVERED_MIN)
        {
            all_delivered = XQC_FALSE;
        }
    }

    if (elapsed >= XQC_BACKUP_WARMUP_MAX_US) {
        backup->warmup_timed_out = XQC_TRUE;
        return XQC_TRUE;
    }

    return elapsed >= XQC_BACKUP_WARMUP_MIN_US
           && all_selected && all_delivered;
}


static xqc_backup_candidate_t *
xqc_backup_scheduler_pick_warmup(xqc_backup_candidate_t *candidates,
    size_t count)
{
    size_t i;
    xqc_bool_t need_minimum = XQC_FALSE;
    xqc_backup_candidate_t *selected = NULL;

    for (i = 0; i < count; i++) {
        if (candidates[i].can_send
            && candidates[i].sample->selected_bytes
                < XQC_BACKUP_WARMUP_BYTES_PER_PATH)
        {
            need_minimum = XQC_TRUE;
            break;
        }
    }

    for (i = 0; i < count; i++) {
        xqc_backup_candidate_t *candidate = &candidates[i];
        if (!candidate->can_send) {
            continue;
        }
        if (need_minimum
            && candidate->sample->selected_bytes
                >= XQC_BACKUP_WARMUP_BYTES_PER_PATH)
        {
            continue;
        }

        if (selected == NULL
            || candidate->fill_ratio < selected->fill_ratio
            || (candidate->fill_ratio == selected->fill_ratio
                && candidate->sample->selected_bytes
                    < selected->sample->selected_bytes)
            || (candidate->fill_ratio == selected->fill_ratio
                && candidate->sample->selected_bytes
                    == selected->sample->selected_bytes
                && candidate->path->path_id < selected->path->path_id))
        {
            selected = candidate;
        }
    }

    return selected;
}


static void
xqc_backup_scheduler_lock_primary(xqc_backup_scheduler_t *backup,
    xqc_connection_t *conn, xqc_backup_candidate_t *candidates,
    size_t count, xqc_usec_t now)
{
    size_t i;
    xqc_backup_candidate_t *best = NULL;
    xqc_usec_t elapsed = now - backup->warmup_start_us;

    backup->warmup_elapsed_us = elapsed;
    for (i = 0; i < count; i++) {
        xqc_backup_path_sample_t *sample = candidates[i].sample;
        double est_component;

        sample->window_rate_Bps = elapsed > 0
            ? (double)sample->delivered_bytes * 1000000.0 / (double)elapsed
            : 0;
        est_component = (double)sample->est_bw_Bps;
        if (sample->window_rate_Bps > 0
            && est_component > 2.0 * sample->window_rate_Bps)
        {
            est_component = 2.0 * sample->window_rate_Bps;
        }
        sample->score_Bps = sample->window_rate_Bps > 0
            ? 0.8 * sample->window_rate_Bps + 0.2 * est_component
            : est_component;

        if (backup->diag_enabled) {
            printf("[diag_strict_backup_probe] conn=%p path=%" PRIu64
                   " selected_bytes=%" PRIu64 " delivered_bytes=%" PRIu64
                   " delivery_mbps=%.3f ema_mbps=%.3f est_bw_mbps=%.3f"
                   " score_mbps=%.3f srtt_us=%" PRIu64
                   " cwnd=%" PRIu64 " inflight=%" PRIu64
                   " blocked_checks=%" PRIu64 "\n",
                   conn, sample->path_id, sample->selected_bytes,
                   sample->delivered_bytes,
                   sample->window_rate_Bps * 8.0 / 1000000.0,
                   sample->delivery_rate_ema_Bps * 8.0 / 1000000.0,
                   (double)sample->est_bw_Bps * 8.0 / 1000000.0,
                   sample->score_Bps * 8.0 / 1000000.0,
                   sample->srtt_us, sample->cwnd, sample->inflight,
                   sample->blocked_checks);
        }

        if (best == NULL
            || sample->score_Bps
                > best->sample->score_Bps * XQC_BACKUP_SCORE_TIE_RATIO
            || (sample->score_Bps * XQC_BACKUP_SCORE_TIE_RATIO
                >= best->sample->score_Bps
                && sample->score_Bps
                    <= best->sample->score_Bps * XQC_BACKUP_SCORE_TIE_RATIO
                && sample->srtt_us < best->sample->srtt_us))
        {
            best = &candidates[i];
        }
    }

    if (best == NULL) {
        return;
    }

    backup->primary_path_id = best->path->path_id;
    backup->primary_locked = XQC_TRUE;
    backup->warmup_started = XQC_FALSE;
    backup->warmup_completed = XQC_TRUE;
    if (backup->diag_enabled) {
        printf("[diag_strict_backup] conn=%p event=primary_lock"
               " mode=auto_probe primary=%" PRIu64
               " elapsed_ms=%.3f reason=%s score_mbps=%.3f\n",
               conn, backup->primary_path_id, (double)elapsed / 1000.0,
               backup->warmup_timed_out ? "timeout" : "samples_ready",
               best->sample->score_Bps * 8.0 / 1000000.0);
    }
}


static xqc_path_ctx_t *
xqc_backup_scheduler_pick_fallback(xqc_backup_scheduler_t *backup,
    xqc_connection_t *conn, xqc_packet_out_t *packet_out, int check_cwnd,
    int reinject, xqc_bool_t *cc_blocked, xqc_bool_t failover)
{
    xqc_path_ctx_t *best_path[XQC_PATH_CLASS_PERF_CLASS_SIZE];
    xqc_bool_t has_path[XQC_PATH_CLASS_PERF_CLASS_SIZE];
    xqc_path_perf_class_t path_class;
    xqc_bool_t available_path_exists = XQC_FALSE;
    xqc_bool_t reached_cwnd_check = XQC_FALSE;
    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;

    for (path_class = XQC_PATH_CLASS_AVAILABLE_HIGH;
         path_class < XQC_PATH_CLASS_PERF_CLASS_SIZE;
         path_class++)
    {
        best_path[path_class] = NULL;
        has_path[path_class] = XQC_FALSE;
    }

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        xqc_bool_t path_can_send = XQC_FALSE;
        uint64_t path_srtt = 0;

        path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        path_class = xqc_path_get_perf_class(path);
        if (!xqc_backup_scheduler_path_is_usable(path)
            || (reinject && packet_out->po_path_id == path->path_id))
        {
            continue;
        }

        reached_cwnd_check = XQC_TRUE;
        has_path[path_class] = XQC_TRUE;
        path_can_send = xqc_scheduler_check_path_can_send(
            path, packet_out, check_cwnd);
        if (!path_can_send) {
            continue;
        }

        path_srtt = xqc_send_ctl_get_srtt(path->path_send_ctl);
        if (best_path[path_class] == NULL
            || path_srtt < best_path[path_class]->path_send_ctl->ctl_srtt)
        {
            best_path[path_class] = path;
        }
    }

    for (path_class = XQC_PATH_CLASS_AVAILABLE_HIGH;
         path_class < XQC_PATH_CLASS_PERF_CLASS_SIZE;
         path_class++)
    {
        if (has_path[path_class] && ((path_class & 1) == 0)) {
            available_path_exists = XQC_TRUE;
        }
        if (best_path[path_class] == NULL) {
            continue;
        }
        if (available_path_exists && (path_class & 1) && !reinject) {
            continue;
        }

        if (failover) {
            backup->failover_picks++;
            backup->failover_selected_bytes += packet_out->po_used_size;

        } else {
            backup->prelock_picks++;
            backup->prelock_selected_bytes += packet_out->po_used_size;
        }
        if (cc_blocked) {
            *cc_blocked = XQC_FALSE;
        }
        return best_path[path_class];
    }

    if (cc_blocked) {
        *cc_blocked = reached_cwnd_check;
    }
    backup->no_path_returns++;
    return NULL;
}


xqc_path_ctx_t *
xqc_backup_scheduler_get_path(void *scheduler, xqc_connection_t *conn,
    xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_bool_t *cc_blocked)
{
    xqc_backup_scheduler_t *backup = scheduler;
    xqc_path_ctx_t *primary_path;
    xqc_usec_t now = xqc_monotonic_timestamp();
    size_t usable_count;

    if (cc_blocked) {
        *cc_blocked = XQC_FALSE;
    }

    if (backup->primary_locked) {
        primary_path = xqc_backup_scheduler_find_path(
            conn, backup->primary_path_id);
        if (xqc_backup_scheduler_path_is_usable(primary_path)) {
            backup->failover_active = XQC_FALSE;

            if (reinject && packet_out->po_path_id == primary_path->path_id) {
                backup->primary_reinject_waits++;
                backup->no_path_returns++;
                return NULL;
            }

            if (!xqc_scheduler_check_path_can_send(
                    primary_path, packet_out, check_cwnd))
            {
                if (cc_blocked) {
                    *cc_blocked = XQC_TRUE;
                }
                backup->primary_cwnd_waits++;
                backup->no_path_returns++;
                return NULL;
            }

            backup->primary_picks++;
            backup->primary_selected_bytes += packet_out->po_used_size;
            return primary_path;
        }

        if (!backup->failover_active) {
            backup->failover_active = XQC_TRUE;
            backup->failover_events++;
            if (backup->diag_enabled) {
                printf("[diag_strict_backup] conn=%p event=primary_failover"
                       " primary=%" PRIu64 " state=%d status=%d\n",
                       conn, backup->primary_path_id,
                       primary_path ? primary_path->path_state : -1,
                       primary_path ? primary_path->app_path_status : -1);
            }
        }

        return xqc_backup_scheduler_pick_fallback(backup, conn, packet_out,
            check_cwnd, reinject, cc_blocked, XQC_TRUE);
    }

    usable_count = xqc_backup_scheduler_usable_path_count(conn);
    if (usable_count >= 2) {
        xqc_backup_candidate_t candidates[XQC_MAX_PATHS_COUNT];
        xqc_backup_candidate_t *selected;
        xqc_bool_t has_cwnd_sendable = XQC_FALSE;
        size_t count;

        if (!backup->warmup_started) {
            xqc_backup_scheduler_start_warmup(backup, conn, now);
        }

        memset(candidates, 0, sizeof(candidates));
        count = xqc_backup_scheduler_collect_warmup_candidates(
            backup, conn, packet_out, check_cwnd, reinject,
            candidates, now, &has_cwnd_sendable);

        if (count >= 2
            && xqc_backup_scheduler_warmup_complete(
                backup, candidates, count, now))
        {
            xqc_backup_scheduler_lock_primary(
                backup, conn, candidates, count, now);
            if (backup->primary_locked) {
                return xqc_backup_scheduler_get_path(scheduler, conn,
                    packet_out, check_cwnd, reinject, cc_blocked);
            }
        }

        selected = xqc_backup_scheduler_pick_warmup(candidates, count);
        if (selected != NULL) {
            selected->sample->selected_picks++;
            selected->sample->selected_bytes += packet_out->po_used_size;
            backup->warmup_picks++;
            backup->warmup_selected_bytes += packet_out->po_used_size;
            backup->prelock_picks++;
            backup->prelock_selected_bytes += packet_out->po_used_size;
            return selected->path;
        }

        if (cc_blocked) {
            *cc_blocked = count > 0 && !has_cwnd_sendable;
        }
        backup->no_path_returns++;
        return NULL;
    }

    if (backup->warmup_started) {
        backup->warmup_started = XQC_FALSE;
        backup->warmup_completed = XQC_FALSE;
        backup->warmup_start_us = 0;
        backup->warmup_elapsed_us = 0;
        memset(backup->samples, 0, sizeof(backup->samples));
        if (backup->diag_enabled) {
            printf("[diag_strict_backup] conn=%p event=warmup_abort"
                   " reason=active_paths_below_two\n", conn);
        }
    }

    return xqc_backup_scheduler_pick_fallback(backup, conn, packet_out,
        check_cwnd, reinject, cc_blocked, XQC_FALSE);
}


static void
xqc_backup_scheduler_handle_conn_event(void *scheduler,
    xqc_connection_t *conn, xqc_scheduler_conn_event_t event, void *event_arg)
{
    (void)scheduler;
    (void)conn;
    (void)event;
    (void)event_arg;
}


static void
xqc_backup_scheduler_conn_close_diag(void *scheduler, xqc_connection_t *conn)
{
    size_t i;
    xqc_backup_scheduler_t *backup = scheduler;

    if (backup == NULL || conn == NULL || !backup->diag_enabled) {
        return;
    }

    printf("[diag_strict_backup_summary] conn=%p mode=auto_probe_lock"
           " primary=%" PRIu64 " locked=%d warmup_started=%d"
           " warmup_completed=%d warmup_timeout=%d warmup_ms=%.3f"
           " primary_picks=%" PRIu64
           " primary_selected_bytes=%" PRIu64
           " primary_cwnd_waits=%" PRIu64
           " primary_reinject_waits=%" PRIu64
           " prelock_picks=%" PRIu64
           " prelock_selected_bytes=%" PRIu64
           " warmup_picks=%" PRIu64
           " warmup_selected_bytes=%" PRIu64
           " failover_events=%" PRIu64
           " failover_picks=%" PRIu64
           " failover_selected_bytes=%" PRIu64
           " no_path_returns=%" PRIu64 "\n",
           conn, backup->primary_path_id, backup->primary_locked,
           backup->warmup_started, backup->warmup_completed,
           backup->warmup_timed_out,
           (double)backup->warmup_elapsed_us / 1000.0,
           backup->primary_picks,
           backup->primary_selected_bytes,
           backup->primary_cwnd_waits,
           backup->primary_reinject_waits,
           backup->prelock_picks,
           backup->prelock_selected_bytes,
           backup->warmup_picks,
           backup->warmup_selected_bytes,
           backup->failover_events,
           backup->failover_picks,
           backup->failover_selected_bytes,
           backup->no_path_returns);

    for (i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        xqc_backup_path_sample_t *sample = &backup->samples[i];
        if (!sample->in_use) {
            continue;
        }
        printf("[diag_strict_backup_probe_summary] conn=%p path=%" PRIu64
               " selected_picks=%" PRIu64 " selected_bytes=%" PRIu64
               " delivered_bytes=%" PRIu64 " delivery_mbps=%.3f"
               " ema_mbps=%.3f est_bw_mbps=%.3f score_mbps=%.3f"
               " srtt_us=%" PRIu64 " cwnd=%" PRIu64
               " inflight=%" PRIu64 " blocked_checks=%" PRIu64 "\n",
               conn, sample->path_id, sample->selected_picks,
               sample->selected_bytes, sample->delivered_bytes,
               sample->window_rate_Bps * 8.0 / 1000000.0,
               sample->delivery_rate_ema_Bps * 8.0 / 1000000.0,
               (double)sample->est_bw_Bps * 8.0 / 1000000.0,
               sample->score_Bps * 8.0 / 1000000.0,
               sample->srtt_us, sample->cwnd, sample->inflight,
               sample->blocked_checks);
    }
}


const xqc_scheduler_callback_t xqc_backup_scheduler_cb = {
    .xqc_scheduler_size              = xqc_backup_scheduler_size,
    .xqc_scheduler_init              = xqc_backup_scheduler_init,
    .xqc_scheduler_get_path          = xqc_backup_scheduler_get_path,
    .xqc_scheduler_handle_conn_event = xqc_backup_scheduler_handle_conn_event,
    .xqc_scheduler_conn_close_diag   = xqc_backup_scheduler_conn_close_diag,
};
