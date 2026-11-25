#ifndef XQC_ADAPTIVE_V2_H
#define XQC_ADAPTIVE_V2_H
#include "adaptive_arrival_predictor.h"
#include "src/transport/xqc_packet_out.h"
#include "src/transport/xqc_timer.h"

extern const xqc_scheduler_callback_t xqc_adaptive_v2_scheduler_cb;
void xqc_adaptive_v2_init_core(void *, xqc_log_t *, xqc_scheduler_params_t *);
xqc_path_ctx_t *xqc_adaptive_get_path_core(void *, xqc_connection_t *,
    xqc_packet_out_t *, int, int, xqc_bool_t *);
typedef struct xqc_arrival_stream_s {
    char transfer_id[385]; /* JSON escaped at registration */
    uint32_t request_index;
    uint64_t file_offset, body_length, spent_us;
    int cancelled;
} xqc_arrival_stream_t;
typedef struct xqc_arrival_s xqc_arrival_t;
void xqc_arrival_conn_init(xqc_connection_t *);
void xqc_arrival_conn_close(xqc_connection_t *);
void xqc_arrival_stream_close(xqc_connection_t *, uint64_t);
void xqc_arrival_frame_written(xqc_stream_t *, xqc_packet_out_t *, size_t);
void xqc_arrival_priority_promote(xqc_connection_t *, xqc_packet_out_t *, xqc_list_head_t *);
void xqc_arrival_packet_gone(xqc_connection_t *, xqc_packet_out_t *);
void xqc_arrival_path_reset(xqc_connection_t *, uint64_t);
void xqc_arrival_pacing_wake(xqc_send_ctl_t *, uint64_t);
void xqc_arrival_ack(xqc_send_ctl_t *, uint64_t, uint64_t);
void xqc_arrival_rate(xqc_send_ctl_t *, uint64_t, int);
void xqc_arrival_socket_result(xqc_connection_t *, xqc_path_ctx_t *, xqc_packet_out_t *,
    int64_t, uint64_t, uint64_t, int);
void xqc_arrival_sent(xqc_send_ctl_t *, xqc_packet_out_t *,
    uint64_t socket_start_us, uint64_t socket_return_us, int batch);
int xqc_arrival_fallback(xqc_connection_t *, xqc_packet_out_t *, xqc_path_ctx_t *,
    xqc_path_ctx_t *, size_t, int, uint64_t);
void xqc_arrival_roles(xqc_connection_t *, uint64_t, uint64_t, int, uint64_t);
void xqc_arrival_clear_hint(xqc_connection_t *);
int xqc_arrival_is_defer(xqc_connection_t *, xqc_packet_out_t *);
int xqc_arrival_skip(xqc_connection_t *, xqc_packet_out_t *, uint64_t);
void xqc_arrival_queued(xqc_connection_t *, xqc_packet_out_t *, xqc_path_ctx_t *, uint64_t);
xqc_list_head_t *xqc_arrival_scan_start(xqc_connection_t *, xqc_list_head_t *, uint64_t);
xqc_list_head_t *xqc_arrival_scan_next(xqc_connection_t *, xqc_list_head_t *, xqc_list_head_t *, xqc_list_head_t *);
int xqc_arrival_scan_cut(xqc_connection_t *, xqc_list_head_t *, xqc_list_head_t *, unsigned, uint64_t);
#endif
