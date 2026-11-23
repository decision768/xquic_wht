/**
 * BLEST scheduler adapted from the public Peekaboo MPQUIC artifact.
 * See THIRD_PARTY_LICENSES.md for the quic-go MIT notice.
 */

#include "src/transport/scheduler/xqc_scheduler_blest.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_send_ctl.h"


static size_t
xqc_blest_scheduler_size(void)
{
    return 0;
}


static void
xqc_blest_scheduler_init(void *scheduler, xqc_log_t *log,
    xqc_scheduler_params_t *params)
{
    return;
}


static xqc_bool_t
xqc_blest_should_wait(uint64_t mss, uint64_t fast_rtt,
    uint64_t slow_rtt, uint64_t fast_cwnd_bytes, uint64_t slow_inflight,
    uint64_t send_window)
{
    if (mss == 0 || fast_rtt == 0 || slow_rtt <= fast_rtt
        || fast_cwnd_bytes == 0)
    {
        return XQC_FALSE;
    }

    if (send_window <= slow_inflight
        || send_window - slow_inflight <= mss)
    {
        return XQC_TRUE;
    }

    /* Scale both byte counts by 2 * fast_rtt^2 to avoid rounding the RTT ratio.
     * Only the congestion window growth term needs conversion from packets. */
    long double fast_bytes_scaled = (long double) slow_rtt
        * (2.0L * fast_rtt * fast_cwnd_bytes
           + (long double) mss * (slow_rtt - fast_rtt));
    long double window_bytes_scaled = 2.0L * fast_rtt * fast_rtt
        * (send_window - slow_inflight - mss);

    return fast_bytes_scaled > window_bytes_scaled;
}


static xqc_path_ctx_t *
xqc_blest_scheduler_get_path(void *scheduler, xqc_connection_t *conn,
    xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_bool_t *cc_blocked)
{
    xqc_path_ctx_t *fast_path, *slow_path;

    if (cc_blocked != NULL) {
        *cc_blocked = XQC_FALSE;
    }

    if (xqc_scheduler_select_path_pair(conn, packet_out, check_cwnd, reinject,
                                       &fast_path, &slow_path))
    {
        return fast_path;
    }
    if (fast_path == NULL) {
        return NULL;
    }

    if (slow_path == NULL) {
        if (cc_blocked != NULL && check_cwnd) {
            *cc_blocked = XQC_TRUE;
        }
        return NULL;
    }

    if (reinject || (packet_out->po_flag & (XQC_POF_LOST | XQC_POF_TLP))) {
        return slow_path;
    }

    uint64_t fast_rtt = xqc_send_ctl_get_srtt(fast_path->path_send_ctl);
    uint64_t slow_rtt = xqc_send_ctl_get_srtt(slow_path->path_send_ctl);
    uint64_t send_window = xqc_scheduler_get_send_window(conn, packet_out);
    xqc_bool_t wait = xqc_blest_should_wait(
        xqc_conn_get_mss(conn), fast_rtt, slow_rtt,
        xqc_scheduler_get_cwnd(fast_path),
        slow_path->path_send_ctl->ctl_bytes_in_flight, send_window);

    return wait ? NULL : slow_path;
}


const xqc_scheduler_callback_t xqc_blest_scheduler_cb = {
    .xqc_scheduler_size     = xqc_blest_scheduler_size,
    .xqc_scheduler_init     = xqc_blest_scheduler_init,
    .xqc_scheduler_get_path = xqc_blest_scheduler_get_path,
};
