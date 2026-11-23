/**
 * ECF scheduler adapted from the public Peekaboo MPQUIC artifact.
 * See THIRD_PARTY_LICENSES.md for the quic-go MIT notice.
 */

#include "src/transport/scheduler/xqc_scheduler_ecf.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/xqc_conn.h"
#include "src/transport/xqc_send_ctl.h"

typedef struct {
    xqc_bool_t previous_wait;
} xqc_ecf_scheduler_t;


static size_t
xqc_ecf_scheduler_size(void)
{
    return sizeof(xqc_ecf_scheduler_t);
}


static void
xqc_ecf_scheduler_init(void *scheduler, xqc_log_t *log,
    xqc_scheduler_params_t *params)
{
    xqc_ecf_scheduler_t *ecf = scheduler;
    ecf->previous_wait = XQC_FALSE;
}


static xqc_bool_t
xqc_ecf_should_wait(uint64_t fast_rtt, uint64_t slow_rtt,
    uint64_t fast_rttvar, uint64_t slow_rttvar, uint64_t fast_cwnd,
    uint64_t slow_cwnd, uint64_t queue_size, xqc_bool_t previous_wait)
{
    if (fast_rtt == 0 || slow_rtt <= fast_rtt || fast_cwnd == 0
        || slow_cwnd == 0)
    {
        return XQC_FALSE;
    }

    uint64_t delta = xqc_max(fast_rttvar, slow_rttvar);
    uint64_t fast_work = xqc_max(queue_size, fast_cwnd);
    long double lhs = (long double) fast_rtt
        * ((long double) fast_work + fast_cwnd) * 4.0L;
    long double rhs = (long double) fast_cwnd
        * ((long double) slow_rtt + delta)
        * (previous_wait ? 5.0L : 4.0L);

    if (lhs < rhs) {
        uint64_t slow_work = xqc_max(queue_size, slow_cwnd);
        long double slow_lhs = (long double) slow_rtt * slow_work;
        long double slow_rhs = (long double) slow_cwnd
            * (2.0L * fast_rtt + delta);
        if (slow_lhs > slow_rhs) {
            return XQC_TRUE;
        }
    }

    return XQC_FALSE;
}


static xqc_path_ctx_t *
xqc_ecf_scheduler_get_path(void *scheduler, xqc_connection_t *conn,
    xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_bool_t *cc_blocked)
{
    xqc_ecf_scheduler_t *ecf = scheduler;
    xqc_path_ctx_t *fast_path, *slow_path;

    if (cc_blocked != NULL) {
        *cc_blocked = XQC_FALSE;
    }

    if (xqc_scheduler_select_path_pair(conn, packet_out, check_cwnd, reinject,
                                       &fast_path, &slow_path))
    {
        /* A packet exempt from CWND reveals nothing about the data window. */
        if (check_cwnd && XQC_CAN_IN_FLIGHT(packet_out->po_frame_types)) {
            ecf->previous_wait = XQC_FALSE;
        }
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
        ecf->previous_wait = XQC_FALSE;
        return slow_path;
    }

    xqc_send_ctl_t *fast_ctl = fast_path->path_send_ctl;
    xqc_send_ctl_t *slow_ctl = slow_path->path_send_ctl;
    ecf->previous_wait = xqc_ecf_should_wait(
        xqc_send_ctl_get_srtt(fast_ctl), xqc_send_ctl_get_srtt(slow_ctl),
        fast_ctl->ctl_rttvar, slow_ctl->ctl_rttvar,
        xqc_scheduler_get_cwnd(fast_path),
        xqc_scheduler_get_cwnd(slow_path),
        xqc_conn_get_unscheduled_bytes(conn), ecf->previous_wait);

    return ecf->previous_wait ? NULL : slow_path;
}


const xqc_scheduler_callback_t xqc_ecf_scheduler_cb = {
    .xqc_scheduler_size     = xqc_ecf_scheduler_size,
    .xqc_scheduler_init     = xqc_ecf_scheduler_init,
    .xqc_scheduler_get_path = xqc_ecf_scheduler_get_path,
};
