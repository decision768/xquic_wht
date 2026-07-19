/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */


#include "src/transport/scheduler/xqc_scheduler_rap.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_send_ctl.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    xqc_sched_diag_t diag;
    uint64_t selected_min_rtt;
    uint64_t selected_original_reinject;
    uint64_t no_sendable;
} xqc_rap_scheduler_t;


static size_t
xqc_rap_scheduler_size()
{
    return sizeof(xqc_rap_scheduler_t);
}

static void
xqc_rap_scheduler_init(void *scheduler, xqc_log_t *log, xqc_scheduler_params_t *param)
{
    (void)log;
    xqc_rap_scheduler_t *rap = scheduler;
    if (rap) {
        memset(rap, 0, sizeof(*rap));
        xqc_scheduler_diag_init(&rap->diag, "rap", param);
    }
}

xqc_path_ctx_t *
xqc_rap_scheduler_get_path(void *scheduler,
    xqc_connection_t *conn, xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_bool_t *cc_blocked)
{
    xqc_rap_scheduler_t *rap = scheduler;
    xqc_path_ctx_t *best_path = NULL;
    xqc_path_ctx_t *original_path = NULL;

    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;
    xqc_send_ctl_t *send_ctl;

    /* min RTT */
    uint64_t min_rtt = XQC_MAX_UINT64_VALUE;
    uint64_t path_srtt;
    xqc_bool_t reached_cwnd_check = XQC_FALSE;
    xqc_bool_t has_sendable_path = XQC_FALSE;

    if (cc_blocked) {
        *cc_blocked = XQC_FALSE;
    }

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);

        if (path->path_state != XQC_PATH_STATE_ACTIVE) {
            continue;
        }

        /* skip the frozen path */
        if (path->app_path_status == XQC_APP_PATH_STATUS_FROZEN) {
            continue;
        }

        if (!reached_cwnd_check) {
            reached_cwnd_check = XQC_TRUE;
            if (cc_blocked) {
                *cc_blocked = XQC_TRUE;
            }
        }

        xqc_sched_path_eval_t evaluation;
        if (!xqc_scheduler_evaluate_path(path, packet_out, check_cwnd,
            &evaluation))
        {
            xqc_scheduler_diag_record_evaluation(rap ? &rap->diag : NULL,
                path, &evaluation);
            continue;
        }
        xqc_scheduler_diag_record_evaluation(rap ? &rap->diag : NULL,
            path, &evaluation);
        has_sendable_path = XQC_TRUE;

        if (cc_blocked) {
            *cc_blocked = XQC_FALSE;
        }

        if (reinject && (packet_out->po_path_id == path->path_id)) {
            original_path = path;
            continue;
        }

        path_srtt = xqc_send_ctl_get_srtt(path->path_send_ctl);
        xqc_log(conn->log, XQC_LOG_DEBUG, "|path srtt|conn:%p|path_id:%ui|path_srtt:%ui|", 
                conn, path->path_id, path_srtt);

        if (path_srtt < min_rtt) {
            best_path = path;
            min_rtt = path_srtt;
        }
    }

    if (best_path == NULL) {
        if (original_path == NULL) {
            xqc_log(conn->log, XQC_LOG_DEBUG, "|No available paths to schedule|conn:%p|", conn);

        } else {
            if (!(packet_out->po_flag & XQC_POF_REINJECT_DIFF_PATH)) {
                best_path = original_path;
                if (rap && rap->diag.enabled) {
                    rap->selected_original_reinject++;
                }
                xqc_log(conn->log, XQC_LOG_DEBUG, "|the original path is selected|conn:%p|", conn);

            } else {
                xqc_log(conn->log, XQC_LOG_DEBUG, "|the packet must be reinjected on a different path|conn:%p|", conn);
            }
        }
    
    } else {
        if (rap && rap->diag.enabled) {
            rap->selected_min_rtt++;
        }
        xqc_log(conn->log, XQC_LOG_DEBUG, "|best path:%ui|frame_type:%s|",
                best_path->path_id, xqc_frame_type_2_str(conn->engine, packet_out->po_frame_types));
    }

    if (rap && rap->diag.enabled && reached_cwnd_check
        && !has_sendable_path)
    {
        rap->no_sendable++;
    }
    xqc_scheduler_diag_record_decision(rap ? &rap->diag : NULL, best_path,
        packet_out ? packet_out->po_used_size : 0,
        reached_cwnd_check && !has_sendable_path);
    xqc_scheduler_diag_maybe_tick(rap ? &rap->diag : NULL, conn, 0);

    return best_path;
}

static void
xqc_rap_scheduler_conn_close_diag(void *scheduler, xqc_connection_t *conn)
{
    xqc_rap_scheduler_t *rap = scheduler;
    if (!rap || !rap->diag.enabled || rap->diag.summary_emitted) {
        return;
    }
    printf("[diag_rap_summary] selected_min_rtt=%" PRIu64
           " selected_original_reinject=%" PRIu64
           " no_sendable=%" PRIu64 "\n", rap->selected_min_rtt,
           rap->selected_original_reinject, rap->no_sendable);
    xqc_scheduler_diag_close(&rap->diag, conn);
}

const xqc_scheduler_callback_t xqc_rap_scheduler_cb = {
    .xqc_scheduler_size             = xqc_rap_scheduler_size,
    .xqc_scheduler_init             = xqc_rap_scheduler_init,
    .xqc_scheduler_get_path         = xqc_rap_scheduler_get_path,
    .xqc_scheduler_conn_close_diag  = xqc_rap_scheduler_conn_close_diag,
};
