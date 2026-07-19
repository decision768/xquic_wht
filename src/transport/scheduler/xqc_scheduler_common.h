#ifndef _XQC_SCHEDULER_COMMON_H_INCLUDED_
#define _XQC_SCHEDULER_COMMON_H_INCLUDED_

#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>

typedef enum {
    XQC_SCHED_BLOCK_NONE = 0,
    XQC_SCHED_BLOCK_REAL_INFLIGHT,
    XQC_SCHED_BLOCK_SCHEDULED_RESERVATION,
} xqc_sched_block_reason_t;

typedef struct {
    uint64_t                    raw_cwnd;
    uint64_t                    effective_cwnd;
    uint64_t                    bytes_in_flight;
    uint64_t                    path_schedule_bytes;
    uint32_t                    packet_size;
    uint64_t                    headroom;
    xqc_bool_t                  can_send;
    xqc_sched_block_reason_t    block_reason;
} xqc_sched_path_eval_t;

typedef struct {
    xqc_bool_t              in_use;
    uint64_t                path_id;
    uint64_t                checks;
    uint64_t                sendable;
    uint64_t                blocked_real;
    uint64_t                blocked_scheduled;
    uint64_t                selected_packets;
    uint64_t                selected_bytes;
    uint64_t                tick_selected_base;
    xqc_sched_path_eval_t   last_eval;
} xqc_sched_diag_path_t;

typedef struct {
    xqc_bool_t              enabled;
    xqc_bool_t              summary_emitted;
    const char             *sched_name;
    xqc_usec_t              start_us;
    xqc_usec_t              last_tick_us;
    xqc_usec_t              interval_us;
    uint64_t                decisions;
    uint64_t                active_no_sendable;
    uint64_t                tick_poll_count;
    xqc_sched_diag_path_t   paths[XQC_MAX_PATHS_COUNT];
} xqc_sched_diag_t;

xqc_sched_block_reason_t xqc_scheduler_classify_cwnd_block(
    uint64_t effective_cwnd, uint64_t inflight, uint64_t scheduled,
    uint32_t packet_size, int check_cwnd);
xqc_bool_t xqc_scheduler_evaluate_path(xqc_path_ctx_t *path,
    xqc_packet_out_t *packet_out, int check_cwnd,
    xqc_sched_path_eval_t *evaluation);
xqc_sched_diag_path_t *xqc_scheduler_diag_record_evaluation(
    xqc_sched_diag_t *diag, xqc_path_ctx_t *path,
    const xqc_sched_path_eval_t *evaluation);
void xqc_scheduler_diag_init(xqc_sched_diag_t *diag, const char *sched_name,
    const xqc_scheduler_params_t *params);
void xqc_scheduler_diag_record_decision(xqc_sched_diag_t *diag,
    xqc_path_ctx_t *selected, size_t packet_size, xqc_bool_t no_sendable);
void xqc_scheduler_diag_maybe_tick(xqc_sched_diag_t *diag,
    xqc_connection_t *conn, xqc_usec_t now);
void xqc_scheduler_diag_close(xqc_sched_diag_t *diag,
    xqc_connection_t *conn);

xqc_bool_t xqc_scheduler_check_path_can_send(xqc_path_ctx_t *path, xqc_packet_out_t *packet_out, int check_cwnd);
void xqc_scheduler_handle_path_probe(xqc_connection_t *conn);

#endif
