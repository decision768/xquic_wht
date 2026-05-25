#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_multipath.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_send_ctl.h"

#define XQC_MIN_PATH_RTT_PROBE_TIMEOUT_US 200000
#define XQC_BOOTSTRAP_PATH_RTT_PROBE_TIMEOUT_US 50000

xqc_bool_t
xqc_scheduler_check_path_can_send(xqc_path_ctx_t *path, xqc_packet_out_t *packet_out, int check_cwnd)
{
    xqc_send_ctl_t *send_ctl = path->path_send_ctl;
    uint32_t schedule_bytes = path->path_schedule_bytes;
    uint64_t cwnd = 0;
    uint64_t bytes_in_flight = 0;
    uint64_t bytes_on_path = 0;

    if (send_ctl && send_ctl->ctl_cong_callback
        && send_ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd)
    {
        cwnd = send_ctl->ctl_cong_callback->xqc_cong_ctl_get_cwnd(send_ctl->ctl_cong);
    }
    bytes_in_flight = send_ctl ? send_ctl->ctl_bytes_in_flight : 0;
    bytes_on_path = schedule_bytes + bytes_in_flight;

    /* normal packets in send list will be blocked by cc */
    if (check_cwnd && (!xqc_send_packet_cwnd_allows(send_ctl, packet_out, schedule_bytes, 0)))
    {
        xqc_log(send_ctl->ctl_conn->log, XQC_LOG_DEBUG, "|path:%ui|blocked by cwnd|", path->path_id);
        return XQC_FALSE;
    }

    return XQC_TRUE;
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
