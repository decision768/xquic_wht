/**
 * XQUIC adaptation of Peekaboo's public artifact deployment scheduler.
 *
 * Peekaboo source artifact copyright (c) 2016 the quic-go authors & Google,
 * Inc., used under the MIT License. See THIRD_PARTY_LICENSES.md.
 */

#include "src/transport/scheduler/xqc_scheduler_peekaboo.h"
#include "src/transport/scheduler/xqc_scheduler_common.h"
#include "src/transport/scheduler/xqc_scheduler_go_rand.h"
#include "src/transport/xqc_send_ctl.h"

#define XQC_PEEKABOO_SEED                 2020
#define XQC_PEEKABOO_WAIT_FOLLOW_PERCENT  70
#define XQC_PEEKABOO_SEND_FOLLOW_PERCENT  90
#define XQC_PEEKABOO_MODEL_DIMENSION      6

typedef struct {
    xqc_go_rand_t   random;
    xqc_bool_t     waiting_for_fast_path;
} xqc_peekaboo_scheduler_t;

/* Precomputed inverse(A) * b for the upstream built-in deployment model. */
static const double xqc_peekaboo_theta_wait[XQC_PEEKABOO_MODEL_DIMENSION] = {
    1.8783506607643780e-04,
    7.1928252271007882e-05,
    1.4543977921745361e-03,
    9.4641576431458844e-04,
    1.9274250353218893e-04,
    1.3965510094924384e-04,
};

static const double xqc_peekaboo_theta_send[XQC_PEEKABOO_MODEL_DIMENSION] = {
    2.2456489027088597e-04,
    7.5905098924424492e-05,
    1.8815874912196180e-03,
    1.2479756498805540e-03,
    2.2980004350297512e-04,
    1.6294607662836717e-04,
};

static size_t
xqc_peekaboo_scheduler_size(void)
{
    return sizeof(xqc_peekaboo_scheduler_t);
}

static void
xqc_peekaboo_scheduler_init(void *scheduler, xqc_log_t *log,
    xqc_scheduler_params_t *params)
{
    xqc_peekaboo_scheduler_t *peekaboo = scheduler;
    xqc_go_rand_seed(&peekaboo->random, XQC_PEEKABOO_SEED);
    peekaboo->waiting_for_fast_path = XQC_FALSE;
}

static xqc_bool_t
xqc_peekaboo_model_prefers_wait(xqc_connection_t *conn,
    const xqc_packet_out_t *packet_out, const xqc_send_ctl_t *fast_ctl,
    const xqc_send_ctl_t *slow_ctl, const xqc_sched_path_eval_t *fast_eval,
    const xqc_sched_path_eval_t *slow_eval)
{
    uint64_t fast_rtt = fast_ctl->ctl_latest_rtt != 0
        ? fast_ctl->ctl_latest_rtt : fast_ctl->ctl_srtt;
    uint64_t slow_rtt = slow_ctl->ctl_latest_rtt != 0
        ? slow_ctl->ctl_latest_rtt : slow_ctl->ctl_srtt;
    if (fast_rtt == 0 || slow_rtt == 0) {
        return XQC_FALSE;
    }

    /* The upstream model was fitted with RTTs expressed in nanoseconds. */
    double fast_rtt_ns = (double) fast_rtt * 1000.0;
    double slow_rtt_ns = (double) slow_rtt * 1000.0;
    double window = (double) xqc_scheduler_get_send_window(conn, packet_out);
    const double feature[XQC_PEEKABOO_MODEL_DIMENSION] = {
        fast_eval->raw_cwnd / fast_rtt_ns,
        slow_eval->bytes_in_flight / slow_rtt_ns,
        window / fast_rtt_ns,
        window / slow_rtt_ns,
        fast_eval->bytes_in_flight / fast_rtt_ns,
        slow_eval->raw_cwnd / slow_rtt_ns,
    };

    double wait_score = 0;
    double send_score = 0;
    for (size_t i = 0; i < XQC_PEEKABOO_MODEL_DIMENSION; i++) {
        wait_score += xqc_peekaboo_theta_wait[i] * feature[i];
        send_score += xqc_peekaboo_theta_send[i] * feature[i];
    }
    return send_score < wait_score;
}

static xqc_path_ctx_t *
xqc_peekaboo_scheduler_get_path(void *scheduler, xqc_connection_t *conn,
    xqc_packet_out_t *packet_out, int check_cwnd, int reinject,
    xqc_bool_t *cc_blocked)
{
    xqc_peekaboo_scheduler_t *peekaboo = scheduler;
    xqc_path_ctx_t *fast_path = NULL;
    xqc_path_ctx_t *slow_path = NULL;
    uint64_t fast_rtt = 0;
    uint64_t slow_rtt = 0;
    xqc_list_head_t *pos;

    if (cc_blocked != NULL) {
        *cc_blocked = XQC_FALSE;
    }

    xqc_list_for_each(pos, &conn->conn_paths_list) {
        xqc_path_ctx_t *path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        if (!xqc_scheduler_path_eligible(path, packet_out, reinject)) {
            continue;
        }

        uint64_t rtt = xqc_send_ctl_get_srtt(path->path_send_ctl);
        if (fast_path == NULL || rtt < fast_rtt) {
            fast_path = path;
            fast_rtt = rtt;
        }
    }
    if (fast_path == NULL) {
        return NULL;
    }

    xqc_sched_path_eval_t fast_eval;
    if (xqc_scheduler_evaluate_path(fast_path, packet_out, check_cwnd, &fast_eval)) {
        peekaboo->waiting_for_fast_path = XQC_FALSE;
        return fast_path;
    }

    xqc_sched_path_eval_t slow_eval = {0};
    xqc_list_for_each(pos, &conn->conn_paths_list) {
        xqc_path_ctx_t *path = xqc_list_entry(pos, xqc_path_ctx_t, path_list);
        if (path == fast_path || !xqc_scheduler_path_eligible(path, packet_out, reinject)) {
            continue;
        }

        uint64_t rtt = xqc_send_ctl_get_srtt(path->path_send_ctl);
        xqc_sched_path_eval_t candidate_eval;
        if ((slow_path == NULL || rtt < slow_rtt)
            && xqc_scheduler_evaluate_path(path, packet_out, check_cwnd,
                                           &candidate_eval))
        {
            slow_path = path;
            slow_rtt = rtt;
            slow_eval = candidate_eval;
        }
    }
    if (slow_path == NULL) {
        if (cc_blocked != NULL) {
            *cc_blocked = XQC_TRUE;
        }
        return NULL;
    }

    if (reinject || (packet_out->po_flag & (XQC_POF_LOST | XQC_POF_TLP))) {
        peekaboo->waiting_for_fast_path = XQC_FALSE;
        return slow_path;
    }

    if (!peekaboo->waiting_for_fast_path) {
        xqc_bool_t model_wait = xqc_peekaboo_model_prefers_wait(conn, packet_out,
            fast_path->path_send_ctl, slow_path->path_send_ctl, &fast_eval, &slow_eval);
        uint32_t draw = xqc_go_rand_intn_100(&peekaboo->random);
        peekaboo->waiting_for_fast_path = model_wait
            ? draw < XQC_PEEKABOO_WAIT_FOLLOW_PERCENT
            : draw >= XQC_PEEKABOO_SEND_FOLLOW_PERCENT;
    }

    /* An intentional wait leaves cc_blocked false. */
    return peekaboo->waiting_for_fast_path ? NULL : slow_path;
}

const xqc_scheduler_callback_t xqc_peekaboo_scheduler_cb = {
    .xqc_scheduler_size     = xqc_peekaboo_scheduler_size,
    .xqc_scheduler_init     = xqc_peekaboo_scheduler_init,
    .xqc_scheduler_get_path = xqc_peekaboo_scheduler_get_path,
};
