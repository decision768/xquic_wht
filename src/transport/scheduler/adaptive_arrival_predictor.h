#ifndef XQC_ARRIVAL_PREDICTOR_H
#define XQC_ARRIVAL_PREDICTOR_H
#include <xquic/xqc_adaptive_arrival.h>
#define XQC_ARRIVAL_CAP 16
typedef enum {
    AR_OK, AR_INPUT, AR_OVERFLOW, AR_RATE_SAMPLES, AR_RATE_STALE,
    AR_RATE_SPAN, AR_CC_UNSUPPORTED, AR_ACK_SAMPLES, AR_ACK_JITTER,
    AR_ACK_FORECAST_OVERDUE, AR_ACK_HORIZON_UNSUPPORTED,
    AR_HORIZON, AR_EVENT_LIMIT, AR_PRIORITY, AR_SOCKET_BLOCKED, AR_REASON_COUNT
} xqc_arrival_invalid_t;
typedef struct {
    uint64_t time_us, rate, interval_us, seq;
    unsigned phase;
} xqc_arrival_rate_t;
typedef struct { uint64_t time_us, bytes, seq; } xqc_arrival_ack_t;
typedef struct {
    xqc_arrival_rate_t rates[XQC_ARRIVAL_CAP];
    xqc_arrival_ack_t acks[XQC_ARRIVAL_CAP];
    unsigned nr, na;
    uint64_t ack_seq;
} xqc_arrival_history_t;
typedef struct {
    uint64_t path_id, now_us, C, I, Z, L, srtt_us, raw_bw, pacing_Bps;
    uint64_t budget, pending, last_sent_us, pacing_expire_us, max_burst;
    uint32_t packet_quantum;
    int pacing_on, bbr_full, priority_pending, socket_blocked;
} xqc_arrival_snapshot_t;
typedef struct {
    int valid, rate_metadata_known, ack_age_known, service_known;
    xqc_arrival_invalid_t reason;
    uint64_t W_us, service_us, network_us, cost_us, next_event_us;
    uint64_t r_hat, rate_age, rate_span, ack_age, ack_gap, ack_bytes;
    unsigned events, rate_count, ack_count;
    xqc_arrival_snapshot_t snapshot;
} xqc_arrival_prediction_t;
void xqc_arrival_history_rate(xqc_arrival_history_t *, const xqc_arrival_config_t *,
    uint64_t now, uint64_t rate, uint64_t interval, int limited, unsigned phase);
void xqc_arrival_history_ack(xqc_arrival_history_t *, const xqc_arrival_config_t *,
    uint64_t now, uint64_t actual_freed);
xqc_arrival_prediction_t xqc_arrival_predict(const xqc_arrival_config_t *,
    const xqc_arrival_history_t *, const xqc_arrival_snapshot_t *);
int xqc_arrival_compare(const xqc_arrival_config_t *, const xqc_arrival_prediction_t *,
    const xqc_arrival_prediction_t *, uint64_t episode_left, uint64_t stream_left,
    int64_t *gain, uint64_t *margin);
const char *xqc_arrival_invalid_name(unsigned);
#endif
