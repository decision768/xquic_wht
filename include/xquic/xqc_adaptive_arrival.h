#ifndef XQC_ADAPTIVE_ARRIVAL_H
#define XQC_ADAPTIVE_ARRIVAL_H
#include <xquic/xquic.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XQC_ARRIVAL_CONFIG_VERSION 1
#define XQC_ARRIVAL_MODEL "arrival_proxy_v1"
typedef enum {
    XQC_ARRIVAL_OFF, XQC_ARRIVAL_SHADOW, XQC_ARRIVAL_ENFORCE
} xqc_arrival_mode_t;

/* A separate, size/version checked API; old connection settings keep their ABI. */
typedef struct {
    uint32_t struct_size, version, mode;
    uint32_t predict_horizon_us, max_defer_episode_us, max_defer_stream_us;
    uint32_t recheck_interval_us, gain_margin_floor_us;
    double gain_margin_rtt_ratio;
    uint32_t min_valid_rate_samples, rate_sample_capacity;
    uint32_t ack_batch_capacity, min_ack_batches, max_sim_events_per_path;
    uint32_t max_waiting_streams, schedule_scan_budget, prediction_log_limit_per_s;
    char output_prefix[1024];
} xqc_arrival_config_t;

XQC_EXPORT_PUBLIC_API void xqc_arrival_config_init(xqc_arrival_config_t *);
XQC_EXPORT_PUBLIC_API int xqc_arrival_config_validate(const xqc_arrival_config_t *);
/* Keys are the names above; also accepts adaptive_arrival_mode and mode. */
XQC_EXPORT_PUBLIC_API int xqc_arrival_config_option(xqc_arrival_config_t *, const char *, const char *);
XQC_EXPORT_PUBLIC_API const char *xqc_arrival_mode_name(unsigned);
XQC_EXPORT_PUBLIC_API const char *xqc_arrival_source_build_id(void);
XQC_EXPORT_PUBLIC_API int xqc_engine_set_arrival_config(xqc_engine_t *, const xqc_arrival_config_t *);
/* Engine config applies to future connections; set_mode cancels existing waits. */
XQC_EXPORT_PUBLIC_API int xqc_arrival_set_mode(xqc_engine_t *, const xqc_cid_t *, unsigned);
XQC_EXPORT_PUBLIC_API int xqc_arrival_register_get(xqc_engine_t *, const xqc_cid_t *,
    uint64_t stream_id, const char *transfer_id, uint32_t request_index,
    uint64_t file_offset, uint64_t body_length);
typedef int (*xqc_arrival_sink_pt)(void *, int, const char *);
XQC_EXPORT_PUBLIC_API int xqc_arrival_attach_sink(xqc_engine_t *, const xqc_cid_t *,
    xqc_arrival_sink_pt, void *);
#ifdef __cplusplus
}
#endif
#endif
