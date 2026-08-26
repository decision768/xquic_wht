/**
 * @file mini_server_cb.c contains callbacks definitions for mini_server, including:
 * 1. engine callbacks
 * 2. hq callbacks
 * 3. h3 callbacks
 */

#include "mini_server_cb.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdarg.h>
#include <time.h>



#define XQC_MINI_SVR_MAX_TRACKED_FRAGMENTS 262144
#define SERVER_OUTPUT_FILE "server_received.txt"
#define XQC_MINI_SVR_STREAM_RANGE_SIZE (4 * 1024 * 1024)
/* engine callbacks */
typedef struct {
    int initialized;
    int generation;
    int expected_streams;
    size_t total_size;
    size_t total_received;
    int completed_streams;
    struct timeval earliest_start;
    struct timeval latest_end;
    unsigned char *stream_done;
    size_t *stream_progress;
    size_t stream_capacity;
    uint64_t recv_aggregate_sample_count;
    uint64_t recv_aggregate_sample_bytes;
    uint64_t recv_aggregate_total_ns;
    uint64_t recv_aggregate_min_ns;
    uint64_t recv_aggregate_max_ns;
    char output_path[PATH_LEN];
} xqc_mini_svr_file_state_t;

static xqc_mini_svr_file_state_t *xqc_mini_svr_get_file_state(
    xqc_mini_svr_user_stream_t *user_stream, int create);
static int xqc_mini_svr_reset_file_state(xqc_mini_svr_file_state_t *state,
    int stream_count, size_t total_size, const char *output_path,
    xqc_mini_svr_user_conn_t *user_conn, const char *request_path);
static void xqc_mini_svr_update_upload_progress(xqc_mini_svr_user_stream_t *user_stream);
static int xqc_mini_svr_prepare_output_file(xqc_mini_svr_user_stream_t *user_stream);//Post文件输出路径
static void xqc_mini_svr_mark_stream_complete(xqc_mini_svr_user_stream_t *user_stream);//检测流帧是否传输完成
static int xqc_mini_svr_time_cmp(const struct timeval *a, const struct timeval *b);
static double xqc_mini_svr_duration_ms(const struct timeval *start, const struct timeval *end);
static xqc_mini_svr_user_conn_t *xqc_mini_svr_get_conn_from_request(xqc_mini_svr_user_stream_t *user_stream);
static int xqc_mini_svr_resolve_file_path(
    xqc_mini_svr_user_conn_t *user_conn, const char *request_path,
    char *resolved, size_t resolved_len);
static int xqc_mini_svr_open_api_path(
    xqc_mini_svr_user_conn_t *user_conn, const char *request_path,
    int flags, mode_t mode);
static void xqc_mini_svr_prepare_text_response(xqc_mini_svr_user_stream_t *user_stream,
    int status, const char *fmt, ...);
static void xqc_mini_svr_compute_stream_segment(int stream_count, int stream_index,
    size_t total_size, size_t stream_range_size, size_t *offset, size_t *length);
static int xqc_mini_svr_prepare_file_response(xqc_mini_svr_user_stream_t *user_stream);
static void xqc_mini_svr_fill_generated_chunk(unsigned char *buf, size_t len,
    size_t offset);
static int xqc_mini_svr_resolve_upload_path(
    xqc_mini_svr_user_conn_t *user_conn, const char *request_path,
    char *resolved, size_t resolved_len);
static int xqc_mini_svr_get_target_file_size(xqc_mini_svr_user_stream_t *user_stream,
    size_t *file_size);
static void xqc_mini_svr_log_mp_active_state(xqc_mini_svr_user_conn_t *user_conn,
    const char *scene);
static void xqc_mini_svr_log_mp_active_state_force(xqc_mini_svr_user_conn_t *user_conn,
    const char *scene);
static uint64_t xqc_mini_svr_now_us(void);
static void xqc_mini_svr_get_diag_capture(xqc_mini_svr_user_conn_t *user_conn,
    xqc_mini_svr_get_diag_snapshot_t *snapshot);
static void xqc_mini_svr_get_diag_maybe_reset_phase(xqc_mini_svr_user_stream_t *user_stream);
static void xqc_mini_svr_log_get_submit_done(xqc_mini_svr_user_stream_t *user_stream);
static void xqc_mini_svr_log_get_path_response_done(xqc_mini_svr_user_stream_t *user_stream,
    double duration_ms, double mbps);


static void
xqc_mini_svr_copy_string(char *destination, size_t destination_size,
    const char *source)
{
    if (destination == NULL || destination_size == 0) {
        return;
    }
    if (source == NULL) {
        destination[0] = '\0';
        return;
    }

    size_t length = 0;
    while (length + 1 < destination_size && source[length] != '\0') {
        length++;
    }
    if (length > 0) {
        memcpy(destination, source, length);
    }
    destination[length] = '\0';
}


static size_t
xqc_mini_svr_get_stream_range_size(xqc_mini_svr_user_stream_t *user_stream)
{
    xqc_mini_svr_user_conn_t *user_conn = xqc_mini_svr_get_conn_from_request(user_stream);
    if (user_conn != NULL && user_conn->ctx != NULL
        && user_conn->ctx->args != NULL
        && user_conn->ctx->args->net_cfg.stream_range_size > 0) {
        return user_conn->ctx->args->net_cfg.stream_range_size;
    }

    return 4 * 1024 * 1024;
}


static void
xqc_mini_svr_log_response_send_stats(xqc_mini_svr_user_stream_t *user_stream)
{
    if (user_stream->send_logged
        || !user_stream->send_started
        || user_stream->response_body_sent < user_stream->response_body_len) {
        return;
    }

    gettimeofday(&user_stream->send_end_time, NULL);
    double duration_ms = xqc_mini_svr_duration_ms(&user_stream->send_start_time,
        &user_stream->send_end_time);
    double mbps = 0.0;

    if (duration_ms > 0.0) {
        mbps = (user_stream->response_body_len * 8.0)
            / (duration_ms * 1000.0);
        printf("[server] response body submitted to XQUIC. send_len=%zu bytes,"
               " time=%.3f ms, app_submit_mbps=%.3f"
               " metric=app_submit_not_delivered\n",
               user_stream->response_body_len, duration_ms, mbps);

    } else {
        printf("[server] response body submitted to XQUIC. send_len=%zu bytes,"
               " time=%.3f ms metric=app_submit_not_delivered"
               " (throughput unavailable)\n",
               user_stream->response_body_len, duration_ms);
    }

    if (user_stream->method == REQUEST_METHOD_GET && user_stream->use_file_response) {
        printf("[diag_server_io][response_done] stream=%d/%d offset=%zu len=%zu"
               " file_read_cnt=%" PRIu64 " file_read_bytes=%" PRIu64
               " file_read_max_us=%" PRIu64
               " h3_send_cnt=%" PRIu64 " h3_send_bytes=%" PRIu64
               " h3_send_max_us=%" PRIu64 "\n",
            user_stream->stream_index,
            user_stream->stream_count,
            user_stream->response_body_offset,
            user_stream->response_body_len,
            user_stream->file_read_count,
            user_stream->file_read_bytes,
            user_stream->file_read_max_us,
            user_stream->h3_send_count,
               user_stream->h3_send_bytes,
               user_stream->h3_send_max_us);

        xqc_mini_svr_log_get_submit_done(user_stream);
        xqc_mini_svr_log_get_path_response_done(user_stream, duration_ms, mbps);
    }

    user_stream->send_logged = 1;
}

static uint64_t
xqc_mini_svr_now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 * 1000 + (uint64_t)tv.tv_usec;
}

static uint64_t
xqc_mini_svr_monotonic_now_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000 * 1000 * 1000 + (uint64_t)ts.tv_nsec;
}

static void
xqc_mini_svr_record_latency(uint64_t *sample_count, uint64_t *sample_bytes,
    uint64_t *total_ns, uint64_t *min_ns, uint64_t *max_ns,
    uint64_t start_ns, uint64_t end_ns, size_t bytes)
{
    if (start_ns == 0 || end_ns < start_ns || bytes == 0) {
        return;
    }

    uint64_t latency_ns = end_ns - start_ns;
    if (*sample_count == 0 || latency_ns < *min_ns) {
        *min_ns = latency_ns;
    }
    if (latency_ns > *max_ns) {
        *max_ns = latency_ns;
    }
    (*sample_count)++;
    *sample_bytes += (uint64_t)bytes;
    *total_ns += latency_ns;
}

static void
xqc_mini_svr_merge_latency(uint64_t *sample_count, uint64_t *sample_bytes,
    uint64_t *total_ns, uint64_t *min_ns, uint64_t *max_ns,
    uint64_t source_count, uint64_t source_bytes, uint64_t source_total_ns,
    uint64_t source_min_ns, uint64_t source_max_ns)
{
    if (source_count == 0) {
        return;
    }

    if (*sample_count == 0 || source_min_ns < *min_ns) {
        *min_ns = source_min_ns;
    }
    if (source_max_ns > *max_ns) {
        *max_ns = source_max_ns;
    }
    *sample_count += source_count;
    *sample_bytes += source_bytes;
    *total_ns += source_total_ns;
}

static void
xqc_mini_svr_log_fragment_latency(const char *direction, const char *operation,
    uint64_t sample_count, uint64_t sample_bytes, uint64_t total_ns,
    uint64_t min_ns, uint64_t max_ns)
{
    if (sample_count == 0) {
        printf("[server_fragment_latency] direction=%s operation=%s"
               " sample_count=0 sample_bytes=0 processing_avg_us=unavailable\n",
            direction, operation);
        return;
    }

    printf("[server_fragment_latency] direction=%s operation=%s"
           " sample_count=%" PRIu64 " sample_bytes=%" PRIu64
           " processing_avg_us=%.3f processing_min_us=%.3f"
           " processing_max_us=%.3f metric=server_internal_processing\n",
        direction, operation, sample_count, sample_bytes,
        (double)total_ns / (double)sample_count / 1000.0,
        (double)min_ns / 1000.0, (double)max_ns / 1000.0);
}

static void
xqc_mini_svr_update_max_u64(uint64_t *value, uint64_t candidate)
{
    if (*value < candidate) {
        *value = candidate;
    }
}

static uint64_t
xqc_mini_svr_delta_u64(uint64_t end, uint64_t start)
{
    return end >= start ? end - start : 0;
}


static const xqc_mini_svr_path_diag_t *
xqc_mini_svr_get_diag_find_path(const xqc_mini_svr_get_diag_snapshot_t *snapshot,
    uint64_t path_id)
{
    if (snapshot == NULL || !snapshot->valid) {
        return NULL;
    }

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (snapshot->paths[i].valid && snapshot->paths[i].path_id == path_id) {
            return &snapshot->paths[i];
        }
    }

    return NULL;
}

static void
xqc_mini_svr_get_diag_capture(xqc_mini_svr_user_conn_t *user_conn,
    xqc_mini_svr_get_diag_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    if (user_conn == NULL || user_conn->ctx == NULL || user_conn->ctx->engine == NULL) {
        return;
    }

    xqc_conn_stats_t stats = xqc_conn_get_stats(user_conn->ctx->engine, &user_conn->cid);

    snapshot->valid = 1;
    snapshot->mp_state = stats.mp_state;
    snapshot->enable_mp = stats.enable_multipath;
    snapshot->send_count = stats.send_count;
    snapshot->recv_count = stats.recv_count;
    snapshot->lost_count = stats.lost_count;
    snapshot->spurious_loss_count = stats.spurious_loss_count;
    snapshot->srtt = (uint64_t)stats.srtt;
    snapshot->min_rtt = (uint64_t)stats.min_rtt;
    snapshot->inflight_bytes = stats.inflight_bytes;
    snapshot->total_app_bytes = stats.total_app_bytes;
    snapshot->standby_path_app_bytes = stats.standby_path_app_bytes;

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (stats.paths_info[i].path_id == UINT64_MAX) {
            continue;
        }

        xqc_mini_svr_path_diag_t *dst = &snapshot->paths[i];
        dst->valid = 1;
        dst->path_id = stats.paths_info[i].path_id;
        dst->path_state = stats.paths_info[i].path_state;
        dst->path_app_status = stats.paths_info[i].path_app_status;
        dst->path_srtt = stats.paths_info[i].path_srtt;
        dst->path_send_bytes = stats.paths_info[i].path_send_bytes;
        dst->path_recv_bytes = stats.paths_info[i].path_recv_bytes;
        dst->path_pkt_send_count = stats.paths_info[i].path_pkt_send_count;
        dst->path_pkt_recv_count = stats.paths_info[i].path_pkt_recv_count;
        dst->path_recv_effective_bytes = stats.paths_info[i].path_recv_effective_bytes;

        /* xqc_path_state_t: ACTIVE == 2 */
        if (dst->path_state == 2) {
            snapshot->active_path_cnt++;
        }
    }

    snapshot->both_active = snapshot->active_path_cnt >= 2;
}

static void
xqc_mini_svr_get_diag_maybe_reset_phase(xqc_mini_svr_user_stream_t *user_stream)
{
    if (user_stream == NULL || user_stream->method != REQUEST_METHOD_GET
        || !user_stream->use_file_response)
    {
        return;
    }

    xqc_mini_svr_user_conn_t *user_conn =
        xqc_mini_svr_get_conn_from_request(user_stream);
    if (user_conn == NULL) {
        return;
    }

    if (!user_conn->get_submit_active
        || (user_stream->stream_index == 0
            && user_conn->get_submit_streams > 0))
    {
        user_conn->get_submit_active = 1;
        user_conn->get_submit_expected_streams = 0;
        user_conn->get_submit_start_us = xqc_mini_svr_now_us();
        user_conn->get_submit_last_us = 0;
        user_conn->get_submit_streams = 0;
        user_conn->get_submit_bytes = 0;
        user_conn->get_split_sample_count = 0;
        user_conn->get_split_sample_bytes = 0;
        user_conn->get_split_total_ns = 0;
        user_conn->get_split_min_ns = 0;
        user_conn->get_split_max_ns = 0;
        printf("[diag_server_get_submit][reset] stream=%d/%d"
               " offset=%zu len=%zu metric=app_submit_not_delivered\n",
               user_stream->stream_index, user_stream->stream_count,
               user_stream->response_body_offset,
               user_stream->response_body_len);
    }

    if (user_stream->stream_count > user_conn->get_submit_expected_streams) {
        user_conn->get_submit_expected_streams = user_stream->stream_count;
    }
}

static void
xqc_mini_svr_log_get_submit_done(xqc_mini_svr_user_stream_t *user_stream)
{
    if (user_stream == NULL || user_stream->method != REQUEST_METHOD_GET) {
        return;
    }

    xqc_mini_svr_user_conn_t *user_conn =
        xqc_mini_svr_get_conn_from_request(user_stream);
    if (user_conn == NULL) {
        return;
    }
    if (!user_conn->get_submit_active) {
        xqc_mini_svr_get_diag_maybe_reset_phase(user_stream);
    }

    uint64_t now_us = xqc_mini_svr_now_us();
    user_conn->get_submit_last_us = now_us;
    user_conn->get_submit_streams++;
    user_conn->get_submit_bytes += user_stream->response_body_len;
    xqc_mini_svr_merge_latency(&user_conn->get_split_sample_count,
        &user_conn->get_split_sample_bytes, &user_conn->get_split_total_ns,
        &user_conn->get_split_min_ns, &user_conn->get_split_max_ns,
        user_stream->send_split_sample_count,
        user_stream->send_split_sample_bytes,
        user_stream->send_split_total_ns,
        user_stream->send_split_min_ns,
        user_stream->send_split_max_ns);
    if (user_stream->stream_count > user_conn->get_submit_expected_streams) {
        user_conn->get_submit_expected_streams = user_stream->stream_count;
    }

    double wall_time_ms = now_us >= user_conn->get_submit_start_us
        ? (double)(now_us - user_conn->get_submit_start_us) / 1000.0 : 0.0;
    double app_submit_mbps = wall_time_ms > 0.0
        ? (double)user_conn->get_submit_bytes * 8.0
            / (wall_time_ms * 1000.0)
        : 0.0;

    printf("[diag_server_get_submit][summary] stream=%d/%d"
           " completed=%" PRIu64 " expected=%d bytes=%" PRIu64
           " wall_time_ms=%.3f app_submit_mbps=%.3f"
           " metric=app_submit_not_delivered\n",
           user_stream->stream_index, user_stream->stream_count,
           user_conn->get_submit_streams,
           user_conn->get_submit_expected_streams,
           user_conn->get_submit_bytes, wall_time_ms, app_submit_mbps);

    if (user_conn->get_submit_expected_streams > 0
        && user_conn->get_submit_streams
            >= (uint64_t)user_conn->get_submit_expected_streams)
    {
        xqc_mini_svr_log_fragment_latency("send", "split",
            user_conn->get_split_sample_count,
            user_conn->get_split_sample_bytes,
            user_conn->get_split_total_ns,
            user_conn->get_split_min_ns,
            user_conn->get_split_max_ns);
        user_conn->get_submit_active = 0;
    }
}

static void
xqc_mini_svr_log_get_path_response_done(xqc_mini_svr_user_stream_t *user_stream,
    double duration_ms, double mbps)
{
    if (user_stream == NULL || user_stream->method != REQUEST_METHOD_GET
        || !user_stream->use_file_response || !user_stream->get_path_diag_started)
    {
        return;
    }

    xqc_mini_svr_user_conn_t *user_conn = xqc_mini_svr_get_conn_from_request(user_stream);
    xqc_mini_svr_get_diag_snapshot_t end_snapshot;
    xqc_mini_svr_get_diag_capture(user_conn, &end_snapshot);

    xqc_mini_svr_get_diag_snapshot_t *start = &user_stream->get_path_diag_start;
    if (!start->valid || !end_snapshot.valid) {
        return;
    }

    uint64_t total_path_send_delta = 0;
    uint64_t total_path_recv_delta = 0;

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (!end_snapshot.paths[i].valid) {
            continue;
        }

        const xqc_mini_svr_path_diag_t *start_path =
            xqc_mini_svr_get_diag_find_path(start, end_snapshot.paths[i].path_id);
        uint64_t start_send = start_path ? start_path->path_send_bytes : 0;
        uint64_t start_recv = start_path ? start_path->path_recv_bytes : 0;
        total_path_send_delta += xqc_mini_svr_delta_u64(
            end_snapshot.paths[i].path_send_bytes, start_send);
        total_path_recv_delta += xqc_mini_svr_delta_u64(
            end_snapshot.paths[i].path_recv_bytes, start_recv);
    }

    uint64_t conn_send_delta = xqc_mini_svr_delta_u64(end_snapshot.send_count,
        start->send_count);
    uint64_t conn_recv_delta = xqc_mini_svr_delta_u64(end_snapshot.recv_count,
        start->recv_count);
    uint64_t conn_lost_delta = xqc_mini_svr_delta_u64(end_snapshot.lost_count,
        start->lost_count);
    uint64_t app_delta = xqc_mini_svr_delta_u64(end_snapshot.total_app_bytes,
        start->total_app_bytes);
    double loss_rate_end = end_snapshot.send_count > 0
        ? (double)end_snapshot.lost_count * 100.0 / (double)end_snapshot.send_count
        : 0.0;
    double loss_rate_delta = conn_send_delta > 0
        ? (double)conn_lost_delta * 100.0 / (double)conn_send_delta
        : 0.0;

    printf("[diag_server_get_path][response_done] stream=%d/%d offset=%zu len=%zu"
           " start_both=%d end_both=%d start_active=%d end_active=%d"
           " start_mp_state=%d end_mp_state=%d enable_mp=%d"
           " duration_ms=%.3f app_submit_mbps=%.3f"
           " metric=app_submit_not_delivered"
           " conn_send_delta=%" PRIu64 " conn_recv_delta=%" PRIu64
           " conn_lost_delta=%" PRIu64 " loss_delta_rate=%.3f%% loss_rate_end=%.3f%%"
           " app_delta=%" PRIu64 " path_send_delta=%" PRIu64
           " path_recv_delta=%" PRIu64 " inflight_end=%" PRIu64
           " srtt_start=%" PRIu64 " srtt_end=%" PRIu64 "\n",
        user_stream->stream_index,
        user_stream->stream_count,
        user_stream->response_body_offset,
        user_stream->response_body_len,
        start->both_active,
        end_snapshot.both_active,
        start->active_path_cnt,
        end_snapshot.active_path_cnt,
        start->mp_state,
        end_snapshot.mp_state,
        end_snapshot.enable_mp,
        duration_ms,
        mbps,
        conn_send_delta,
        conn_recv_delta,
        conn_lost_delta,
        loss_rate_delta,
        loss_rate_end,
        app_delta,
        total_path_send_delta,
        total_path_recv_delta,
        end_snapshot.inflight_bytes,
        start->srtt,
        end_snapshot.srtt);

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (!end_snapshot.paths[i].valid) {
            continue;
        }

        const xqc_mini_svr_path_diag_t *start_path =
            xqc_mini_svr_get_diag_find_path(start, end_snapshot.paths[i].path_id);
        uint64_t start_send = start_path ? start_path->path_send_bytes : 0;
        uint64_t start_recv = start_path ? start_path->path_recv_bytes : 0;
        uint64_t start_send_pkts = start_path ? start_path->path_pkt_send_count : 0;
        uint64_t start_recv_pkts = start_path ? start_path->path_pkt_recv_count : 0;
        uint64_t start_eff = start_path ? start_path->path_recv_effective_bytes : 0;
        uint64_t start_srtt = start_path ? start_path->path_srtt : 0;
        uint64_t send_delta = xqc_mini_svr_delta_u64(
            end_snapshot.paths[i].path_send_bytes, start_send);
        uint64_t recv_delta = xqc_mini_svr_delta_u64(
            end_snapshot.paths[i].path_recv_bytes, start_recv);
        uint64_t send_pkts_delta = xqc_mini_svr_delta_u64(
            end_snapshot.paths[i].path_pkt_send_count, start_send_pkts);
        uint64_t recv_pkts_delta = xqc_mini_svr_delta_u64(
            end_snapshot.paths[i].path_pkt_recv_count, start_recv_pkts);
        uint64_t recv_eff_delta = xqc_mini_svr_delta_u64(
            end_snapshot.paths[i].path_recv_effective_bytes, start_eff);
        double send_share = total_path_send_delta > 0
            ? (double)send_delta * 100.0 / (double)total_path_send_delta
            : 0.0;

        printf("[diag_server_get_path][path_delta] stream=%d/%d idx=%zu"
               " id=%" PRIu64 " state=%u app_status=%u"
               " send_delta=%" PRIu64 " send_share=%.2f%%"
               " recv_delta=%" PRIu64 " recv_eff_delta=%" PRIu64
               " send_pkts_delta=%" PRIu64 " recv_pkts_delta=%" PRIu64
               " start_srtt=%" PRIu64 " end_srtt=%" PRIu64
               " end_send_bytes=%" PRIu64 " end_recv_bytes=%" PRIu64 "\n",
            user_stream->stream_index,
            user_stream->stream_count,
            i,
            end_snapshot.paths[i].path_id,
            end_snapshot.paths[i].path_state,
            end_snapshot.paths[i].path_app_status,
            send_delta,
            send_share,
            recv_delta,
            recv_eff_delta,
            send_pkts_delta,
            recv_pkts_delta,
            start_srtt,
            end_snapshot.paths[i].path_srtt,
            end_snapshot.paths[i].path_send_bytes,
            end_snapshot.paths[i].path_recv_bytes);
    }

}

static ssize_t
xqc_mini_svr_pread_file_chunk(xqc_mini_svr_user_stream_t *user_stream,
    unsigned char *buf, size_t len, off_t offset)
{
    xqc_usec_t start_us = xqc_mini_svr_now_us();
    ssize_t ret;

    do {
        ret = pread(user_stream->send_body_fd, buf, len, offset);
    } while (ret < 0 && errno == EINTR);

    xqc_usec_t end_us = xqc_mini_svr_now_us();
    user_stream->file_read_count++;
    if (ret > 0) {
        user_stream->file_read_bytes += (uint64_t)ret;
    }
    if (end_us >= start_us) {
        xqc_mini_svr_update_max_u64(&user_stream->file_read_max_us,
            (uint64_t)(end_us - start_us));
    }

    return ret;
}

static ssize_t
xqc_mini_svr_h3_send_body_record(xqc_mini_svr_user_stream_t *user_stream,
    unsigned char *data, size_t len, int fin)
{
    uint64_t start_ns = xqc_mini_svr_monotonic_now_ns();
    ssize_t ret = xqc_h3_request_send_body(user_stream->h3_request, data, len, fin);
    uint64_t end_ns = xqc_mini_svr_monotonic_now_ns();

    if (user_stream->method == REQUEST_METHOD_GET && user_stream->use_file_response) {
        user_stream->h3_send_count++;
        if (ret > 0) {
            user_stream->h3_send_bytes += (uint64_t)ret;
            xqc_mini_svr_record_latency(&user_stream->send_split_sample_count,
                &user_stream->send_split_sample_bytes,
                &user_stream->send_split_total_ns,
                &user_stream->send_split_min_ns,
                &user_stream->send_split_max_ns,
                start_ns, end_ns, (size_t)ret);
        }
        if (start_ns != 0 && end_ns >= start_ns) {
            xqc_mini_svr_update_max_u64(&user_stream->h3_send_max_us,
                (end_ns - start_ns) / 1000);
        }
    }

    return ret;
}

static void
xqc_mini_svr_log_mp_active_state_ex(xqc_mini_svr_user_conn_t *user_conn,
    const char *scene, int force)
{
    if (user_conn == NULL || user_conn->ctx == NULL || user_conn->ctx->engine == NULL) {
        return;
    }

    uint64_t now_us = xqc_mini_svr_now_us();
    if (!force
        && user_conn->last_mp_state_log_us != 0
        && now_us - user_conn->last_mp_state_log_us < 500 * 1000)
    {
        return;
    }

    user_conn->last_mp_state_log_us = now_us;

    xqc_conn_stats_t stats = xqc_conn_get_stats(user_conn->ctx->engine, &user_conn->cid);
    int active_path_cnt = 0;

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (stats.paths_info[i].path_id == UINT64_MAX) {
            continue;
        }

        /* xqc_path_state_t: ACTIVE == 2 */
        if (stats.paths_info[i].path_state == 2) {
            active_path_cnt++;
        }
    }

    int both_active = (active_path_cnt >= 2);

    printf("[mini_mp][%s] both_active=%d, active_path_cnt=%d, mp_state=%d, enable_mp=%d, conn_info=%s\n",
           scene ? scene : "unknown", both_active, active_path_cnt,
           stats.mp_state, stats.enable_multipath, stats.conn_info);

    double loss_rate = stats.send_count > 0
        ? (double)stats.lost_count * 100.0 / (double)stats.send_count
        : 0.0;

    printf("[diag_server_quic][%s] send=%u recv=%u lost=%u loss_rate=%.3f%%"
           " spurious_loss=%u srtt_us=%" PRIu64 " min_rtt_us=%" PRIu64
           " inflight=%" PRIu64 " mp_state=%d enable_mp=%d"
           " total_app_bytes=%" PRIu64 " standby_app_bytes=%" PRIu64 "\n",
           scene ? scene : "unknown",
           stats.send_count,
           stats.recv_count,
           stats.lost_count,
           loss_rate,
           stats.spurious_loss_count,
           (uint64_t)stats.srtt,
           (uint64_t)stats.min_rtt,
           stats.inflight_bytes,
           stats.mp_state,
           stats.enable_multipath,
           stats.total_app_bytes,
           stats.standby_path_app_bytes);

    for (size_t i = 0; i < XQC_MAX_PATHS_COUNT; i++) {
        if (stats.paths_info[i].path_id == UINT64_MAX) {
            continue;
        }

        printf("[mini_mp][%s] path=%" PRIu64 ", state=%u, app_status=%u, srtt=%" PRIu64
               ", send_bytes=%" PRIu64 ", recv_bytes=%" PRIu64 "\n",
               scene ? scene : "unknown",
               stats.paths_info[i].path_id,
               stats.paths_info[i].path_state,
               stats.paths_info[i].path_app_status,
               stats.paths_info[i].path_srtt,
               stats.paths_info[i].path_send_bytes,
               stats.paths_info[i].path_recv_bytes);

        printf("[diag_server_path][%s] idx=%zu id=%" PRIu64
               " state=%u app_status=%u srtt_us=%" PRIu64
               " send_bytes=%" PRIu64 " recv_bytes=%" PRIu64
               " send_pkts=%" PRIu64 " recv_pkts=%" PRIu64
               " recv_eff_bytes=%" PRIu64 "\n",
               scene ? scene : "unknown",
               i,
               stats.paths_info[i].path_id,
               stats.paths_info[i].path_state,
               stats.paths_info[i].path_app_status,
               stats.paths_info[i].path_srtt,
               stats.paths_info[i].path_send_bytes,
               stats.paths_info[i].path_recv_bytes,
               stats.paths_info[i].path_pkt_send_count,
               stats.paths_info[i].path_pkt_recv_count,
               stats.paths_info[i].path_recv_effective_bytes);
    }
}

static void
xqc_mini_svr_log_mp_active_state(xqc_mini_svr_user_conn_t *user_conn, const char *scene)
{
    /* Keep transfer callbacks free of periodic stdout and transport side effects. */
    (void)user_conn;
    (void)scene;
}

static void
xqc_mini_svr_log_mp_active_state_force(xqc_mini_svr_user_conn_t *user_conn,
    const char *scene)
{
    xqc_mini_svr_log_mp_active_state_ex(user_conn, scene, 1);
}



static int
xqc_mini_svr_time_cmp(const struct timeval *a, const struct timeval *b)
{
    if (a->tv_sec < b->tv_sec) {
        return -1;
    }
    if (a->tv_sec > b->tv_sec) {
        return 1;
    }
    if (a->tv_usec < b->tv_usec) {
        return -1;
    }
    if (a->tv_usec > b->tv_usec) {
        return 1;
    }
    return 0;
}

static double
xqc_mini_svr_duration_ms(const struct timeval *start, const struct timeval *end)
{
    return (end->tv_sec - start->tv_sec) * 1000.0
        + (end->tv_usec - start->tv_usec) / 1000.0;
}
static xqc_mini_svr_user_conn_t *
xqc_mini_svr_get_conn_from_request(xqc_mini_svr_user_stream_t *user_stream)
{
    if (user_stream == NULL) {
        return NULL;
    }
    if (user_stream->user_conn != NULL) {
        return user_stream->user_conn;
    }
    user_stream->user_conn =
        (xqc_mini_svr_user_conn_t *)xqc_h3_get_conn_user_data_by_request(
            user_stream->h3_request);
    return user_stream->user_conn;
}

static void
xqc_mini_svr_defer_request(xqc_mini_svr_user_conn_t *user_conn,
    xqc_mini_svr_user_stream_t *user_stream,
    xqc_request_notify_flag_t flag)
{
    user_stream->pending_read_flags |= (unsigned int)flag;
    if (user_stream->pending_for_serve) {
        return;
    }

    user_stream->pending_for_serve = 1;
    user_stream->pending_next = NULL;
    if (user_conn->pending_stream_tail == NULL) {
        user_conn->pending_stream_head = user_stream;
    } else {
        user_conn->pending_stream_tail->pending_next = user_stream;
    }
    user_conn->pending_stream_tail = user_stream;
}

static void
xqc_mini_svr_unlink_deferred_request(xqc_mini_svr_user_conn_t *user_conn,
    xqc_mini_svr_user_stream_t *user_stream)
{
    if (user_conn == NULL || user_stream == NULL
        || !user_stream->pending_for_serve)
    {
        return;
    }

    xqc_mini_svr_user_stream_t **cursor =
        &user_conn->pending_stream_head;
    while (*cursor != NULL) {
        if (*cursor == user_stream) {
            *cursor = user_stream->pending_next;
            if (user_conn->pending_stream_tail == user_stream) {
                user_conn->pending_stream_tail = NULL;
                for (xqc_mini_svr_user_stream_t *tail =
                        user_conn->pending_stream_head;
                    tail != NULL; tail = tail->pending_next)
                {
                    user_conn->pending_stream_tail = tail;
                }
            }
            user_stream->pending_next = NULL;
            user_stream->pending_for_serve = 0;
            user_stream->pending_read_flags = 0;
            return;
        }
        cursor = &(*cursor)->pending_next;
    }

    user_stream->pending_next = NULL;
    user_stream->pending_for_serve = 0;
    user_stream->pending_read_flags = 0;
}

int
xqc_mini_svr_resume_pending_requests(xqc_mini_svr_user_conn_t *user_conn)
{
    if (user_conn == NULL || !user_conn->serve_configured
        || user_conn->serve_cancelled)
    {
        return XQC_ERROR;
    }

    while (user_conn->pending_stream_head != NULL) {
        xqc_mini_svr_user_stream_t *user_stream =
            user_conn->pending_stream_head;
        user_conn->pending_stream_head = user_stream->pending_next;
        if (user_conn->pending_stream_head == NULL) {
            user_conn->pending_stream_tail = NULL;
        }

        xqc_request_notify_flag_t flags =
            (xqc_request_notify_flag_t)user_stream->pending_read_flags;
        user_stream->pending_next = NULL;
        user_stream->pending_for_serve = 0;
        user_stream->pending_read_flags = 0;

        int ret = xqc_mini_svr_h3_request_read_notify(
            user_stream->h3_request, flags, user_stream);
        if (ret < 0) {
            return ret;
        }
    }
    return XQC_OK;
}

void
xqc_mini_svr_release_connection_state(xqc_mini_svr_user_conn_t *user_conn)
{
    if (user_conn == NULL || user_conn->file_task_state == NULL) {
        return;
    }

    xqc_mini_svr_file_state_t *state =
        (xqc_mini_svr_file_state_t *)user_conn->file_task_state;
    free(state->stream_done);
    free(state->stream_progress);
    state->stream_done = NULL;
    state->stream_progress = NULL;
    free(state);
    user_conn->file_task_state = NULL;
}

static xqc_mini_svr_file_state_t *
xqc_mini_svr_get_file_state(xqc_mini_svr_user_stream_t *user_stream,
    int create)
{
    xqc_mini_svr_user_conn_t *user_conn =
        xqc_mini_svr_get_conn_from_request(user_stream);
    if (user_conn == NULL) {
        return NULL;
    }

    if (user_conn->file_task_state == NULL && create) {
        user_conn->file_task_state = calloc(1,
            sizeof(xqc_mini_svr_file_state_t));
    }
    return (xqc_mini_svr_file_state_t *)user_conn->file_task_state;
}
/*用于Get方法的响应发送文件路径 */
static int
xqc_mini_svr_relative_path_is_safe(const char *path)
{
    if (path == NULL || path[0] == '\0' || strchr(path, '\\') != NULL) {
        return 0;
    }
    const char *component = path;
    while (*component != '\0') {
        const char *end = strchr(component, '/');
        size_t length = end != NULL
            ? (size_t)(end - component) : strlen(component);
        if (length == 0
            || (length == 1 && component[0] == '.')
            || (length == 2 && component[0] == '.' && component[1] == '.'))
        {
            return 0;
        }
        if (end == NULL) {
            break;
        }
        component = end + 1;
    }
    return 1;
}

static int
xqc_mini_svr_open_api_path(
    xqc_mini_svr_user_conn_t *user_conn, const char *request_path,
    int flags, mode_t mode)
{
#if defined(XQC_SYS_WINDOWS)
    (void)user_conn;
    (void)request_path;
    (void)flags;
    (void)mode;
    errno = EOPNOTSUPP;
    return -1;
#else
    if (user_conn == NULL || !user_conn->api_owned
        || !user_conn->serve_configured || user_conn->serve_cancelled
        || user_conn->serve_root_fd < 0 || request_path == NULL)
    {
        errno = EACCES;
        return -1;
    }

    const char *target = request_path;
    while (*target == '/') {
        target++;
    }
    if (!xqc_mini_svr_relative_path_is_safe(target)) {
        errno = EACCES;
        return -1;
    }

    int directory_fd = dup(user_conn->serve_root_fd);
    if (directory_fd < 0) {
        return -1;
    }

    const char *component = target;
    for (;;) {
        const char *separator = strchr(component, '/');
        size_t component_len = separator != NULL
            ? (size_t)(separator - component) : strlen(component);
        if (component_len == 0 || component_len >= PATH_LEN) {
            close(directory_fd);
            errno = ENAMETOOLONG;
            return -1;
        }

        char name[PATH_LEN];
        memcpy(name, component, component_len);
        name[component_len] = '\0';
        if (separator == NULL) {
            int file_fd = openat(directory_fd, name,
                flags | O_NOFOLLOW | O_CLOEXEC, mode);
            int saved_errno = errno;
            close(directory_fd);
            if (file_fd < 0) {
                errno = saved_errno;
            }
            return file_fd;
        }

        int next_fd = openat(directory_fd, name,
            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        int saved_errno = errno;
        close(directory_fd);
        if (next_fd < 0) {
            errno = saved_errno;
            return -1;
        }
        directory_fd = next_fd;
        component = separator + 1;
    }
#endif
}

static int
xqc_mini_svr_header_name_is(const xqc_http_header_t *header,
    const char *name)
{
    size_t name_len = strlen(name);
    return header != NULL && header->name.iov_len == name_len
        && strncasecmp((const char *)header->name.iov_base,
            name, name_len) == 0;
}

static int
xqc_mini_svr_parse_u64_header(const xqc_http_header_t *header,
    uint64_t *value)
{
    if (header == NULL || value == NULL || header->value.iov_len == 0
        || header->value.iov_len >= 64)
    {
        return XQC_ERROR;
    }

    char buffer[64];
    size_t length = header->value.iov_len;
    memcpy(buffer, header->value.iov_base, length);
    buffer[length] = '\0';
    for (size_t i = 0; i < length; i++) {
        if (buffer[i] < '0' || buffer[i] > '9') {
            return XQC_ERROR;
        }
    }

    char *end = NULL;
    errno = 0;
    unsigned long long parsed = strtoull(buffer, &end, 10);
    if (errno != 0 || end != buffer + length) {
        return XQC_ERROR;
    }
    *value = (uint64_t)parsed;
    return XQC_OK;
}

static int
xqc_mini_svr_resolve_file_path(
    xqc_mini_svr_user_conn_t *user_conn, const char *request_path,
    char *resolved, size_t resolved_len)
{
    if (resolved_len == 0) {
        return XQC_ERROR;
    }

    resolved[0] = '\0';
    if (user_conn == NULL || user_conn->ctx == NULL
        || user_conn->ctx->args == NULL)
    {
        return XQC_ERROR;
    }
    xqc_mini_svr_ctx_t *ctx = user_conn->ctx;

    const char *target = request_path;
    if (user_conn->api_owned) {
        if (!user_conn->serve_configured) {
            return XQC_ERROR;
        }
        while (target != NULL && *target == '/') {
            target++;
        }
        if (!xqc_mini_svr_relative_path_is_safe(target)) {
            return XQC_ERROR;
        }
        int written = snprintf(resolved, resolved_len, "%s/%s",
            user_conn->serve_root_path, target);
        return written >= 0 && (size_t)written < resolved_len
            ? XQC_OK : XQC_ERROR;
    }

    if (target == NULL || target[0] == '\0' || strcmp(target, "/") == 0) {
        target = ctx->args->env_cfg.default_send_file;
        if (target[0] == '\0') {
            target = SERVER_DEFAULT_FILE;
        }
    } else {
        while (*target == '/') {
            target++;
        }
        if (*target == '\0') {
            target = ctx->args->env_cfg.default_send_file;
        }
    }

    if (target[0] == '/') {
        xqc_mini_svr_copy_string(resolved, resolved_len, target);
        return XQC_OK;
    }

    int written = snprintf(resolved, resolved_len, "%s/%s",
        ctx->args->env_cfg.data_dir, target);
    return written >= 0 && (size_t)written < resolved_len
        ? XQC_OK : XQC_ERROR;
}

/*组织响应报文*/
static void
xqc_mini_svr_prepare_text_response(xqc_mini_svr_user_stream_t *user_stream,
    int status, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(user_stream->static_response, sizeof(user_stream->static_response),
        fmt ? fmt : "", args);
    va_end(args);

    user_stream->static_response_len = strlen(user_stream->static_response);
    user_stream->response_body_len = user_stream->static_response_len;
    user_stream->response_body_sent = 0;
    user_stream->buffered_len = 0;
    user_stream->buffered_sent = 0;
    user_stream->use_file_response = 0;
    user_stream->use_generated_response = 0;
    user_stream->requested_generated_length = 0;
    user_stream->response_status = status;
    user_stream->response_content_type = "text/plain";
    user_stream->response_prepared = 1;
}

static int
xqc_mini_svr_prepare_file_response(xqc_mini_svr_user_stream_t *user_stream)
{
    xqc_mini_svr_user_conn_t *user_conn = xqc_mini_svr_get_conn_from_request(user_stream);
    if (user_conn == NULL || user_conn->ctx == NULL) {
        return XQC_ERROR;
    }

    if (xqc_mini_svr_resolve_file_path(user_conn,
            user_stream->request_path, user_stream->resolved_path,
            sizeof(user_stream->resolved_path)) != XQC_OK)
    {
        xqc_mini_svr_prepare_text_response(user_stream, 403,
            "invalid or unconfigured remote path");
        return XQC_OK;
    }

    size_t resp_len = user_stream->requested_generated_length;
    int file_fd = user_conn->api_owned
        ? xqc_mini_svr_open_api_path(user_conn, user_stream->request_path,
            O_RDONLY, 0)
        : open(user_stream->resolved_path, O_RDONLY);
    if (file_fd < 0) {
        xqc_mini_svr_prepare_text_response(user_stream,
            errno == EACCES || errno == ELOOP ? 403 : 404,
            "file %s not found", user_stream->resolved_path);
        return XQC_OK;
    }
    struct stat st;
    if (fstat(file_fd, &st) != 0 || !S_ISREG(st.st_mode)
        || st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX)
    {
        close(file_fd);
        xqc_mini_svr_prepare_text_response(user_stream, 404,
            "file %s not found", user_stream->resolved_path);
        return XQC_OK;
    }

    if (resp_len == 0) {
        resp_len = (size_t)st.st_size;
    }

    size_t file_size = (size_t)st.st_size;
    if (user_stream->response_body_offset > file_size) {
        resp_len = 0;
    } else if (resp_len > file_size - user_stream->response_body_offset) {
        resp_len = file_size - user_stream->response_body_offset;
    }

    user_stream->send_body_fd = file_fd;

    user_stream->response_body_len = resp_len;
    user_stream->response_body_sent = 0;
    if (!user_stream->metadata_parsed) {
        user_stream->response_body_offset = 0;
    }
    user_stream->buffered_len = 0;
    user_stream->buffered_sent = 0;
    user_stream->use_file_response = 1;
    user_stream->use_generated_response = 0;
    user_stream->response_status = 200;
    user_stream->response_content_type = "application/octet-stream";
    user_stream->response_prepared = 1;

    // printf("[server] serving generated data for %s (%zu bytes, offset=%zu)\n",
    //     user_stream->resolved_path, user_stream->response_body_len,
    //     user_stream->response_body_offset);
    printf("[server] serving file data for %s (%zu bytes)\n",
        user_stream->resolved_path, user_stream->response_body_len);


    return XQC_OK;
}
static int
xqc_mini_svr_get_target_file_size(xqc_mini_svr_user_stream_t *user_stream,
    size_t *file_size)
{
    if (user_stream == NULL || file_size == NULL) {
        return XQC_ERROR;
    }

    xqc_mini_svr_user_conn_t *user_conn = xqc_mini_svr_get_conn_from_request(user_stream);
    if (user_conn == NULL || user_conn->ctx == NULL) {
        return XQC_ERROR;
    }

    if (xqc_mini_svr_resolve_file_path(user_conn,
            user_stream->request_path, user_stream->resolved_path,
            sizeof(user_stream->resolved_path)) != XQC_OK)
    {
        return XQC_ERROR;
    }

    int file_fd = user_conn->api_owned
        ? xqc_mini_svr_open_api_path(user_conn, user_stream->request_path,
            O_RDONLY, 0)
        : open(user_stream->resolved_path, O_RDONLY);
    if (file_fd < 0) {
        return XQC_ERROR;
    }

    struct stat st;
    int ret = fstat(file_fd, &st);
    close(file_fd);
    if (ret != 0 || !S_ISREG(st.st_mode) || st.st_size < 0
        || (uintmax_t)st.st_size > SIZE_MAX)
    {
        return XQC_ERROR;
    }

    *file_size = (size_t)st.st_size;
    return XQC_OK;
}



static int
xqc_mini_svr_reset_file_state(xqc_mini_svr_file_state_t *state,
    int stream_count, size_t total_size, const char *output_path,
    xqc_mini_svr_user_conn_t *user_conn, const char *request_path)
{
    if (state == NULL || stream_count <= 0
        || stream_count > XQC_MINI_SVR_MAX_TRACKED_FRAGMENTS)
    {
        return XQC_ERROR;
    }

    unsigned char *stream_done = calloc((size_t)stream_count,
        sizeof(*stream_done));
    size_t *stream_progress = calloc((size_t)stream_count,
        sizeof(*stream_progress));
    if (stream_done == NULL || stream_progress == NULL) {
        free(stream_done);
        free(stream_progress);
        return XQC_ERROR;
    }

    free(state->stream_done);
    free(state->stream_progress);
    state->stream_done = stream_done;
    state->stream_progress = stream_progress;
    state->stream_capacity = (size_t)stream_count;
    state->initialized = 1;
    state->generation++;
    state->expected_streams = stream_count;
    state->total_size = total_size;
    state->total_received = 0;
    state->completed_streams = 0;
    state->earliest_start.tv_sec = 0;
    state->earliest_start.tv_usec = 0;
    state->latest_end.tv_sec = 0;
    state->latest_end.tv_usec = 0;
    state->recv_aggregate_sample_count = 0;
    state->recv_aggregate_sample_bytes = 0;
    state->recv_aggregate_total_ns = 0;
    state->recv_aggregate_min_ns = 0;
    state->recv_aggregate_max_ns = 0;
    if (output_path != NULL && output_path[0] != '\0') {
        xqc_mini_svr_copy_string(state->output_path,
            sizeof(state->output_path), output_path);
    } else {
        state->output_path[0] = '\0';
    }
    const char *output_file = state->output_path[0] != '\0'
        ? state->output_path
        : SERVER_OUTPUT_FILE;
#if defined(XQC_SYS_WINDOWS)
    FILE *fp = fopen(output_file, "wb");
    if (fp == NULL
        || (total_size > 0
            && (fseek(fp, (long)(total_size - 1), SEEK_SET) != 0
                || fputc('\0', fp) == EOF)))
    {
        if (fp != NULL) {
            fclose(fp);
        }
        perror("fopen/fseek");
        printf("[error] failed to prepare output file '%s'\n", output_file);
        state->initialized = 0;
        return XQC_ERROR;
    }
    fclose(fp);
#else
    if ((uintmax_t)total_size > (uintmax_t)INT64_MAX) {
        state->initialized = 0;
        return XQC_ERROR;
    }
    int output_fd = user_conn != NULL && user_conn->api_owned
        ? xqc_mini_svr_open_api_path(user_conn, request_path,
            O_RDWR | O_CREAT | O_TRUNC, 0644)
        : open(output_file, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (output_fd < 0
        || ftruncate(output_fd, (off_t)total_size) != 0)
    {
        int saved_errno = errno;
        if (output_fd >= 0) {
            close(output_fd);
        }
        errno = saved_errno;
        perror("open/ftruncate");
        printf("[error] failed to prepare output file '%s'\n", output_file);
        state->initialized = 0;
        return XQC_ERROR;
    }
    close(output_fd);
#endif

    printf("[server] prepared output file '%s' for totaling %zu bytes\n",
        output_file, total_size);
    return XQC_OK;
}
static int
xqc_mini_svr_resolve_upload_path(
    xqc_mini_svr_user_conn_t *user_conn, const char *request_path,
    char *resolved, size_t resolved_len)
{
    if (resolved_len == 0) {
        return XQC_ERROR;
    }
    resolved[0] = '\0';
    if (user_conn == NULL || user_conn->ctx == NULL
        || user_conn->ctx->args == NULL)
    {
        return XQC_ERROR;
    }
    if (user_conn->api_owned) {
        return xqc_mini_svr_resolve_file_path(user_conn, request_path,
            resolved, resolved_len);
    }

    xqc_mini_svr_ctx_t *ctx = user_conn->ctx;
    const char *target = ctx->args->env_cfg.upload_path;
    if (target == NULL || target[0] == '\0') {
        target = SERVER_OUTPUT_FILE;
    }
    if (target[0] == '/') {
        xqc_mini_svr_copy_string(resolved, resolved_len, target);
        return XQC_OK;
    }
    int written = snprintf(resolved, resolved_len, "%s/%s",
        ctx->args->env_cfg.data_dir, target);
    return written >= 0 && (size_t)written < resolved_len
        ? XQC_OK : XQC_ERROR;
}


static void
xqc_mini_svr_compute_stream_segment(int stream_count, int stream_index,
    size_t total_size, size_t stream_range_size, size_t *offset, size_t *length)
{
    (void)stream_count;
    if (offset == NULL || length == NULL) {
        return;
    }
    if (stream_index < 0 || total_size == 0) {
        *offset = 0;
        *length = 0;
        return;
    }
    // if (stream_index < 0) {
    //     stream_index = 0;
    // }
    // if (stream_index >= stream_count) {
    //     stream_index = stream_count - 1;
    // }
    // size_t base = total_size / (size_t)stream_count;
    // size_t remainder = total_size % (size_t)stream_count;
    // size_t start = base * (size_t)stream_index;
    // if ((size_t)stream_index < remainder) {
    //     start += (size_t)stream_index;
    // } else {
    //     start += remainder;
    size_t start = (size_t)stream_index * stream_range_size;
    if (start >= total_size) {
        *offset = total_size;
        *length = 0;
        return;
    }
    // size_t len = base + ((size_t)stream_index < remainder ? 1 : 0);
    size_t remaining = total_size - start;
    size_t len = remaining < stream_range_size
        ? remaining : stream_range_size;

    *offset = start;
    *length = len;
}


static int
xqc_mini_svr_prepare_output_file(xqc_mini_svr_user_stream_t *user_stream)
{
    xqc_mini_svr_user_conn_t *user_conn = xqc_mini_svr_get_conn_from_request(user_stream);
    char output_path[PATH_LEN];
    if (xqc_mini_svr_resolve_upload_path(user_conn,
            user_stream->request_path, output_path, sizeof(output_path))
        != XQC_OK)
    {
        printf("[error] invalid or unconfigured upload path\n");
        return XQC_ERROR;
    }
    const char *output_file = output_path[0] != '\0' ? output_path : SERVER_OUTPUT_FILE;
    int tracked_stream_count = user_stream->stream_count;
    if (tracked_stream_count <= 0
        || tracked_stream_count > XQC_MINI_SVR_MAX_TRACKED_FRAGMENTS)
    {
        printf("[error] invalid tracked stream count %d (max %d)\n",
            tracked_stream_count, XQC_MINI_SVR_MAX_TRACKED_FRAGMENTS);
        return XQC_ERROR;
    }

    xqc_mini_svr_file_state_t *state =
        xqc_mini_svr_get_file_state(user_stream, 1);
    if (state == NULL) {
        return XQC_ERROR;
    }

    if (!state->initialized
        || state->expected_streams != tracked_stream_count
        || state->total_size != user_stream->total_file_size
        || strncmp(state->output_path, output_file,
            sizeof(state->output_path)) != 0
        || state->completed_streams >= state->expected_streams)
    {
        if (xqc_mini_svr_reset_file_state(state, tracked_stream_count,
                user_stream->total_file_size, output_file, user_conn,
                user_stream->request_path) != XQC_OK)
        {
            printf("[error] failed to initialize upload task state\n");
            return XQC_ERROR;
        }
    }

    user_stream->file_generation = state->generation;

    if (user_stream->recv_body_fd < 0) {
        user_stream->recv_body_fd = user_conn != NULL && user_conn->api_owned
            ? xqc_mini_svr_open_api_path(user_conn,
                user_stream->request_path, O_RDWR, 0)
            : open(output_file, O_RDWR);
        if (user_stream->recv_body_fd < 0) {
            perror("open");
            printf("[error] failed to open output file '%s'\n", output_file);
            return XQC_ERROR;
        }
    }

    return XQC_OK;
}
static void
xqc_mini_svr_fill_generated_chunk(unsigned char *buf, size_t len, size_t offset)
{
    if (buf == NULL || len == 0) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        size_t global_index = offset + i;
        buf[i] = (global_index % 2 == 0) ? '0' : '1';
    }
}


static void
xqc_mini_svr_update_upload_progress(xqc_mini_svr_user_stream_t *user_stream)
{
    xqc_mini_svr_file_state_t *state =
        xqc_mini_svr_get_file_state(user_stream, 0);
    if (state == NULL) {
        return;
    }
    if (user_stream->stream_index < 0
        || (size_t)user_stream->stream_index >= state->stream_capacity) {
        return;
    }

    if (user_stream->file_generation != state->generation) {
        return;
    }

    size_t prev = state->stream_progress[user_stream->stream_index];
    if (user_stream->recv_body_len > prev) {
        state->total_received += user_stream->recv_body_len - prev;
        state->stream_progress[user_stream->stream_index] =
            user_stream->recv_body_len;
    }

    if (state->total_size > 0) {
        double progress = (double)state->total_received * 100.0
            / state->total_size;
        if (progress > 100.0) {
            progress = 100.0;
        }
        printf("\r[upload] %.2f%%", progress);
        fflush(stdout);
        if (state->total_received >= state->total_size) {
            printf("\n");
        }
    }
}

static void
xqc_mini_svr_mark_stream_complete(xqc_mini_svr_user_stream_t *user_stream)
{
    xqc_mini_svr_file_state_t *state =
        xqc_mini_svr_get_file_state(user_stream, 0);
    if (state == NULL) {
        return;
    }
    if (user_stream->stream_index < 0
        || (size_t)user_stream->stream_index >= state->stream_capacity) {
        return;
    }

    if (user_stream->file_generation != state->generation) {
        return;
    }

    if (!state->stream_done[user_stream->stream_index]) {
        state->stream_done[user_stream->stream_index] = 1;
        

        if (state->completed_streams == 0) {
            state->earliest_start = user_stream->start_time;
            state->latest_end = user_stream->end_time;

        } else {
            if (xqc_mini_svr_time_cmp(&user_stream->start_time,
                    &state->earliest_start) < 0) {
                state->earliest_start = user_stream->start_time;
            }
            if (xqc_mini_svr_time_cmp(&user_stream->end_time,
                    &state->latest_end) > 0) {
                state->latest_end = user_stream->end_time;
            }
        }

        xqc_mini_svr_merge_latency(
            &state->recv_aggregate_sample_count,
            &state->recv_aggregate_sample_bytes,
            &state->recv_aggregate_total_ns,
            &state->recv_aggregate_min_ns,
            &state->recv_aggregate_max_ns,
            user_stream->recv_aggregate_sample_count,
            user_stream->recv_aggregate_sample_bytes,
            user_stream->recv_aggregate_total_ns,
            user_stream->recv_aggregate_min_ns,
            user_stream->recv_aggregate_max_ns);
        state->completed_streams++;
        printf("[server] segment %d complete (%zu bytes at offset %zu)\n",
            user_stream->stream_index, user_stream->recv_body_len,
            user_stream->stream_offset);

        if (state->completed_streams >= state->expected_streams) {
            printf("[server] all %d segments received, assembled size %zu bytes\n",
                state->expected_streams, state->total_size);
            double duration_ms = xqc_mini_svr_duration_ms(&state->earliest_start,
                &state->latest_end);
            if (duration_ms > 0.0) {
                double mbps = (state->total_received * 8.0)
                    / (duration_ms * 1000.0);
                printf("[server] aggregate throughput: %.3f Mbps over %.3f ms (%zu bytes)\n",
                    mbps, duration_ms, state->total_received);
            } else {
                printf("[server] aggregate throughput unavailable (duration %.3f ms)\n",
                    duration_ms);
            }
            xqc_mini_svr_log_fragment_latency("recv", "aggregate",
                state->recv_aggregate_sample_count,
                state->recv_aggregate_sample_bytes,
                state->recv_aggregate_total_ns,
                state->recv_aggregate_min_ns,
                state->recv_aggregate_max_ns);
        }
    }
}

static const char *line_break = "\n";

/**
 * @brief engine callbacks to trigger engine main logic 
 */
void
xqc_mini_svr_engine_cb(int fd, short what, void *arg)
{
    xqc_mini_svr_ctx_t *ctx = (xqc_mini_svr_ctx_t *) arg;

    xqc_engine_main_logic(ctx->engine);
}

/**
 * @brief callbacks to set timer of engine callbacks
 */
void
xqc_mini_svr_set_event_timer(xqc_msec_t wake_after, void *arg)
{
    xqc_mini_svr_ctx_t *ctx = (xqc_mini_svr_ctx_t *)arg;

    struct timeval tv;
    tv.tv_sec = wake_after / 1000000;
    tv.tv_usec = wake_after % 1000000;
    event_add(ctx->ev_engine, &tv);
}

int
xqc_mini_svr_open_log_file(void *arg)
{
    xqc_mini_svr_ctx_t *ctx = (xqc_mini_svr_ctx_t*)arg;
    return open(ctx->args->env_cfg.log_path, (O_WRONLY | O_APPEND | O_CREAT), 0644);
}

static void
xqc_mini_svr_write_zlog(xqc_mini_svr_ctx_t *ctx, xqc_log_level_t lvl, const void *buf, size_t size)
{
    if (ctx->zlog_cat == NULL) {
        return;
    }

    int api_level = xqc_mini_log_level_from_xqc(lvl);
    if (ctx->args->env_cfg.zlog_level_filter_enabled
        && api_level < ctx->args->env_cfg.zlog_minimum_level)
    {
        return;
    }

    switch (api_level) {
    case 0:
        zlog_debug(ctx->zlog_cat, "%.*s", (int)size, (const char *)buf);
        break;
    case 2:
        zlog_warn(ctx->zlog_cat, "%.*s", (int)size, (const char *)buf);
        break;
    case 3:
        zlog_error(ctx->zlog_cat, "%.*s", (int)size, (const char *)buf);
        break;
    default:
        zlog_info(ctx->zlog_cat, "%.*s", (int)size, (const char *)buf);
        break;
    }
}

void
xqc_mini_svr_write_log_file(xqc_log_level_t lvl, const void *buf, size_t size, void *arg)
{
    xqc_mini_svr_ctx_t *ctx = (xqc_mini_svr_ctx_t*)arg;

    if (ctx->args->env_cfg.use_zlog && ctx->zlog_cat != NULL) {
        xqc_mini_svr_write_zlog(ctx, lvl, buf, size);
        return;
    }
    if (ctx->log_fd <= 0) {
        return;
    }

    int write_len = write(ctx->log_fd, buf, size);
    if (write_len < 0) {
        printf("write log failed, errno: %d\n", get_sys_errno());
        return;
    }
    write_len = write(ctx->log_fd, line_break, 1);
    if (write_len < 0) {
        printf("write log failed, errno: %d\n", get_sys_errno());
    }
}


void
xqc_mini_svr_close_log_file(void *arg)
{
    xqc_mini_svr_ctx_t *ctx = (xqc_mini_svr_ctx_t*)arg;
    if (ctx->log_fd > 0) {
        close(ctx->log_fd);
        ctx->log_fd = 0;
    }
}

void
xqc_mini_svr_write_qlog_file(qlog_event_importance_t imp, const void *buf, size_t size, void *arg)
{
    xqc_mini_svr_ctx_t *ctx = (xqc_mini_svr_ctx_t*)arg;
    if (ctx->args->env_cfg.use_zlog && ctx->zlog_cat != NULL) {
        if (!ctx->args->env_cfg.zlog_level_filter_enabled
            || ctx->args->env_cfg.zlog_minimum_level <= 1)
        {
            zlog_info(ctx->zlog_cat, "[qlog] %.*s", (int)size,
                (const char *)buf);
        }
        return;
    }

    if (ctx->log_fd <= 0) {
        return;
    }

    int write_len = write(ctx->log_fd, buf, size);
    if (write_len < 0) {
        printf("write qlog failed, errno: %d\n", get_sys_errno());
        return;
    }
    write_len = write(ctx->log_fd, line_break, 1);
    if (write_len < 0) {
        printf("write qlog failed, errno: %d\n", get_sys_errno());
    }
}

int
xqc_mini_svr_open_keylog_file(void *arg)
{
    xqc_mini_svr_ctx_t *ctx = (xqc_mini_svr_ctx_t*)arg;
    return open(ctx->args->env_cfg.key_out_path, (O_WRONLY | O_APPEND | O_CREAT), 0644);
}

void
xqc_mini_svr_keylog_cb(const xqc_cid_t *scid, const char *line, void *arg)
{
    xqc_mini_svr_ctx_t *ctx = (xqc_mini_svr_ctx_t*)arg;
    if (ctx->keylog_fd <= 0) {
        printf("write keys error!\n");
        return;
    }

    int write_len = write(ctx->keylog_fd, line, strlen(line));
    if (write_len < 0) {
        printf("write keys failed, errno: %d\n", get_sys_errno());
        return;
    }
    write_len = write(ctx->keylog_fd, line_break, 1);
    if (write_len < 0) {
        printf("write keys failed, errno: %d\n", get_sys_errno());
    }
}

void
xqc_mini_svr_close_keylog_file(void *arg)
{
    xqc_mini_svr_ctx_t *ctx = (xqc_mini_svr_ctx_t*)arg;
    if (ctx->keylog_fd > 0) {
        close(ctx->keylog_fd);
        ctx->keylog_fd = 0;
    }
}
int
xqc_mini_svr_accept(xqc_engine_t *engine, xqc_connection_t *conn, const xqc_cid_t *cid,
    void *arg)
{
    DEBUG;
    (void)engine;
    (void)conn;
    (void)cid;

    xqc_mini_svr_user_conn_t *listener =
        (xqc_mini_svr_user_conn_t *)arg;
    if (listener == NULL
        || (listener->api_listener_mode && !listener->accepting))
    {
        return XQC_ERROR;
    }
    return 0;
}


ssize_t 
xqc_mini_svr_write_socket(const unsigned char *buf, size_t size, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, void *arg)
{
    ssize_t res = XQC_OK;
    xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t *)arg;
    int fd = user_conn->ctx->current_fd;

    do {
        set_sys_errno(0);
        res = sendto(fd, buf, size, 0, peer_addr, peer_addrlen);
        if (res < 0) {
            printf("[error] xqc_mini_svr_write_socket err %zd %s, fd: %d\n", 
                res, strerror(get_sys_errno()), fd);
            if (get_sys_errno() == EAGAIN) {
                res = XQC_SOCKET_EAGAIN;
            }
        }
    } while ((res < 0) && (get_sys_errno() == EINTR));

    // printf("[report] xqc_mini_svr_write_socket success size=%lu\n", size);
    return res;
}

ssize_t
xqc_mini_svr_write_socket_ex(uint64_t path_id, const unsigned char *buf, size_t size, 
    const struct sockaddr *peer_addr,socklen_t peer_addrlen, void *conn_user_data)
{
    return xqc_mini_svr_write_socket(buf, size, peer_addr, peer_addrlen, conn_user_data);
}

void
xqc_mini_svr_conn_update_cid_notify(xqc_connection_t *conn, const xqc_cid_t *retire_cid,
    const xqc_cid_t *new_cid, void *user_data)
{
    (void)conn;
    (void)retire_cid;
    xqc_mini_svr_user_conn_t *user_conn =
        (xqc_mini_svr_user_conn_t *)user_data;
    if (user_conn != NULL && new_cid != NULL) {
        memcpy(&user_conn->cid, new_cid, sizeof(*new_cid));
    }
}

void
xqc_mini_svr_ready_to_create_path_notify(const xqc_cid_t *scid, void *conn_user_data)
{
    (void)scid;
    xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t *)conn_user_data;
    printf("[diag_server_path_event] ready_to_create_path\n");
    xqc_mini_svr_log_mp_active_state_force(user_conn, "ready_to_create_path");
}

int
xqc_mini_svr_path_created_notify(xqc_connection_t *conn, const xqc_cid_t *scid,
    uint64_t path_id, void *conn_user_data)
{
    (void)conn;
    (void)scid;
    xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t *)conn_user_data;
    printf("[diag_server_path_event] created path_id=%" PRIu64 "\n", path_id);
    xqc_mini_svr_log_mp_active_state_force(user_conn, "path_created");
    return XQC_OK;
}

void
xqc_mini_svr_path_removed_notify(const xqc_cid_t *scid, uint64_t path_id,
    void *conn_user_data)
{
    (void)scid;
    xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t *)conn_user_data;
    printf("[diag_server_path_event] removed path_id=%" PRIu64 "\n", path_id);
    xqc_mini_svr_log_mp_active_state_force(user_conn, "path_removed");
}

/* h3 callbacks */
int
xqc_mini_svr_h3_conn_create_notify(xqc_h3_conn_t *h3_conn, const xqc_cid_t *cid,
    void *conn_user_data)
{
    DEBUG;
    xqc_mini_svr_user_conn_t *listener_conn =
        (xqc_mini_svr_user_conn_t *)conn_user_data;
    if (listener_conn == NULL || listener_conn->ctx == NULL) {
        printf("[error] missing server listener connection scope\n");
        return XQC_ERROR;
    }
    xqc_mini_svr_user_conn_t *user_conn = calloc(1, sizeof(*user_conn));
    if (user_conn == NULL) {
        printf("[error] allocate server connection scope failed\n");
        return XQC_ERROR;
    }
    user_conn->local_addr = calloc(1, sizeof(*user_conn->local_addr));
    user_conn->peer_addr = calloc(1, sizeof(*user_conn->peer_addr));
    if (user_conn->local_addr == NULL || user_conn->peer_addr == NULL) {
        printf("[error] allocate server connection addresses failed\n");
        free(user_conn->local_addr);
        free(user_conn->peer_addr);
        free(user_conn);
        return XQC_ERROR;
    }

    user_conn->listener_conn = listener_conn;
    user_conn->is_connection_scope = 1;
    user_conn->api_owned = listener_conn->api_listener_mode;
    user_conn->ctx = listener_conn->ctx;
    user_conn->fd = listener_conn->fd;
    user_conn->serve_root_fd = -1;
    user_conn->h3_conn = h3_conn;
    user_conn->local_addrlen = listener_conn->local_addrlen;
    if (listener_conn->local_addr != NULL) {
        memcpy(user_conn->local_addr, listener_conn->local_addr,
            sizeof(*user_conn->local_addr));
    }
    xqc_h3_conn_set_user_data(h3_conn, user_conn);
    xqc_h3_conn_get_peer_addr(h3_conn, (struct sockaddr *)user_conn->peer_addr,
                              sizeof(struct sockaddr_in), &user_conn->peer_addrlen);
    memcpy(&user_conn->cid, cid, sizeof(*cid));

    if (user_conn->api_owned) {
        user_conn->all_next = listener_conn->connection_head;
        listener_conn->connection_head = user_conn;
    }

    printf("[stats] xqc_mini_svr_h3_conn_create_notify \n");
    printf("[diag_server_conn] h3_conn_create\n");
    xqc_mini_svr_log_mp_active_state_force(user_conn, "h3_conn_create");
    return 0;
}

static void
xqc_mini_svr_unlink_connection(xqc_mini_svr_user_conn_t *listener_conn,
    xqc_mini_svr_user_conn_t *user_conn)
{
    if (listener_conn == NULL || user_conn == NULL) {
        return;
    }
    xqc_mini_svr_user_conn_t **cursor = &listener_conn->connection_head;
    while (*cursor != NULL) {
        if (*cursor == user_conn) {
            *cursor = user_conn->all_next;
            user_conn->all_next = NULL;
            return;
        }
        cursor = &(*cursor)->all_next;
    }
}


int
xqc_mini_svr_h3_conn_close_notify(xqc_h3_conn_t *h3_conn, const xqc_cid_t *cid,
    void *conn_user_data)
{
    DEBUG;
    xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t*)conn_user_data;

    if (user_conn == NULL) {
        return 0;
    }
    xqc_mini_svr_log_mp_active_state_force(user_conn, "h3_conn_close");
    printf("[stats] xqc_mini_svr_h3_conn_close_notify success \n");
    if (user_conn->is_connection_scope) {
        xqc_mini_svr_user_conn_t *listener_conn = user_conn->listener_conn;
        if (user_conn->ctx != NULL && user_conn->ctx->args != NULL
            && user_conn->ctx->args->stream_poc.enabled)
        {
            xqc_mini_stream_poc_server_on_connection_close(user_conn);
        }
        int h3_error = xqc_h3_conn_get_errno(h3_conn);
        user_conn->last_error_code =
            h3_error == H3_NO_ERROR ? 0 : h3_error;
        xqc_h3_conn_set_user_data(h3_conn, listener_conn);
        user_conn->h3_conn = NULL;
        user_conn->connection_closed = 1;
        user_conn->serve_cancelled = 1;
        user_conn->serve_configured = 0;
        if (user_conn->api_owned) {
            if (!user_conn->queued && !user_conn->delivered) {
                xqc_mini_svr_unlink_connection(listener_conn, user_conn);
                xqc_mini_svr_free_user_conn(user_conn);
            }
            return 0;
        }
        xqc_mini_svr_free_user_conn(user_conn);
    }
    return 0;
}


void 
xqc_mini_svr_h3_conn_handshake_finished(xqc_h3_conn_t *h3_conn, void *conn_user_data)
{
    DEBUG;
    xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t *)conn_user_data;
    (void)h3_conn;
    if (user_conn == NULL) {
        return;
    }
    user_conn->handshake_finished = 1;
    if (user_conn->api_owned && !user_conn->queued && !user_conn->delivered
        && !user_conn->connection_closed)
    {
        xqc_mini_svr_user_conn_t *listener_conn = user_conn->listener_conn;
        if (listener_conn != NULL && listener_conn->accepting) {
            user_conn->queued = 1;
            listener_conn->accept_yield_requested = 1;
            user_conn->accept_next = NULL;
            if (listener_conn->accept_tail == NULL) {
                listener_conn->accept_head = user_conn;
            } else {
                listener_conn->accept_tail->accept_next = user_conn;
            }
            listener_conn->accept_tail = user_conn;
        }
    }
    printf("[diag_server_conn] handshake_finished\n");
    xqc_mini_svr_log_mp_active_state_force(user_conn, "handshake_finished");
}


int
xqc_mini_svr_h3_request_create_notify(xqc_h3_request_t *h3_request, void *strm_user_data)
{
    DEBUG;
    xqc_mini_svr_user_stream_t *user_stream = calloc(1, sizeof(*user_stream));
    if (user_stream == NULL) {
        return XQC_ERROR;
    }
    user_stream->h3_request = h3_request;
    user_stream->user_conn =
        (xqc_mini_svr_user_conn_t *)xqc_h3_get_conn_user_data_by_request(
            h3_request);
    user_stream->method = REQUEST_METHOD_POST;
    user_stream->response_status = 200;
    user_stream->response_content_type = "text/plain";
    user_stream->response_prepared = 0;
    user_stream->use_file_response = 0;
    user_stream->send_body_fp = NULL;
    user_stream->send_body_fd = -1;
    user_stream->recv_body_fd = -1;
    user_stream->static_response[0] = '\0';
    user_stream->request_path[0] = '\0';
    user_stream->resolved_path[0] = '\0';
    user_stream->response_body_offset = 0;

    xqc_h3_request_set_user_data(h3_request, user_stream);

    printf("[stats] xqc_mini_svr_h3_request_create_notify success \n");
    return 0;
}

int
xqc_mini_svr_h3_request_close_notify(xqc_h3_request_t *h3_request, void *strm_user_data)
{
    DEBUG;
    xqc_request_stats_t stats = xqc_h3_request_get_stats(h3_request);
    printf("[stats] xqc_mini_svr_h3_request_close_notify success, cwnd_blocked:%"PRIu64"\n", stats.cwnd_blocked_ms);

    xqc_mini_svr_user_stream_t *user_stream = (xqc_mini_svr_user_stream_t*)strm_user_data;
    if (user_stream == NULL) {
        return 0;
    }
    xqc_mini_svr_user_conn_t *user_conn =
        xqc_mini_svr_get_conn_from_request(user_stream);
    xqc_mini_svr_unlink_deferred_request(user_conn, user_stream);
    if (user_stream->recv_body_fd >= 0) {
        close(user_stream->recv_body_fd);
        user_stream->recv_body_fd = -1;
    }
    if (user_stream->send_body_fp) {
        fclose(user_stream->send_body_fp);
        user_stream->send_body_fp = NULL;
    }
    if (user_stream->send_body_fd >= 0) {
        close(user_stream->send_body_fd);
        user_stream->send_body_fd = -1;
    }
    free(user_stream);

    return 0;
}
int
xqc_mini_svr_send_response(xqc_mini_svr_user_stream_t *user_stream)
{
    DEBUG;
    if (!user_stream->response_prepared) {
        printf("[error] response not prepared for stream %d\n",
            user_stream->stream_index);
        return XQC_ERROR;
    }
    snprintf(user_stream->response_status_str,
        sizeof(user_stream->response_status_str), "%03d",
        user_stream->response_status);
    snprintf(user_stream->response_content_length,
        sizeof(user_stream->response_content_length), "%zu",
        user_stream->response_body_len);

    char *content_type = user_stream->response_content_type;
    if (content_type == NULL) {
        content_type = "text/plain";
        user_stream->response_content_type = content_type;
    }

    /* response header buf list */
    char response_total_length[32] = {0};
    xqc_http_header_t rsp_hdr[] = {
        {
            .name = {.iov_base = ":status", .iov_len = 7},
            .value = {.iov_base = user_stream->response_status_str,
                .iov_len = strlen(user_stream->response_status_str)},
            .flags = 0,
        },
        {
            .name = {.iov_base = "content-type", .iov_len = 12},
            .value = {.iov_base = content_type,
                .iov_len = strlen(content_type)},
            .flags = 0,
        },
        {
            .name = {.iov_base = "content-length", .iov_len = 14},
            .value = {.iov_base = user_stream->response_content_length,
                .iov_len = strlen(user_stream->response_content_length)},
            .flags = 0,
        },
        {
            .name = {.iov_base = "x-total-length", .iov_len = 14},
            .value = {.iov_base = response_total_length, .iov_len = 0},
            .flags = 0,
        },
    };
    size_t rsp_hdr_cnt = 3;
    if (user_stream->method == REQUEST_METHOD_GET) {
        snprintf(response_total_length, sizeof(response_total_length), "%zu",
            user_stream->total_file_size);
        rsp_hdr[3].value.iov_len = strlen(response_total_length);
        rsp_hdr_cnt = 4;
    }

    /* response header */
    xqc_http_headers_t rsp_hdrs;
    rsp_hdrs.headers = rsp_hdr;
    rsp_hdrs.count =  rsp_hdr_cnt;

    if (user_stream->header_sent == 0) {
        ssize_t ret = xqc_h3_request_send_headers(user_stream->h3_request,
            &rsp_hdrs, 0);
        if (ret == -XQC_EAGAIN) {
            return XQC_OK;
        }
        if (ret < 0) {
            printf("[error] xqc_h3_request_send_headers error %zd\n", ret);
            return (int)ret;
        } 
        printf("[stats] response headers sent for stream %d\n",
            user_stream->stream_index);
        user_stream->header_sent = 1;
    }

    return xqc_mini_svr_send_body(user_stream);
}

int
xqc_mini_svr_h3_request_read_notify(xqc_h3_request_t *h3_request, xqc_request_notify_flag_t flag,
    void *strm_user_data)
{
    DEBUG;
    char recv_buff[16384] = {0};
    ssize_t recv_buff_size, read;
    unsigned char fin = 0;
    xqc_http_headers_t *headers = NULL;
    xqc_mini_svr_user_stream_t *user_stream = (xqc_mini_svr_user_stream_t *)strm_user_data;
    xqc_mini_svr_user_conn_t *user_conn = xqc_mini_svr_get_conn_from_request(user_stream);
    xqc_mini_svr_log_mp_active_state(user_conn, "h3_read");
    if (user_stream->fin_received && user_stream->response_prepared) {
        return XQC_OK;
    }
    if (user_conn != NULL && user_conn->api_owned
        && user_conn->serve_cancelled)
    {
        printf("[error] request received outside an active serve loop\n");
        return XQC_ERROR;
    }
    if (user_conn != NULL && user_conn->api_owned
        && !user_conn->serve_configured)
    {
        int first_defer = !user_stream->pending_for_serve;
        xqc_mini_svr_defer_request(user_conn, user_stream, flag);
        if (first_defer) {
            printf("[stats] request deferred until serve loop is active\n");
        }
        return XQC_OK;
    }


    read = 0;
    recv_buff_size = sizeof(recv_buff);
    
    
    
    /* recv headers */
    if (flag & XQC_REQ_NOTIFY_READ_HEADER) {
        headers = xqc_h3_request_recv_headers(h3_request, &fin);
        if (headers == NULL) {
            printf("[error] xqc_h3_request_recv_headers error\n");
            return XQC_ERROR;
        }
        int have_index = 0;
        int have_count = 0;
        int have_offset = 0;
        int have_total = 0;
        int have_content_length = 0;
        int have_method = 0;
        int have_path = 0;
        uint64_t parsed_index = 0;
        uint64_t parsed_count = 0;
        uint64_t parsed_offset = 0;
        uint64_t parsed_total = 0;
        uint64_t parsed_content_length = 0;

        printf("========== [server] Received HTTP/3 Request Headers ==========\n");
        for (size_t i = 0; i < headers->count; i++) {
            xqc_http_header_t *h = &headers->headers[i];
            printf("%.*s: %.*s\n",
                (int)h->name.iov_len, (char*)h->name.iov_base,
                (int)h->value.iov_len, (char*)h->value.iov_base);
            if (xqc_mini_svr_header_name_is(h, ":method")) {
                char buf[16] = {0};
                if (have_method
                    || (h->value.iov_len != 3 && h->value.iov_len != 4))
                {
                    return XQC_ERROR;
                }
                size_t len = h->value.iov_len;
                memcpy(buf, h->value.iov_base, len);
                if (strcasecmp(buf, "GET") == 0) {
                    user_stream->method = REQUEST_METHOD_GET;
                } else if (strcasecmp(buf, "POST") == 0) {
                    user_stream->method = REQUEST_METHOD_POST;
                } else {
                    printf("[error] unsupported request method\n");
                    return XQC_ERROR;
                }
                have_method = 1;
                continue;
            }

            if (xqc_mini_svr_header_name_is(h, ":path")) {
                if (have_path || h->value.iov_len == 0
                    || h->value.iov_len >= sizeof(user_stream->request_path)
                    || memchr(h->value.iov_base, '\0',
                        h->value.iov_len) != NULL)
                {
                    return XQC_ERROR;
                }
                size_t len = h->value.iov_len;
                memcpy(user_stream->request_path, h->value.iov_base, len);
                user_stream->request_path[len] = '\0';
                have_path = 1;
                continue;
            }
            if (xqc_mini_svr_header_name_is(h, "content-length")) {
                if (have_content_length
                    || xqc_mini_svr_parse_u64_header(
                        h, &parsed_content_length) != XQC_OK)
                {
                    return XQC_ERROR;
                }
                have_content_length = 1;
                continue;
            }

            if (xqc_mini_svr_header_name_is(h, "x-stream-index")) {
                if (have_index
                    || xqc_mini_svr_parse_u64_header(h, &parsed_index)
                        != XQC_OK)
                {
                    return XQC_ERROR;
                }
                have_index = 1;
                continue;
            }

            if (xqc_mini_svr_header_name_is(h, "x-stream-count")) {
                if (have_count
                    || xqc_mini_svr_parse_u64_header(h, &parsed_count)
                        != XQC_OK)
                {
                    return XQC_ERROR;
                }
                have_count = 1;
                continue;
            }

            if (xqc_mini_svr_header_name_is(h, "x-stream-offset")) {
                if (have_offset
                    || xqc_mini_svr_parse_u64_header(h, &parsed_offset)
                        != XQC_OK)
                {
                    return XQC_ERROR;
                }
                have_offset = 1;
                continue;
            }

            if (xqc_mini_svr_header_name_is(h, "x-total-length")) {
                if (have_total
                    || xqc_mini_svr_parse_u64_header(h, &parsed_total)
                        != XQC_OK)
                {
                    return XQC_ERROR;
                }
                have_total = 1;
                continue;
            }
        }
        printf("=============================================================\n");
        if (!have_method || !have_path || user_stream->request_path[0] != '/') {
            printf("[error] missing or invalid method/path headers\n");
            return XQC_ERROR;
        }
        if (have_content_length && parsed_content_length > SIZE_MAX) {
            printf("[error] content length exceeds platform size\n");
            return XQC_ERROR;
        }
        user_stream->expected_content_length =
            have_content_length ? (size_t)parsed_content_length : 0;

        if (user_stream->method == REQUEST_METHOD_GET) {
            if (!have_index || !have_count || !have_offset || !have_total
                || !have_content_length || parsed_count == 0
                || parsed_count > XQC_MINI_SVR_MAX_TRACKED_FRAGMENTS
                || parsed_index >= parsed_count || parsed_index > INT_MAX
                || parsed_offset > SIZE_MAX || parsed_total > SIZE_MAX)
            {
                printf("[error] invalid GET stream metadata\n");
                return XQC_ERROR;
            }
            size_t real_file_size = 0;
            if (xqc_mini_svr_get_target_file_size(user_stream, &real_file_size) != XQC_OK) {
                xqc_mini_svr_prepare_text_response(user_stream, 404,
                    "file %s not found", user_stream->request_path);
            } else {
                user_stream->total_file_size = real_file_size;
                size_t stream_offset = 0;
                size_t stream_length = 0;
                xqc_mini_svr_compute_stream_segment((int)parsed_count,
                    (int)parsed_index, user_stream->total_file_size,
                    xqc_mini_svr_get_stream_range_size(user_stream),
                    &stream_offset, &stream_length);
                int invalid_discovery = parsed_total == 0
                    && (parsed_index != 0 || parsed_count != 1
                        || parsed_offset != 0
                        || parsed_content_length != 0);
                int invalid_fragment = parsed_total != 0
                    && (parsed_total != user_stream->total_file_size
                        || parsed_offset != stream_offset
                        || parsed_content_length != stream_length);
                if (invalid_discovery || invalid_fragment) {
                    printf("[error] inconsistent GET stream metadata\n");
                    return XQC_ERROR;
                }
                user_stream->stream_index = (int)parsed_index;
                user_stream->stream_count = (int)parsed_count;
                user_stream->stream_offset = stream_offset;
                user_stream->metadata_parsed = 1;
                user_stream->requested_generated_length = stream_length;
                user_stream->response_body_offset = stream_offset;
            }
            printf("[diag_server_get] headers stream=%d/%d offset=%zu len=%zu total=%zu"
                   " metadata=%d path=%s resolved=%s\n",
                user_stream->stream_index,
                user_stream->stream_count,
                user_stream->response_body_offset,
                user_stream->requested_generated_length,
                user_stream->total_file_size,
                user_stream->metadata_parsed,
                user_stream->request_path,
                user_stream->resolved_path);
            if (!user_stream->response_prepared) {
                int prep_ret = xqc_mini_svr_prepare_file_response(user_stream);
                if (prep_ret == XQC_ERROR) {
                    xqc_mini_svr_prepare_text_response(user_stream, 500,
                        "failed to read %s", user_stream->resolved_path);
                }
                printf("[diag_server_get] response_start status=%d body_len=%zu"
                       " body_offset=%zu use_file=%d path=%s\n",
                    user_stream->response_status,
                    user_stream->response_body_len,
                    user_stream->response_body_offset,
                    user_stream->use_file_response,
                    user_stream->resolved_path);
            }
            xqc_mini_svr_send_response(user_stream);
            user_stream->header_recvd = 1;
        } else {
            if (!have_index || !have_count || !have_offset || !have_total
                || !have_content_length)
            {
                printf("[error] missing POST stream metadata headers\n");
                return XQC_ERROR;
            }

            if (parsed_count == 0
                || parsed_count > XQC_MINI_SVR_MAX_TRACKED_FRAGMENTS
                || parsed_count > INT_MAX)
            {
                printf("[error] invalid stream count %" PRIu64 "\n",
                    parsed_count);
                return XQC_ERROR;
            }
            if (parsed_index >= parsed_count || parsed_index > INT_MAX
                || parsed_offset > SIZE_MAX || parsed_total > SIZE_MAX
                || parsed_total > INT64_MAX)
            {
                printf("[error] invalid stream index %" PRIu64
                       " for count %" PRIu64 "\n",
                    parsed_index, parsed_count);
                return XQC_ERROR;
            }

            size_t range_size = xqc_mini_svr_get_stream_range_size(user_stream);
            size_t total_size = (size_t)parsed_total;
            size_t required_streams = total_size == 0 ? 1
                : ((total_size - 1) / range_size) + 1;
            if (parsed_count != required_streams) {
                printf("[error] stream count does not match total length\n");
                return XQC_ERROR;
            }

            user_stream->stream_index = (int)parsed_index;
            user_stream->stream_count = (int)parsed_count;
            user_stream->stream_offset = (size_t)parsed_offset;
            user_stream->total_file_size = total_size;
            user_stream->metadata_parsed = 1;

            size_t expect_offset = 0;
            size_t expect_length = 0;
            xqc_mini_svr_compute_stream_segment(user_stream->stream_count,
                user_stream->stream_index, user_stream->total_file_size,
                range_size,
                &expect_offset, &expect_length);
            if (user_stream->stream_offset != expect_offset
                || user_stream->expected_content_length != expect_length)
            {
                printf("[error] POST fragment offset/length mismatch\n");
                return XQC_ERROR;
            }

            if (xqc_mini_svr_prepare_output_file(user_stream) != XQC_OK) {
                return XQC_ERROR;
            }

            printf("[server] stream %d/%d offset=%zu length=%zu total=%zu\n",
                user_stream->stream_index, user_stream->stream_count,
                user_stream->stream_offset, user_stream->expected_content_length,
                user_stream->total_file_size);

            user_stream->header_recvd = 1;

            gettimeofday(&user_stream->start_time, NULL);
        }
    }
        /* TODO: if recv headers once for all? */
    if (user_stream->method == REQUEST_METHOD_GET) {
        return XQC_OK;
    }
     

    if (flag & XQC_REQ_NOTIFY_READ_BODY) {   /* recv body */
        if (!user_stream->metadata_parsed) {
            printf("[error] received body before metadata parsed\n");
            return XQC_ERROR;
        }
        if (user_conn != NULL && user_conn->serve_cancelled) {
            printf("[error] upload cancelled before body write\n");
            return XQC_ERROR;
        }

        if (user_stream->recv_body_fd < 0) {
            if (xqc_mini_svr_prepare_output_file(user_stream) != XQC_OK) {
                return XQC_ERROR;
            }
        }
        do {
            uint64_t aggregate_start_ns = xqc_mini_svr_monotonic_now_ns();
            read = xqc_h3_request_recv_body(h3_request, recv_buff, recv_buff_size, &fin);

            if (read == -XQC_EAGAIN) {
                break;

            } else if (read < 0) {
                printf("[error] xqc_h3_request_recv_body error %zd\n", read);
                return XQC_ERROR;
            }
            if (user_stream->recv_body_len
                    > user_stream->expected_content_length
                || (size_t)read
                > user_stream->expected_content_length
                    - user_stream->recv_body_len)
            {
                printf("[error] POST body exceeds declared fragment length\n");
                return XQC_ERROR;
            }

            off_t write_off = (off_t)(user_stream->stream_offset + user_stream->recv_body_len);
            size_t write_total = 0;
            while (write_total < (size_t)read) {
                if (user_conn != NULL && user_conn->serve_cancelled) {
                    return XQC_ERROR;
                }
                ssize_t written = pwrite(user_stream->recv_body_fd,
                    recv_buff + write_total, (size_t)read - write_total,
                    write_off + (off_t)write_total);
                if (written < 0 && errno == EINTR) {
                    continue;
                }
                if (written <= 0) {
                    perror("pwrite");
                    return XQC_ERROR;
                }
                write_total += (size_t)written;
            }

            user_stream->recv_body_len += read;
            uint64_t aggregate_end_ns = xqc_mini_svr_monotonic_now_ns();
            if (read > 0) {
                xqc_mini_svr_record_latency(
                    &user_stream->recv_aggregate_sample_count,
                    &user_stream->recv_aggregate_sample_bytes,
                    &user_stream->recv_aggregate_total_ns,
                    &user_stream->recv_aggregate_min_ns,
                    &user_stream->recv_aggregate_max_ns,
                    aggregate_start_ns, aggregate_end_ns, (size_t)read);
            }
            xqc_mini_svr_update_upload_progress(user_stream);
            //printf("[server] received chunk: %zd bytes (total: %zu)\n", read, user_stream->recv_body_len);
        } while (read > 0);
    }
    if (fin) {
        user_stream->fin_received = 1;
        gettimeofday(&user_stream->end_time, NULL);
        printf("[stats] read h3 request finish. total %zu bytes\n", user_stream->recv_body_len);
        
    }
    if (user_stream->fin_received
        && user_stream->recv_body_len != user_stream->expected_content_length)
    {
        printf("[error] POST body ended at %zu bytes, expected %zu\n",
            user_stream->recv_body_len,
            user_stream->expected_content_length);
        return XQC_ERROR;
    }
    if (user_stream->fin_received) {

        if (user_stream->recv_body_fd >= 0) {
            if (fsync(user_stream->recv_body_fd) != 0) {
                perror("fsync");
                return XQC_ERROR;
            }
            close(user_stream->recv_body_fd);
            user_stream->recv_body_fd = -1;
        }
        double duration_ms = (user_stream->end_time.tv_sec - user_stream->start_time.tv_sec) * 1000.0 +
                         (user_stream->end_time.tv_usec - user_stream->start_time.tv_usec) / 1000.0;

        double mbps = duration_ms > 0.0
            ? (user_stream->recv_body_len * 8.0)
                / (duration_ms * 1000.0)
            : 0.0;

        printf("[stats] Body finished. recv_len=%zu bytes, time=%.3f ms, speed=%.3f Mbps\n",
           user_stream->recv_body_len, duration_ms, mbps);
        xqc_mini_svr_mark_stream_complete(user_stream);

        xqc_mini_svr_prepare_text_response(user_stream, 200,
            "received %zu bytes on stream %d", user_stream->recv_body_len,
            user_stream->stream_index);
        xqc_mini_svr_send_response(user_stream);

        
    }
    return 0;
}

int
xqc_mini_svr_send_body(xqc_mini_svr_user_stream_t *user_stream)
{
    int first_send = !user_stream->send_started;
    if (!user_stream->send_started) {
        gettimeofday(&user_stream->send_start_time, NULL);
        user_stream->send_started = 1;
    }
    if (first_send && user_stream->method == REQUEST_METHOD_GET) {
        xqc_mini_svr_user_conn_t *user_conn =
            xqc_mini_svr_get_conn_from_request(user_stream);
        xqc_mini_svr_get_diag_maybe_reset_phase(user_stream);
        xqc_mini_svr_get_diag_capture(user_conn, &user_stream->get_path_diag_start);
        user_stream->get_path_diag_started = user_stream->get_path_diag_start.valid;
        printf("[diag_server_get] send_body_enter body_len=%zu body_offset=%zu"
               " use_file=%d stream=%d/%d\n",
            user_stream->response_body_len,
            user_stream->response_body_offset,
            user_stream->use_file_response,
            user_stream->stream_index,
            user_stream->stream_count);
    }
    if (user_stream->response_body_len == 0
        && user_stream->response_body_sent == 0) {
        ssize_t ret = xqc_h3_request_send_body(user_stream->h3_request,
            NULL, 0, 1);
        if (ret == -XQC_EAGAIN) {
            return XQC_OK;
        }
        if (ret < 0) {
            printf("[error] xqc_h3_request_send_body error %zd\n", ret);
            return (int)ret;
        }
        user_stream->response_body_sent = user_stream->response_body_len;
        xqc_mini_svr_log_response_send_stats(user_stream);
        return XQC_OK;
    }

    if (user_stream->use_generated_response) {
        while (user_stream->response_body_sent < user_stream->response_body_len
            || user_stream->buffered_sent < user_stream->buffered_len) {
            if (user_stream->buffered_sent == user_stream->buffered_len) {
                size_t remaining = user_stream->response_body_len
                    - user_stream->response_body_sent;
                size_t chunk = remaining < sizeof(user_stream->send_cache)
                    ? remaining : sizeof(user_stream->send_cache);
                if (chunk == 0) {
                    break;
                }
                xqc_mini_svr_fill_generated_chunk(user_stream->send_cache, chunk,
                    user_stream->response_body_offset + user_stream->response_body_sent);
                user_stream->buffered_len = chunk;
                user_stream->buffered_sent = 0;
            }

            size_t bytes_left = user_stream->buffered_len - user_stream->buffered_sent;
            int fin = (user_stream->response_body_sent + bytes_left
                >= user_stream->response_body_len) ? 1 : 0;
            ssize_t ret = xqc_h3_request_send_body(user_stream->h3_request,
                user_stream->send_cache + user_stream->buffered_sent,
                bytes_left, fin);
            if (ret == -XQC_EAGAIN) {
                break;
            }
            if (ret < 0) {
                printf("[error] xqc_h3_request_send_body error %zd\n", ret);
                return (int)ret;
            }
            user_stream->buffered_sent += ret;
            user_stream->response_body_sent += ret;
        }
        xqc_mini_svr_log_response_send_stats(user_stream);
        return XQC_OK;

    } else if (!user_stream->use_file_response) {
        size_t remaining = user_stream->response_body_len
            - user_stream->response_body_sent;
        if (remaining == 0) {
            return XQC_OK;
        }
        unsigned char *base = (unsigned char *)user_stream->static_response
            + user_stream->response_body_sent;
        int fin = 1;
        ssize_t ret = xqc_h3_request_send_body(user_stream->h3_request,
            base, remaining, fin);
        if (ret == -XQC_EAGAIN) {
            return XQC_OK;
        }
        if (ret < 0) {
            printf("[error] xqc_h3_request_send_body error %zd\n", ret);
            return (int)ret;
        }
        user_stream->response_body_sent += ret;
        xqc_mini_svr_log_response_send_stats(user_stream);
        return XQC_OK;
    }

    while (user_stream->response_body_sent < user_stream->response_body_len
        || user_stream->buffered_sent < user_stream->buffered_len) {
        if (user_stream->buffered_sent == user_stream->buffered_len) {
            if (user_stream->send_body_fd < 0) {
                printf("[error] file response fd is not open\n");
                return XQC_ERROR;
            }
            size_t remaining = user_stream->response_body_len
                - user_stream->response_body_sent;
            size_t chunk = remaining < sizeof(user_stream->send_cache)
                ? remaining : sizeof(user_stream->send_cache);
            if (chunk == 0) {
                break;
            }
            off_t read_off = (off_t)(user_stream->response_body_offset
                + user_stream->response_body_sent);
            ssize_t read_len = xqc_mini_svr_pread_file_chunk(user_stream,
                user_stream->send_cache, chunk, read_off);
            if (read_len < 0) {
                perror("pread");
                return XQC_ERROR;
            }
            if (read_len == 0) {
                break;
            }
            user_stream->buffered_len = (size_t)read_len;
            user_stream->buffered_sent = 0;
        }

        size_t bytes_left = user_stream->buffered_len - user_stream->buffered_sent;
        int fin = (user_stream->response_body_sent + bytes_left
            >= user_stream->response_body_len) ? 1 : 0;
        ssize_t ret = xqc_mini_svr_h3_send_body_record(user_stream,
            user_stream->send_cache + user_stream->buffered_sent,
            bytes_left, fin);
        if (ret == -XQC_EAGAIN) {
            break;
        }
        if (ret < 0) {
            printf("[error] xqc_h3_request_send_body error %zd\n", ret);
            return (int)ret;
        }
        user_stream->buffered_sent += ret;
        user_stream->response_body_sent += ret;
    }
    xqc_mini_svr_log_response_send_stats(user_stream);
    return XQC_OK;
}

int
xqc_mini_svr_h3_request_write_notify(xqc_h3_request_t *h3_request, void *strm_user_data)
{
    DEBUG;
    xqc_mini_svr_user_stream_t *user_stream = (xqc_mini_svr_user_stream_t *)strm_user_data;
    if (!user_stream->response_prepared) {
        return XQC_OK;
    }

    xqc_mini_svr_user_conn_t *user_conn = xqc_mini_svr_get_conn_from_request(user_stream);
    xqc_mini_svr_log_mp_active_state(user_conn, "h3_write");


    int ret = user_stream->header_sent
        ? xqc_mini_svr_send_body(user_stream)
        : xqc_mini_svr_send_response(user_stream);

    // printf("[stats] write notify handled for stream %d\n",
    //     user_stream->stream_index);
    return ret;
}
