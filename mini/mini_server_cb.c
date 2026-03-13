/**
 * @file mini_server_cb.c contains callbacks definitions for mini_server, including:
 * 1. engine callbacks
 * 2. hq callbacks
 * 3. h3 callbacks
 */

#include "mini_server_cb.h"
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <stdarg.h>



#define XQC_MINI_SVR_MAX_STREAMS 16
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
    unsigned char stream_done[XQC_MINI_SVR_MAX_STREAMS];
    size_t stream_progress[XQC_MINI_SVR_MAX_STREAMS];
    char output_path[PATH_LEN];
} xqc_mini_svr_file_state_t;

static xqc_mini_svr_file_state_t g_svr_file_state = {0};

static void xqc_mini_svr_reset_file_state(int stream_count, size_t total_size,const char *output_path);
static void xqc_mini_svr_update_upload_progress(xqc_mini_svr_user_stream_t *user_stream);
static int xqc_mini_svr_prepare_output_file(xqc_mini_svr_user_stream_t *user_stream);//Post文件输出路径
static void xqc_mini_svr_mark_stream_complete(xqc_mini_svr_user_stream_t *user_stream);//检测流帧是否传输完成
static int xqc_mini_svr_time_cmp(const struct timeval *a, const struct timeval *b);
static double xqc_mini_svr_duration_ms(const struct timeval *start, const struct timeval *end);
static xqc_mini_svr_user_conn_t *xqc_mini_svr_get_conn_from_request(xqc_mini_svr_user_stream_t *user_stream);
static void xqc_mini_svr_resolve_file_path(xqc_mini_svr_ctx_t *ctx, const char *request_path,
    char *resolved, size_t resolved_len);
static void xqc_mini_svr_prepare_text_response(xqc_mini_svr_user_stream_t *user_stream,
    int status, const char *fmt, ...);
static void xqc_mini_svr_compute_stream_segment(int stream_count, int stream_index,
    size_t total_size, size_t stream_range_size, size_t *offset, size_t *length);
static int xqc_mini_svr_prepare_file_response(xqc_mini_svr_user_stream_t *user_stream);
static void xqc_mini_svr_fill_generated_chunk(unsigned char *buf, size_t len,
    size_t offset);
static void xqc_mini_svr_resolve_upload_path(xqc_mini_svr_ctx_t *ctx, char *resolved,
    size_t resolved_len);
static int xqc_mini_svr_get_target_file_size(xqc_mini_svr_user_stream_t *user_stream,
    size_t *file_size);
static void xqc_mini_svr_log_mp_active_state(xqc_mini_svr_user_conn_t *user_conn,
    const char *scene);


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

    if (duration_ms > 0.0) {
        double mbps = (user_stream->response_body_len * 8.0)
            / (duration_ms * 1000.0);
        printf("[server] response send finished. send_len=%zu bytes, time=%.3f ms, speed=%.3f Mbps\n",
            user_stream->response_body_len, duration_ms, mbps);

    } else {
        printf("[server] response send finished. send_len=%zu bytes, time=%.3f ms (throughput unavailable)\n",
            user_stream->response_body_len, duration_ms);
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

static void
xqc_mini_svr_log_mp_active_state(xqc_mini_svr_user_conn_t *user_conn, const char *scene)
{
    if (user_conn == NULL || user_conn->ctx == NULL || user_conn->ctx->engine == NULL) {
        return;
    }

    uint64_t now_us = xqc_mini_svr_now_us();
    if (user_conn->last_mp_state_log_us != 0
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

    if (both_active && scene != NULL && strcmp(scene, "h3_write") == 0) {
        xqc_int_t ping_ret = xqc_h3_conn_send_ping(user_conn->ctx->engine,
                                                   &user_conn->cid, NULL);
        if (ping_ret != XQC_OK) {
            printf("[warn] mini server periodic mp ping failed, ret=%d\n", ping_ret);
        }
    }

    printf("[mini_mp][%s] both_active=%d, active_path_cnt=%d, mp_state=%d, enable_mp=%d, conn_info=%s\n",
           scene ? scene : "unknown", both_active, active_path_cnt,
           stats.mp_state, stats.enable_multipath, stats.conn_info);

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
    }
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
    return (xqc_mini_svr_user_conn_t *)xqc_h3_get_conn_user_data_by_request(
        user_stream->h3_request);
}
/*用于Get方法的响应发送文件路径 */
static void
xqc_mini_svr_resolve_file_path(xqc_mini_svr_ctx_t *ctx, const char *request_path,
    char *resolved, size_t resolved_len)
{
    if (resolved_len == 0) {
        return;
    }

    resolved[0] = '\0';
    if (ctx == NULL || ctx->args == NULL) {
        return;
    }

    const char *target = request_path;
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
        strncpy(resolved, target, resolved_len - 1);
        resolved[resolved_len - 1] = '\0';
        return;
    }

    snprintf(resolved, resolved_len, "%s/%s", ctx->args->env_cfg.data_dir, target);
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

    xqc_mini_svr_ctx_t *ctx = user_conn->ctx;
    xqc_mini_svr_resolve_file_path(ctx, user_stream->request_path,
        user_stream->resolved_path, sizeof(user_stream->resolved_path));

    // FILE *fp = fopen(user_stream->resolved_path, "rb");
    // if (fp == NULL) {
    //     xqc_mini_svr_prepare_text_response(user_stream, 404,
    //         "file %s not found", user_stream->resolved_path);
    //     return XQC_OK;
    size_t resp_len = user_stream->requested_generated_length;
    
    if (resp_len == 0) {
        FILE *fp = fopen(user_stream->resolved_path, "rb");
        if (fp == NULL) {
            xqc_mini_svr_prepare_text_response(user_stream, 404,
                "file %s not found", user_stream->resolved_path);
            return XQC_OK;
        }
    // if (fseek(fp, 0, SEEK_END) != 0) {
    //     perror("fseek");
    //     fclose(fp);
    //     return XQC_ERROR;
    // }
    // long file_length = ftell(fp);
    // if (file_length < 0) {
    //     perror("ftell");
         if (fseek(fp, 0, SEEK_END) != 0) {
            perror("fseek");
            fclose(fp);
            return XQC_ERROR;
        }
        long file_length = ftell(fp);
        if (file_length < 0) {
            perror("ftell");
            fclose(fp);
            return XQC_ERROR;
        }
        fclose(fp);
        resp_len = (size_t)file_length;
    }

    user_stream->send_body_fp = fopen(user_stream->resolved_path, "rb");
    if (user_stream->send_body_fp == NULL) {
        xqc_mini_svr_prepare_text_response(user_stream, 404,
            "file %s not found", user_stream->resolved_path);
        return XQC_OK;
    }
    if (user_stream->response_body_offset > 0) {
        if (fseek(user_stream->send_body_fp,
                (long)user_stream->response_body_offset, SEEK_SET) != 0) {
            perror("fseek");
            fclose(user_stream->send_body_fp);
            user_stream->send_body_fp = NULL;
            xqc_mini_svr_prepare_text_response(user_stream, 500,
                "failed to seek file %s", user_stream->resolved_path);
            return XQC_OK;
        }
    }

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

    xqc_mini_svr_resolve_file_path(user_conn->ctx, user_stream->request_path,
        user_stream->resolved_path, sizeof(user_stream->resolved_path));

    FILE *fp = fopen(user_stream->resolved_path, "rb");
    if (fp == NULL) {
        return XQC_ERROR;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return XQC_ERROR;
    }
    long file_length = ftell(fp);
    fclose(fp);
    if (file_length < 0) {
        return XQC_ERROR;
    }

    *file_size = (size_t)file_length;
    return XQC_OK;
}



static void
xqc_mini_svr_reset_file_state(int stream_count, size_t total_size,const char *output_path)
{
    if (stream_count <= 0) {
        stream_count = 1;
    }
    if (stream_count > XQC_MINI_SVR_MAX_STREAMS) {
        printf("[server] stream count %d exceeds limit %d, clamp\n",
            stream_count, XQC_MINI_SVR_MAX_STREAMS);
        stream_count = XQC_MINI_SVR_MAX_STREAMS;
    }

    g_svr_file_state.initialized = 1;
    g_svr_file_state.generation++;
    g_svr_file_state.expected_streams = stream_count;
    g_svr_file_state.total_size = total_size;
    g_svr_file_state.total_received = 0;
    g_svr_file_state.completed_streams = 0;
    g_svr_file_state.earliest_start.tv_sec = 0;
    g_svr_file_state.earliest_start.tv_usec = 0;
    g_svr_file_state.latest_end.tv_sec = 0;
    g_svr_file_state.latest_end.tv_usec = 0;
    memset(g_svr_file_state.stream_done, 0, sizeof(g_svr_file_state.stream_done));
    memset(g_svr_file_state.stream_progress, 0, sizeof(g_svr_file_state.stream_progress));
    if (output_path != NULL && output_path[0] != '\0') {
        strncpy(g_svr_file_state.output_path, output_path,
            sizeof(g_svr_file_state.output_path) - 1);
        g_svr_file_state.output_path[sizeof(g_svr_file_state.output_path) - 1] = '\0';
    } else {
        g_svr_file_state.output_path[0] = '\0';
    }
    const char *output_file = g_svr_file_state.output_path[0] != '\0'
        ? g_svr_file_state.output_path
        : SERVER_OUTPUT_FILE;
    if (remove(output_file) == 0) {
        printf("[server] removed previous output file %s\n", output_file);
    }



    FILE *fp = fopen(output_file, "wb");
    if (fp == NULL) {
        perror("fopen");
        printf("[error] failed to prepare output file '%s'\n", output_file);
        return;
    }
    if (total_size > 0) {
        if (fseek(fp, (long)(total_size - 1), SEEK_SET) != 0) {
            perror("fseek");
        } else {
            fputc('\0', fp);
        }
    }
    fclose(fp);

    printf("[server] prepared output file '%s' for totaling %zu bytes\n",
        output_file, total_size);
}
static void
xqc_mini_svr_resolve_upload_path(xqc_mini_svr_ctx_t *ctx, char *resolved,
    size_t resolved_len)
{
    if (resolved_len == 0) {
        return;
    }
    resolved[0] = '\0';
    if (ctx == NULL || ctx->args == NULL) {
        return;
    }

    const char *target = ctx->args->env_cfg.upload_path;
    if (target == NULL || target[0] == '\0') {
        target = SERVER_OUTPUT_FILE;
    }
    if (target[0] == '/') {
        strncpy(resolved, target, resolved_len - 1);
        resolved[resolved_len - 1] = '\0';
        return;
    }
    snprintf(resolved, resolved_len, "%s/%s", ctx->args->env_cfg.data_dir, target);
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
    size_t start = (size_t)stream_index * XQC_MINI_SVR_STREAM_RANGE_SIZE;
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
    xqc_mini_svr_ctx_t *ctx = user_conn ? user_conn->ctx : NULL;
    char output_path[PATH_LEN];
    xqc_mini_svr_resolve_upload_path(ctx, output_path, sizeof(output_path));
    const char *output_file = output_path[0] != '\0' ? output_path : SERVER_OUTPUT_FILE;

    if (!g_svr_file_state.initialized
        || g_svr_file_state.expected_streams != user_stream->stream_count
        || g_svr_file_state.total_size != user_stream->total_file_size
        || strncmp(g_svr_file_state.output_path, output_file,
            sizeof(g_svr_file_state.output_path)) != 0) {

        xqc_mini_svr_reset_file_state(user_stream->stream_count,
            user_stream->total_file_size,output_file);
    }

    user_stream->file_generation = g_svr_file_state.generation;

    if (user_stream->recv_body_fp == NULL) {
        user_stream->recv_body_fp = fopen(output_file, "wb+");
        if (user_stream->recv_body_fp == NULL) {
            perror("fopen");
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
    if (user_stream->stream_index < 0
        || user_stream->stream_index >= XQC_MINI_SVR_MAX_STREAMS) {
        return;
    }

    if (user_stream->file_generation != g_svr_file_state.generation) {
        return;
    }

    size_t prev = g_svr_file_state.stream_progress[user_stream->stream_index];
    if (user_stream->recv_body_len > prev) {
        g_svr_file_state.total_received += user_stream->recv_body_len - prev;
        g_svr_file_state.stream_progress[user_stream->stream_index] = user_stream->recv_body_len;
    }

    if (g_svr_file_state.total_size > 0) {
        double progress = (double)g_svr_file_state.total_received * 100.0
            / g_svr_file_state.total_size;
        if (progress > 100.0) {
            progress = 100.0;
        }
        printf("\r[upload] %.2f%%", progress);
        fflush(stdout);
        if (g_svr_file_state.total_received >= g_svr_file_state.total_size) {
            printf("\n");
        }
    }
}

static void
xqc_mini_svr_mark_stream_complete(xqc_mini_svr_user_stream_t *user_stream)
{
    if (user_stream->stream_index < 0
        || user_stream->stream_index >= XQC_MINI_SVR_MAX_STREAMS) {
        return;
    }

    if (user_stream->file_generation != g_svr_file_state.generation) {
        return;
    }

    if (!g_svr_file_state.stream_done[user_stream->stream_index]) {
        g_svr_file_state.stream_done[user_stream->stream_index] = 1;
        

        if (g_svr_file_state.completed_streams == 0) {
            g_svr_file_state.earliest_start = user_stream->start_time;
            g_svr_file_state.latest_end = user_stream->end_time;

        } else {
            if (xqc_mini_svr_time_cmp(&user_stream->start_time,
                    &g_svr_file_state.earliest_start) < 0) {
                g_svr_file_state.earliest_start = user_stream->start_time;
            }
            if (xqc_mini_svr_time_cmp(&user_stream->end_time,
                    &g_svr_file_state.latest_end) > 0) {
                g_svr_file_state.latest_end = user_stream->end_time;
            }
        }

        g_svr_file_state.completed_streams++;
        printf("[server] segment %d complete (%zu bytes at offset %zu)\n",
            user_stream->stream_index, user_stream->recv_body_len,
            user_stream->stream_offset);

        if (g_svr_file_state.completed_streams >= g_svr_file_state.expected_streams) {
            printf("[server] all %d segments received, assembled size %zu bytes\n",
                g_svr_file_state.expected_streams, g_svr_file_state.total_size);
            double duration_ms = xqc_mini_svr_duration_ms(&g_svr_file_state.earliest_start,
                &g_svr_file_state.latest_end);
            if (duration_ms > 0.0) {
                double mbps = (g_svr_file_state.total_received * 8.0)
                    / (duration_ms * 1000.0);
                printf("[server] aggregate throughput: %.3f Mbps over %.3f ms (%zu bytes)\n",
                    mbps, duration_ms, g_svr_file_state.total_received);
            } else {
                printf("[server] aggregate throughput unavailable (duration %.3f ms)\n",
                    duration_ms);
            }
        }
    }
}

const char *line_break = "\n";

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

    switch (lvl) {
    case XQC_LOG_DEBUG:
        zlog_debug(ctx->zlog_cat, "%.*s", (int)size, (const char *)buf);
        break;
    case XQC_LOG_WARN:
        zlog_warn(ctx->zlog_cat, "%.*s", (int)size, (const char *)buf);
        break;
    case XQC_LOG_ERROR:
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
        zlog_info(ctx->zlog_cat, "[qlog] %.*s", (int)size, (const char *)buf);
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
    DEBUG;
    // xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t *)user_data;
    // memcpy(&user_conn->cid, new_cid, sizeof(*new_cid));
}

/* h3 callbacks */
int
xqc_mini_svr_h3_conn_create_notify(xqc_h3_conn_t *h3_conn, const xqc_cid_t *cid,
    void *conn_user_data)
{
    DEBUG;
    xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t *)conn_user_data;
    xqc_h3_conn_set_user_data(h3_conn, user_conn);
    xqc_h3_conn_get_peer_addr(h3_conn, (struct sockaddr *)user_conn->peer_addr,
                              sizeof(struct sockaddr_in), &user_conn->peer_addrlen);
    memcpy(&user_conn->cid, cid, sizeof(*cid));

    g_svr_file_state.initialized = 0;
    g_svr_file_state.expected_streams = 0;
    g_svr_file_state.total_size = 0;
    g_svr_file_state.completed_streams = 0;
    memset(g_svr_file_state.stream_done, 0, sizeof(g_svr_file_state.stream_done));

    printf("[stats] xqc_mini_svr_h3_conn_create_notify \n");
    return 0;
}


int
xqc_mini_svr_h3_conn_close_notify(xqc_h3_conn_t *h3_conn, const xqc_cid_t *cid,
    void *conn_user_data)
{
    DEBUG;
    xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t*)conn_user_data;
    
    printf("[stats] xqc_mini_svr_h3_conn_close_notify success \n");
    return 0;
}


void 
xqc_mini_svr_h3_conn_handshake_finished(xqc_h3_conn_t *h3_conn, void *conn_user_data)
{
    DEBUG;
    xqc_mini_svr_user_conn_t *user_conn = (xqc_mini_svr_user_conn_t *)conn_user_data;
    xqc_conn_stats_t stats = xqc_conn_get_stats(user_conn->ctx->engine, &user_conn->cid);
}


int
xqc_mini_svr_h3_request_create_notify(xqc_h3_request_t *h3_request, void *strm_user_data)
{
    DEBUG;
    xqc_mini_svr_user_stream_t *user_stream = calloc(1, sizeof(*user_stream));
    user_stream->h3_request = h3_request;
    user_stream->method = REQUEST_METHOD_POST;
    user_stream->response_status = 200;
    user_stream->response_content_type = "text/plain";
    user_stream->response_prepared = 0;
    user_stream->use_file_response = 0;
    user_stream->send_body_fp = NULL;
    user_stream->static_response[0] = '\0';
    user_stream->request_path[0] = '\0';
    user_stream->resolved_path[0] = '\0';
    user_stream->response_body_offset = 0;

    xqc_h3_request_set_user_data(h3_request, user_stream);
    user_stream->recv_buf = calloc(1, REQ_BUF_SIZE);

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
    if (user_stream->recv_body_fp) {
        fclose(user_stream->recv_body_fp);
        user_stream->recv_body_fp = NULL;
    }
    if (user_stream->send_body_fp) {
        fclose(user_stream->send_body_fp);
        user_stream->send_body_fp = NULL;
    }
    free(user_stream->recv_buf);
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
    if (user_stream->total_file_size > 0) {
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
    int ret;
    char recv_buff[16384] = {0};
    ssize_t recv_buff_size, read, read_sum;
    unsigned char fin = 0;
    xqc_http_headers_t *headers = NULL;
    xqc_mini_svr_user_stream_t *user_stream = (xqc_mini_svr_user_stream_t *)strm_user_data;
    xqc_mini_svr_user_conn_t *user_conn = xqc_mini_svr_get_conn_from_request(user_stream);
    xqc_mini_svr_log_mp_active_state(user_conn, "h3_read");


    read = read_sum = 0;
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
        long parsed_index = 0;
        long parsed_count = 0;
        unsigned long long parsed_offset = 0;
        unsigned long long parsed_total = 0;

        printf("========== [server] Received HTTP/3 Request Headers ==========\n");
        for (size_t i = 0; i < headers->count; i++) {
            xqc_http_header_t *h = &headers->headers[i];
            printf("%.*s: %.*s\n",
                (int)h->name.iov_len, (char*)h->name.iov_base,
                (int)h->value.iov_len, (char*)h->value.iov_base);
            if (h->name.iov_len == strlen(":method")
                && strncasecmp((char *)h->name.iov_base, ":method",
                    h->name.iov_len) == 0) {
                char buf[16] = {0};
                size_t len = (h->value.iov_len < sizeof(buf) - 1)
                    ? h->value.iov_len : sizeof(buf) - 1;
                memcpy(buf, h->value.iov_base, len);
                if (strcasecmp(buf, "GET") == 0) {
                    user_stream->method = REQUEST_METHOD_GET;
                } else {
                    user_stream->method = REQUEST_METHOD_POST;
                }
                continue;
            }

            if (h->name.iov_len == strlen(":path")
                && strncasecmp((char *)h->name.iov_base, ":path",
                    h->name.iov_len) == 0) {
                size_t len = (h->value.iov_len < sizeof(user_stream->request_path) - 1)
                    ? h->value.iov_len : sizeof(user_stream->request_path) - 1;
                memcpy(user_stream->request_path, h->value.iov_base, len);
                user_stream->request_path[len] = '\0';
                continue;
            }
            if (h->name.iov_len == strlen("content-length")
                && strncasecmp((char*)h->name.iov_base, "content-length",
                               h->name.iov_len) == 0) {
                char buf[64] = {0};
                size_t len = (h->value.iov_len < sizeof(buf) - 1)
                    ? h->value.iov_len : sizeof(buf) - 1;
                memcpy(buf, h->value.iov_base, len);
                buf[len] = '\0';

                unsigned long long content_length = strtoull(buf, NULL, 10);
                user_stream->expected_content_length = content_length;
                printf("[server] >>> Parsed Content-Length: %llu bytes\n", content_length);
                continue;
            }

            if (strncasecmp((char *)h->name.iov_base, "x-stream-index", h->name.iov_len) == 0) {
                char buf[32] = {0};
                size_t len = (h->value.iov_len < sizeof(buf) - 1)
                    ? h->value.iov_len : sizeof(buf) - 1;
                memcpy(buf, h->value.iov_base, len);
                buf[len] = '\0';
                parsed_index = strtol(buf, NULL, 10);
                have_index = 1;
                continue;
            }

            if (strncasecmp((char *)h->name.iov_base, "x-stream-count", h->name.iov_len) == 0) {
                char buf[32] = {0};
                size_t len = (h->value.iov_len < sizeof(buf) - 1)
                    ? h->value.iov_len : sizeof(buf) - 1;
                memcpy(buf, h->value.iov_base, len);
                buf[len] = '\0';
                parsed_count = strtol(buf, NULL, 10);
                have_count = 1;
                continue;
            }

            if (strncasecmp((char *)h->name.iov_base, "x-stream-offset", h->name.iov_len) == 0) {
                char buf[64] = {0};
                size_t len = (h->value.iov_len < sizeof(buf) - 1)
                    ? h->value.iov_len : sizeof(buf) - 1;
                memcpy(buf, h->value.iov_base, len);
                buf[len] = '\0';
                parsed_offset = strtoull(buf, NULL, 10);
                have_offset = 1;
                continue;
            }

            if (strncasecmp((char *)h->name.iov_base, "x-total-length", h->name.iov_len) == 0) {
                char buf[64] = {0};
                size_t len = (h->value.iov_len < sizeof(buf) - 1)
                    ? h->value.iov_len : sizeof(buf) - 1;
                memcpy(buf, h->value.iov_base, len);
                buf[len] = '\0';
                parsed_total = strtoull(buf, NULL, 10);
                have_total = 1;
                continue;
            }
        }
        printf("=============================================================\n");

        if (user_stream->method == REQUEST_METHOD_GET) {
            size_t real_file_size = 0;
            if (xqc_mini_svr_get_target_file_size(user_stream, &real_file_size) != XQC_OK) {
                xqc_mini_svr_prepare_text_response(user_stream, 404,
                    "file %s not found", user_stream->request_path);
            } else {
                user_stream->total_file_size = real_file_size;
                if (have_index && have_count) {
                    
                    size_t stream_offset = 0;
                    size_t stream_length = 0;
                    xqc_mini_svr_compute_stream_segment((int)parsed_count,
                        (int)parsed_index, user_stream->total_file_size,
                        xqc_mini_svr_get_stream_range_size(user_stream),
                        &stream_offset, &stream_length);
                    if (have_total && parsed_total != user_stream->total_file_size) {
                        printf("[warn] total length mismatch, header=%llu file=%zu\n",
                            parsed_total, user_stream->total_file_size);
                    }
                    if (have_offset && parsed_offset != stream_offset) {
                        printf("[warn] stream offset mismatch, header=%llu computed=%zu\n",
                            parsed_offset, stream_offset);
                    }
                    user_stream->stream_index = (int)parsed_index;
                    user_stream->stream_count = (int)parsed_count;
                    user_stream->stream_offset = stream_offset;
                    user_stream->metadata_parsed = 1;
                    user_stream->requested_generated_length = stream_length;
                    user_stream->response_body_offset = stream_offset;
                } else {
                    user_stream->requested_generated_length = user_stream->total_file_size;
                    user_stream->response_body_offset = 0;
                }
            }
            if (!user_stream->response_prepared) {
                int prep_ret = xqc_mini_svr_prepare_file_response(user_stream);
                if (prep_ret == XQC_ERROR) {
                    xqc_mini_svr_prepare_text_response(user_stream, 500,
                        "failed to read %s", user_stream->resolved_path);
                }
                xqc_mini_svr_send_response(user_stream);
            }
            user_stream->header_recvd = 1;
        }else{
            if (!have_index || !have_count || !have_offset || !have_total) {
            printf("[error] missing stream metadata headers\n");
            return XQC_ERROR;
            }


            if (parsed_count <= 0) {
                printf("[error] invalid stream count %ld\n", parsed_count);
                return XQC_ERROR;
            }
            if (parsed_index < 0 || parsed_index >= parsed_count) {
                printf("[error] invalid stream index %ld for count %ld\n",
                    parsed_index, parsed_count);
                return XQC_ERROR;
            }

            user_stream->stream_index = (int)parsed_index;
            user_stream->stream_count = (int)parsed_count;
            user_stream->stream_offset = (size_t)parsed_offset;
            user_stream->total_file_size = (size_t)parsed_total;
            user_stream->metadata_parsed = 1;

            size_t expect_offset = 0;
            size_t expect_length = 0;
            xqc_mini_svr_compute_stream_segment(user_stream->stream_count,
                user_stream->stream_index, user_stream->total_file_size,
                xqc_mini_svr_get_stream_range_size(user_stream),
                &expect_offset, &expect_length);
            if (user_stream->stream_offset != expect_offset) {
                printf("[warn] stream offset mismatch, header=%zu computed=%zu, use computed\n",
                    user_stream->stream_offset, expect_offset);
                user_stream->stream_offset = expect_offset;
            }
            if (user_stream->expected_content_length != expect_length) {
                printf("[warn] stream length mismatch, header=%zu computed=%zu\n",
                    user_stream->expected_content_length, expect_length);
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

        if (user_stream->recv_body_fp == NULL) {
            if (xqc_mini_svr_prepare_output_file(user_stream) != XQC_OK) {
                return XQC_ERROR;
            }
        }
        do {
            read = xqc_h3_request_recv_body(h3_request, recv_buff, recv_buff_size, &fin);

            if (read == -XQC_EAGAIN) {
                break;

            } else if (read < 0) {
                printf("[error] xqc_h3_request_recv_body error %zd\n", read);
                return XQC_OK;
            }

            if (fseek(user_stream->recv_body_fp,
                    (long)(user_stream->stream_offset + user_stream->recv_body_len), SEEK_SET) != 0) {
                perror("fseek");
                return XQC_ERROR;
            }
            size_t written = fwrite(recv_buff, 1, (size_t)read, user_stream->recv_body_fp);
            if (written != (size_t)read) {
                perror("fwrite");
                return XQC_ERROR;
            }

            read_sum += read;
            user_stream->recv_body_len += read;
            xqc_mini_svr_update_upload_progress(user_stream);
            //printf("[server] received chunk: %zd bytes (total: %zu)\n", read, user_stream->recv_body_len);
        } while (read > 0);
    }
    if (fin) {
        user_stream->fin_received = 1;
        gettimeofday(&user_stream->end_time, NULL);
        printf("[stats] read h3 request finish. total %zu bytes\n", user_stream->recv_body_len);
        
    }
    if (user_stream->fin_received &&
        user_stream->recv_body_len >= user_stream->expected_content_length) {

        if (user_stream->recv_body_fp) {
            fflush(user_stream->recv_body_fp);
            fclose(user_stream->recv_body_fp);
            user_stream->recv_body_fp = NULL;
        }
        double duration_ms = (user_stream->end_time.tv_sec - user_stream->start_time.tv_sec) * 1000.0 +
                         (user_stream->end_time.tv_usec - user_stream->start_time.tv_usec) / 1000.0;

        double mbps = (user_stream->recv_body_len * 8.0) / (duration_ms * 1000.0); // Mbps

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
    if (!user_stream->send_started) {
        gettimeofday(&user_stream->send_start_time, NULL);
        user_stream->send_started = 1;
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
            size_t remaining = user_stream->response_body_len
                - user_stream->response_body_sent;
            size_t chunk = remaining < sizeof(user_stream->send_cache)
                ? remaining : sizeof(user_stream->send_cache);
            if (chunk == 0) {
                break;
            }
            size_t read_len = fread(user_stream->send_cache, 1, chunk,
                user_stream->send_body_fp);
            if (read_len == 0) {
                if (feof(user_stream->send_body_fp)) {
                    break;
                }
                perror("fread");
                return XQC_ERROR;
            }
            user_stream->buffered_len = read_len;
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


    int ret = xqc_mini_svr_send_body(user_stream);

    // printf("[stats] write notify handled for stream %d\n",
    //     user_stream->stream_index);
    return ret;
}
