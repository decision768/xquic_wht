#ifndef XQC_TRANSPORT_OBSERVER_H
#define XQC_TRANSPORT_OBSERVER_H
#include <xquic/xqc_observer.h>
#include "src/transport/xqc_observer_state.h"
#define XQC_OBSERVER_MAX_STREAMS 4096U
struct xqc_observer_stream_s {
    xqc_observer_state_t state;
    uint64_t id, request_index, file_offset, fragment_length, body_bytes;
    uint64_t bind_us, first_body_us, last_body_us;
    uint64_t counted_b, interval_start_us;
    const char *end_reason;
    int invalid_reported, counted, bound, direction;
};
struct xqc_observer_s {
    char record[32768], paths_record[24576], body_record[30720], scheduler_record[4096];
    xqc_observer_sink_pt sink;
    void *sink_data;
    char uid[80], endpoint[16], attempt[192], direction[8];
    uint64_t seq, event_seq, dropped, role_epoch, primary_id, secondary_id, overflow_count;
    uint64_t write_cost_us, write_samples, write_bytes, queue_bytes, queue_depth;
    uint64_t aggregate_last_us, aggregate_from_us, sum_b, sum_b_peak, sum_b_byte_us, any_hol_us;
    uint64_t attached_us, first_request_id;
    unsigned pending_streams, unbound_observed;
    struct {
        uint64_t id, generation;
        char interface_name[64], local[160], peer[160];
        int used;
    } mappings[8];
    unsigned stream_count;
    int aggregate_prebind_unobserved;
    int invalid, role_valid, role_initialized, phase;
    double target_pct;
};
void xqc_observer_stream_update(xqc_stream_t *, uint64_t, uint64_t);
void xqc_observer_stream_end(xqc_stream_t *, const char *);
void xqc_observer_destroy(xqc_connection_t *);
void xqc_observer_event(xqc_connection_t *, const char *, const char *);
void xqc_observer_role(xqc_connection_t *, int, uint64_t, uint64_t, double, int);
/* Scheduler export must be a pure read, never a get_path call. */
int xqc_adaptive_observer_path_json(xqc_connection_t *, uint64_t, char *, size_t);
int xqc_adaptive_observer_json(xqc_connection_t *, char *, size_t);
#endif
