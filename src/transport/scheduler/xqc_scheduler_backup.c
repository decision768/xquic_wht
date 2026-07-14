/**
 * @copyright Copyright (c) 2022, Alibaba Group Holding Limited
 */


#include "src/transport/scheduler/xqc_scheduler_backup.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_send_ctl.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>


#define XQC_BACKUP_STRICT_PRIMARY_PATH_ID 1

typedef struct xqc_backup_scheduler_s {
    xqc_bool_t primary_locked;
    xqc_bool_t failover_active;
    uint64_t primary_picks;
    uint64_t primary_selected_bytes;
    uint64_t primary_cwnd_waits;
    uint64_t primary_reinject_waits;
    uint64_t prelock_picks;
    uint64_t prelock_selected_bytes;
    uint64_t failover_events;
    uint64_t failover_picks;
    uint64_t failover_selected_bytes;
    uint64_t no_path_returns;
} xqc_backup_scheduler_t;


static size_t
xqc_backup_scheduler_size()
{
    return sizeof(xqc_backup_scheduler_t);
}

static void
xqc_backup_scheduler_init(void *scheduler, xqc_log_t *log, xqc_scheduler_params_t *param)
{
    xqc_backup_scheduler_t *backup = scheduler;

    memset(backup, 0, sizeof(*backup));
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


static xqc_bool_t
xqc_backup_scheduler_path_is_usable(xqc_path_ctx_t *path)
{
    return path != NULL
           && path->path_state == XQC_PATH_STATE_ACTIVE
           && path->app_path_status != XQC_APP_PATH_STATUS_FROZEN;
}


xqc_path_ctx_t *
xqc_backup_scheduler_get_path(void *scheduler, xqc_connection_t *conn, 
    xqc_packet_out_t *packet_out, int check_cwnd, int reinject, 
    xqc_bool_t *cc_blocked)
{
    xqc_backup_scheduler_t *backup = scheduler;
    xqc_path_ctx_t *primary_path;
    xqc_path_ctx_t *best_path[XQC_PATH_CLASS_PERF_CLASS_SIZE];
    xqc_bool_t has_path[XQC_PATH_CLASS_PERF_CLASS_SIZE];
    xqc_path_perf_class_t path_class;
    xqc_bool_t available_path_exists;

    xqc_list_head_t *pos, *next;
    xqc_path_ctx_t *path;
    xqc_bool_t path_can_send = XQC_FALSE;

    /* min RTT */
    uint64_t path_srtt = 0;
    xqc_bool_t reached_cwnd_check = XQC_FALSE;

    if (cc_blocked) {
        *cc_blocked = XQC_FALSE;
    }

    primary_path = xqc_backup_scheduler_find_path(
        conn, XQC_BACKUP_STRICT_PRIMARY_PATH_ID);

    /*
     * Path 0 is allowed to bootstrap the connection. As soon as path 1 is
     * active, lock this scheduler to path 1 for the rest of the connection.
     */
    if (!backup->primary_locked
        && xqc_backup_scheduler_path_is_usable(primary_path))
    {
        backup->primary_locked = XQC_TRUE;
        printf("[diag_strict_backup] conn=%p event=primary_lock primary=%d\n",
               conn, XQC_BACKUP_STRICT_PRIMARY_PATH_ID);
    }

    if (backup->primary_locked
        && xqc_backup_scheduler_path_is_usable(primary_path))
    {
        backup->failover_active = XQC_FALSE;

        /* Do not move a path-1 packet to the slow path for reinjection. */
        if (reinject && packet_out->po_path_id == primary_path->path_id) {
            backup->primary_reinject_waits++;
            backup->no_path_returns++;
            return NULL;
        }

        path_can_send = xqc_scheduler_check_path_can_send(
            primary_path, packet_out, check_cwnd);
        if (!path_can_send) {
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

    /* A locked primary may fall back only after it is no longer usable. */
    if (backup->primary_locked && !backup->failover_active) {
        backup->failover_active = XQC_TRUE;
        backup->failover_events++;
        printf("[diag_strict_backup] conn=%p event=primary_failover primary=%d"
               " state=%d status=%d\n",
               conn, XQC_BACKUP_STRICT_PRIMARY_PATH_ID,
               primary_path ? primary_path->path_state : -1,
               primary_path ? primary_path->app_path_status : -1);
    }

    for (path_class = XQC_PATH_CLASS_AVAILABLE_HIGH; 
         path_class < XQC_PATH_CLASS_PERF_CLASS_SIZE; 
         path_class++)
    {
        best_path[path_class] = NULL;
        has_path[path_class] = XQC_FALSE;
    }

    xqc_list_for_each_safe(pos, next, &conn->conn_paths_list) {
        path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);

        path_class = xqc_path_get_perf_class(path);

        /* skip inactive paths */
        /* skip frozen paths */
        if (path->path_state != XQC_PATH_STATE_ACTIVE
            || path->app_path_status == XQC_APP_PATH_STATUS_FROZEN
            || (reinject && (packet_out->po_path_id == path->path_id))) 
        {
            goto skip_path;
        }

        if (!reached_cwnd_check) {
            reached_cwnd_check = XQC_TRUE;
            if (cc_blocked) {
                *cc_blocked = XQC_TRUE;
            }
        }

        has_path[path_class] = XQC_TRUE;
        path_can_send = xqc_scheduler_check_path_can_send(path, packet_out, check_cwnd);

        if (!path_can_send) {
            goto skip_path;
        }

        if (cc_blocked) {
            *cc_blocked = XQC_FALSE;
        }

        path_srtt = xqc_send_ctl_get_srtt(path->path_send_ctl);
        
        if (best_path[path_class] == NULL 
            || path_srtt < best_path[path_class]->path_send_ctl->ctl_srtt)
        {
            best_path[path_class] = path;
        }

skip_path:
        xqc_log(conn->log, XQC_LOG_DEBUG, 
                "|path srtt|conn:%p|path_id:%ui|path_srtt:%ui|path_class:%d|"
                "can_send:%d|path_status:%d|path_state:%d|reinj:%d|"
                "pkt_path_id:%ui|best_path:%i|", 
                conn, path->path_id, path_srtt, path_class, path_can_send,
                path->app_path_status, path->path_state, reinject, 
                packet_out->po_path_id,
                best_path[path_class] ? best_path[path_class]->path_id : -1);
    }

    available_path_exists = XQC_FALSE;

    for (path_class = XQC_PATH_CLASS_AVAILABLE_HIGH; 
         path_class < XQC_PATH_CLASS_PERF_CLASS_SIZE; 
         path_class++)
    {
        if (has_path[path_class] && ((path_class & 1) == 0)) {
            available_path_exists = XQC_TRUE;
        }

        /* 
         * do not use a standby path if there 
         * is an available path with higher performence level
         */
        if (best_path[path_class] != NULL) {

            if (available_path_exists && (path_class & 1) && !reinject) {
                /* skip standby path*/
                xqc_log(conn->log, XQC_LOG_DEBUG, 
                        "|skip_standby_path|path_class:%d|path_id:%ui|",
                        path_class, best_path[path_class]->path_id);
                continue;

            } else {
                xqc_log(conn->log, XQC_LOG_DEBUG, "|best path:%ui|frame_type:%s|"
                        "pn:%ui|size:%ud|reinj:%d|path_class:%d|",
                        best_path[path_class]->path_id, 
                        xqc_frame_type_2_str(conn->engine, packet_out->po_frame_types),
                        packet_out->po_pkt.pkt_num,
                        packet_out->po_used_size, reinject, path_class);

                if (backup->primary_locked) {
                    backup->failover_picks++;
                    backup->failover_selected_bytes += packet_out->po_used_size;

                } else {
                    backup->prelock_picks++;
                    backup->prelock_selected_bytes += packet_out->po_used_size;
                }
                return best_path[path_class];
            }
        }
    }

    xqc_log(conn->log, XQC_LOG_DEBUG, 
            "|No available paths to schedule|conn:%p|", conn);
    backup->no_path_returns++;
    return NULL;
}

void 
xqc_backup_scheduler_handle_conn_event(void *scheduler, 
    xqc_connection_t *conn, xqc_scheduler_conn_event_t event, void *event_arg)
{
    return;
}


static void
xqc_backup_scheduler_conn_close_diag(void *scheduler, xqc_connection_t *conn)
{
    xqc_backup_scheduler_t *backup = scheduler;

    printf("[diag_strict_backup_summary] conn=%p mode=fixed_path1"
           " primary=%d locked=%d primary_picks=%" PRIu64
           " primary_selected_bytes=%" PRIu64
           " primary_cwnd_waits=%" PRIu64
           " primary_reinject_waits=%" PRIu64
           " prelock_picks=%" PRIu64
           " prelock_selected_bytes=%" PRIu64
           " failover_events=%" PRIu64
           " failover_picks=%" PRIu64
           " failover_selected_bytes=%" PRIu64
           " no_path_returns=%" PRIu64 "\n",
           conn, XQC_BACKUP_STRICT_PRIMARY_PATH_ID,
           backup->primary_locked,
           backup->primary_picks,
           backup->primary_selected_bytes,
           backup->primary_cwnd_waits,
           backup->primary_reinject_waits,
           backup->prelock_picks,
           backup->prelock_selected_bytes,
           backup->failover_events,
           backup->failover_picks,
           backup->failover_selected_bytes,
           backup->no_path_returns);
}

const xqc_scheduler_callback_t xqc_backup_scheduler_cb = {
    .xqc_scheduler_size      = xqc_backup_scheduler_size,
    .xqc_scheduler_init      = xqc_backup_scheduler_init,
    .xqc_scheduler_get_path  = xqc_backup_scheduler_get_path,
    .xqc_scheduler_handle_conn_event = xqc_backup_scheduler_handle_conn_event,
    .xqc_scheduler_conn_close_diag = xqc_backup_scheduler_conn_close_diag,
};
