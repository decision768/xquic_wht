/**
 * @file mini_server_cb.c contains callbacks definitions for mini_server, including:
 * 1. engine callbacks
 * 2. hq callbacks
 * 3. h3 callbacks
 */
#include "mini_client_cb.h"
#include <strings.h>

/**
 * @brief engine callbacks to trigger engine main logic 
 */
const char *line_break = "\n";
void
xqc_mini_cli_engine_cb(int fd, short what, void *arg)
{
    xqc_mini_cli_ctx_t *ctx = (xqc_mini_cli_ctx_t *) arg;

    xqc_engine_main_logic(ctx->engine);
}

int
xqc_mini_cli_open_log_file(void *arg)
{
    xqc_mini_cli_ctx_t *ctx = (xqc_mini_cli_ctx_t*)arg;
    return open(ctx->args->env_cfg.log_path, (O_WRONLY | O_APPEND | O_CREAT), 0644);
}

void
xqc_mini_cli_close_log_file(void *arg)
{
    xqc_mini_cli_ctx_t *ctx = (xqc_mini_cli_ctx_t*)arg;
    if (ctx->log_fd > 0) {
        close(ctx->log_fd);
        ctx->log_fd = 0;
    }
}

static void
xqc_mini_cli_write_zlog(xqc_mini_cli_ctx_t *ctx, xqc_log_level_t lvl, const void *buf, size_t size)
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
xqc_mini_cli_write_log_file(xqc_log_level_t lvl, const void *buf, size_t size, void *engine_user_data)
{
    xqc_mini_cli_ctx_t *ctx = (xqc_mini_cli_ctx_t*)engine_user_data;
    if (ctx->args->env_cfg.use_zlog && ctx->zlog_cat != NULL) {
        xqc_mini_cli_write_zlog(ctx, lvl, buf, size);
        return;
    }

    if (ctx->log_fd <= 0) {
        return;
    }
    //printf("%s", (char *)buf);
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


int
xqc_mini_cli_open_keylog_file(void *arg)
{
    xqc_mini_cli_ctx_t *ctx = (xqc_mini_cli_ctx_t*)arg;
    return open(ctx->args->env_cfg.key_out_path, (O_WRONLY | O_APPEND | O_CREAT), 0644);
}

void
xqc_mini_cli_close_keylog_file(void *arg)
{
    xqc_mini_cli_ctx_t *ctx = (xqc_mini_cli_ctx_t*)arg;
    if (ctx->keylog_fd > 0) {
        close(ctx->keylog_fd);
        ctx->keylog_fd = 0;
    }
}

void
xqc_mini_cli_write_qlog_file(qlog_event_importance_t imp, const void *buf, size_t size, void *engine_user_data)
{
    xqc_mini_cli_ctx_t *ctx = (xqc_mini_cli_ctx_t*)engine_user_data;
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


void
xqc_mini_cli_keylog_cb(const xqc_cid_t *scid, const char *line, void *engine_user_data)
{
    xqc_mini_cli_ctx_t *ctx = (xqc_mini_cli_ctx_t*)engine_user_data;

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
int
xqc_mini_cli_h3_conn_create_notify(xqc_h3_conn_t *conn, const xqc_cid_t *cid, void *user_data)
{
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)user_data;

    user_conn->h3_conn = conn;
    memcpy(&user_conn->cid, cid, sizeof(xqc_cid_t));

    return XQC_OK;
}

int
xqc_mini_cli_h3_conn_close_notify(xqc_h3_conn_t *conn, const xqc_cid_t *cid, void *user_data)
{
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)user_data;

    xqc_mini_cli_diag_maybe_log(user_conn, "h3_conn_close", 1);
    event_base_loopbreak(user_conn->ctx->eb);
    printf("[stats] xqc_mini_cli_h3_conn_close_notify success \n");
    return XQC_OK;
}

void
xqc_mini_cli_h3_conn_handshake_finished(xqc_h3_conn_t *h3_conn, void *user_data)
{
    (void)h3_conn;
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)user_data;
    if (user_conn == NULL) {
        return;
    }

    xqc_mini_cli_try_rebuild_paths(user_conn);
    user_conn->handshake_finished = 1;
    user_conn->path_wait_rounds_after_handshake = 0;
    printf("[stats] active paths after handshake: %d\n", user_conn->active_path_cnt);

    if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
        && !xqc_mini_cli_paths_ready_for_requests(user_conn))
    {
        printf("[diag_path_gate] wait after handshake ready=%d target=%d round=0/%d\n",
            xqc_mini_cli_get_ready_path_count(user_conn),
            xqc_mini_cli_get_target_path_count(user_conn),
            XQC_MINI_PATH_WAIT_MAX_ROUNDS);
        xqc_mini_cli_schedule_path_retry(user_conn,
            XQC_MINI_PATH_RETRY_INTERVAL_USEC);
        return;
    }

    if (xqc_mini_cli_launch_requests(user_conn) != XQC_OK) {
        printf("[error] launch requests after handshake failed\n");
    }

    return;
}
int
xqc_mini_cli_h3_request_create_notify(xqc_h3_request_t *h3_request, void *h3s_user_data)
{
    // xqc_mini_cli_user_stream_t *user_stream = (xqc_mini_cli_user_stream_t *)h3s_user_data;
    // if (user_stream == NULL || user_stream->user_conn == NULL || user_stream->user_conn->ctx == NULL) {
    //     return 0;
    // }

    // xqc_mini_cli_user_conn_t *user_conn = user_stream->user_conn;
    // xqc_mini_cli_args_t *args = user_conn->ctx->args;

    // if (args->req_cfg.method == REQUEST_METHOD_GET
    //     && args->quic_cfg.multipath
    //     && user_conn->target_requests == 1)
    // {
    //     xqc_h3_priority_t prio;
    //     xqc_h3_priority_init(&prio);
    //     prio.schedule = 1;
    //     prio.reinject = 1;
    //     if (xqc_h3_request_set_priority(h3_request, &prio) != XQC_OK) {
    //         printf("[warn] stream %d set multipath priority failed\n", user_stream->stream_index);

    //     } else {
    //         printf("[stats] stream %d enable per-stream multipath schedule/reinject\n",
    //             user_stream->stream_index);
    //     }
    // }
    return 0;
}

int
xqc_mini_cli_h3_request_close_notify(xqc_h3_request_t *h3_request, void *user_data)
{
    xqc_mini_cli_user_stream_t *user_stream = (xqc_mini_cli_user_stream_t *)user_data;
    xqc_mini_cli_user_conn_t *user_conn = user_stream->user_conn;
    xqc_mini_cli_ctx_t *conn_ctx = user_conn->ctx;
    xqc_request_stats_t stats = xqc_h3_request_get_stats(h3_request);

    if (user_stream->send_body_fp) {
        fclose(user_stream->send_body_fp);
        user_stream->send_body_fp = NULL;
    }
    if (user_stream->recv_body_fp) {
        fclose(user_stream->recv_body_fp);
        user_stream->recv_body_fp = NULL;
    }
    free(user_stream->send_buffer);
    user_stream->send_buffer = NULL;

    user_conn->completed_requests++;
    if (!user_stream->slot_released && user_conn->active_requests > 0) {
        user_conn->active_requests--;
        user_stream->slot_released = 1;
    }


    if (user_conn->completed_requests < user_conn->target_requests) {
        if (xqc_mini_cli_try_schedule_requests(user_conn) != XQC_OK) {
            printf("[error] schedule next request round failed\n");
            xqc_mini_cli_schedule_path_retry(user_conn, XQC_MINI_PATH_RETRY_FAST_INTERVAL_USEC);
        }
    }

    printf("[stats] stream %d close notify, completed %d/%d, active=%d, cwnd_blocked:%"PRIu64"\n",
        user_stream->stream_index, user_conn->completed_requests,
        user_conn->target_requests, user_conn->active_requests, stats.cwnd_blocked_ms);

     if (user_conn->completed_requests >= user_conn->target_requests
        && user_conn->active_requests == 0)
    {
        xqc_h3_conn_close(conn_ctx->engine, &user_conn->cid);
    }

    free(user_stream);

    //printf("[stats] xqc_mini_cli_h3_request_close_notify success, cwnd_blocked:%"PRIu64"\n", stats.cwnd_blocked_ms);
    return 0;
}
/*用于触发接收事件回调，常用于GET方法或者接收response*/
int
xqc_mini_cli_h3_request_read_notify(xqc_h3_request_t *h3_request, 
    xqc_request_notify_flag_t flag, void *h3s_user_data)
{
    char recv_buff[XQC_MAX_BUFF_SIZE] = {0};
    size_t recv_buff_size;
    ssize_t read, read_sum;
    unsigned char fin = 0;
    xqc_mini_cli_user_stream_t *user_stream = (xqc_mini_cli_user_stream_t *)h3s_user_data;
    xqc_mini_cli_user_conn_t *user_conn = user_stream->user_conn;

    if (flag & XQC_REQ_NOTIFY_READ_HEADER) {
        xqc_http_headers_t *headers;
        headers = xqc_h3_request_recv_headers(h3_request, &fin);
        if (headers == NULL) {
            printf("[error] xqc_h3_request_recv_headers error\n");
            return XQC_ERROR;
        }
        int status_code = 0;
        for (int i = 0; i < headers->count; i++) {
            printf("[receive report] %.*s = %.*s\n",
                (int)headers->headers[i].name.iov_len,
                (char *)headers->headers[i].name.iov_base,
                (int)headers->headers[i].value.iov_len,
                (char *)headers->headers[i].value.iov_base);
            if (headers->headers[i].name.iov_len == strlen(":status")
                && strncmp((char *)headers->headers[i].name.iov_base, ":status",
                           headers->headers[i].name.iov_len) == 0) {
                char status_buf[4] = {0};
                size_t len = headers->headers[i].value.iov_len;
                if (len >= sizeof(status_buf)) {
                    len = sizeof(status_buf) - 1;
                }
                memcpy(status_buf, headers->headers[i].value.iov_base, len);
                status_code = atoi(status_buf);
            }else if (strncasecmp((char *)headers->headers[i].name.iov_base,
                "content-length", headers->headers[i].name.iov_len) == 0) {
                char length_buf[32] = {0};
                size_t len = headers->headers[i].value.iov_len;
                if (len >= sizeof(length_buf)) {
                    len = sizeof(length_buf) - 1;
                }
                memcpy(length_buf, headers->headers[i].value.iov_base, len);
                user_stream->expected_content_length = strtoull(length_buf, NULL, 10);
            }else if (strncasecmp((char *)headers->headers[i].name.iov_base,
                "x-total-length", headers->headers[i].name.iov_len) == 0) {
                char total_buf[32] = {0};
                size_t len = headers->headers[i].value.iov_len;
                if (len >= sizeof(total_buf)) {
                    len = sizeof(total_buf) - 1;
                }
                memcpy(total_buf, headers->headers[i].value.iov_base, len);
                size_t total_length = strtoull(total_buf, NULL, 10);
                if (total_length > 0) {
                    user_conn->download_expected_bytes = total_length;
                    user_conn->send_file_size = total_length;
                }
            }
        }

        user_stream->response_status = status_code;

        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && user_conn->download_expected_bytes > 0
            && !user_conn->total_requests_known)
        {
            size_t stream_range_size = user_conn->ctx->args->net_cfg.stream_range_size > 0
                ? user_conn->ctx->args->net_cfg.stream_range_size
                : (4*1024*1024);
            int required_streams = (int)((user_conn->download_expected_bytes + stream_range_size - 1)
                / stream_range_size);
            if (required_streams <= 0) {
                required_streams = 1;
            }
            if (required_streams < user_conn->next_stream_index) {
                required_streams = user_conn->next_stream_index;
            }
            user_conn->target_requests = required_streams;
            user_conn->total_requests_known = 1;
            printf("[stats] GET total length=%zu, total rounds stream count=%d, concurrency=%d\n",
                user_conn->download_expected_bytes, user_conn->target_requests,
                user_conn->max_concurrent_requests);
            if (xqc_mini_cli_try_schedule_requests(user_conn) != XQC_OK) {
                printf("[error] schedule GET rounds failed\n");
            }
        }

        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && status_code >= 200 && status_code < 300
            && user_conn->download_fd < 0) {
            const char *download_path = user_conn->ctx->args->env_cfg.download_path;
            printf("[stats] response status: %d, download_path: %s\n",
                status_code, download_path);
            user_conn->download_fd = open(download_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (user_conn->download_fd < 0) {
                perror("open");
                return XQC_ERROR;
            }
            if (xqc_mini_cli_start_async_writer(user_conn) != XQC_OK) {
                close(user_conn->download_fd);
                user_conn->download_fd = -1;
                return XQC_ERROR;
            }
            
            strncpy(user_conn->download_path, download_path,
                sizeof(user_conn->download_path) - 1);
            user_conn->download_path[sizeof(user_conn->download_path) - 1] = '\0';
            printf("[stats] response body will be stored in %s\n", download_path);
        }
        user_stream->start_time = xqc_now();
        if (fin) {
            /* only header in request */
            user_stream->recv_fin = 1;
            printf("[stats] h3 request read header finish \n");
            
            return XQC_OK;
        }
        
    }

    /* continue to recv body */
    if (!(flag & XQC_REQ_NOTIFY_READ_BODY)) {
        return XQC_OK;
    }

    recv_buff_size = XQC_MAX_BUFF_SIZE;
    read = read_sum = 0;
    
    do {
        read = xqc_h3_request_recv_body(h3_request, recv_buff, recv_buff_size, &fin);
        if (read == -XQC_EAGAIN) {
            break;

        } else if (read < 0) {
            printf("xqc_h3_request_recv_body error %zd\n", read);
            return XQC_OK;
        }
    
        read_sum += read;
        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && read > 0)
        {
            xqc_usec_t body_recv_time = xqc_now();
            if (user_conn->download_first_body_time == 0) {
                user_conn->download_first_body_time = body_recv_time;
            }
            user_conn->download_last_body_time = body_recv_time;
        }

        size_t prev_len = user_stream->recv_body_len;
        user_stream->recv_body_len += read;
        
        
        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET) {
            user_conn->download_received_bytes += (size_t)read;
            if (user_conn->download_expected_bytes > 0) {
                double progress = (double)user_conn->download_received_bytes * 100.0
                    / user_conn->download_expected_bytes;
                if (progress > 100.0) {
                    progress = 100.0;
                }
                int progress_int = (int)progress;
                int previous_progress = user_conn->download_progress_percent;
                if (progress_int != previous_progress) {
                    user_conn->download_progress_percent = progress_int;
                    if (progress_int == 100
                        || progress_int / 10 > previous_progress / 10)
                    {
                        printf("[download] %d%%\n", progress_int);
                    }
                }
            }

        }

        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && user_conn->download_fd >= 0 && read > 0) {
            off_t write_off = (off_t)(user_stream->recv_offset + prev_len);
            if (xqc_mini_cli_async_pwrite(user_conn,
                    (const unsigned char *)recv_buff, (size_t)read, write_off) != XQC_OK) {
                printf("[error] async pwrite enqueue failed\n");
                return XQC_ERROR;
            }
        }
    } while (read > 0 && !fin);

    xqc_mini_cli_diag_maybe_log(user_conn, "h3_read", 0);

    //printf("[report] xqc_h3_request_recv_body size %zd, fin:%d\n", read, fin);

    if (fin) {
        xqc_usec_t end_time = xqc_now();
        double duration_ms = (user_stream->start_time > 0)
            ? (end_time - user_stream->start_time) / 1000.0
            : 0.0;
        double mbps = duration_ms > 0
            ? (user_stream->recv_body_len * 8.0) / (duration_ms * 1000.0)
            : 0.0;
        printf("[stats] recv finished: %zu bytes, time=%.3f ms, speed=%.3f Mbps\n",
            user_stream->recv_body_len, duration_ms, mbps);
        printf("[stats] read h3 request finish. \n");
        user_stream->recv_fin = 1;

        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && !user_stream->slot_released)
        {
            if (user_conn->active_requests > 0) {
                user_conn->active_requests--;
            }
            user_stream->slot_released = 1;
            if (user_conn->next_stream_index < user_conn->target_requests) {
                if (xqc_mini_cli_try_schedule_requests(user_conn) != XQC_OK) {
                    printf("[error] schedule next GET round failed\n");
                    xqc_mini_cli_schedule_path_retry(user_conn, XQC_MINI_PATH_RETRY_FAST_INTERVAL_USEC);
                }
            }
        }
        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && user_stream->expected_content_length > 0) {
            printf("\n");
        }
        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && user_conn->download_fd >= 0) {
            user_conn->download_total_bytes += user_stream->recv_body_len;
            user_conn->download_finished_streams++;
            if (user_conn->download_finished_streams >= user_conn->target_requests) {
                if (xqc_mini_cli_flush_async_writer(user_conn) != XQC_OK) {
                    printf("[error] async writer failed before download completion\n");
                    xqc_mini_cli_stop_async_writer(user_conn);
                    close(user_conn->download_fd);
                    user_conn->download_fd = -1;
                    return XQC_ERROR;
                }
                xqc_usec_t fsync_start_us = xqc_now();
                int fsync_ret = fsync(user_conn->download_fd);
                xqc_usec_t fsync_end_us = xqc_now();
                xqc_mini_cli_diag_record_fsync(user_conn,
                    fsync_end_us >= fsync_start_us ? fsync_end_us - fsync_start_us : 0);
                if (fsync_ret != 0) {
                    perror("fsync");
                }
                double duration_ms = (user_conn->download_first_body_time > 0
                    && user_conn->download_last_body_time
                        > user_conn->download_first_body_time)
                    ? (user_conn->download_last_body_time
                        - user_conn->download_first_body_time) / 1000.0
                    : 0.0;
                double mbps = duration_ms > 0
                    ? (user_conn->download_total_bytes * 8.0) / (duration_ms * 1000.0)
                    : 0.0;
                printf("[stats] multi-stream download complete, %zu bytes saved to %s\n",
                    user_conn->download_total_bytes,
                    user_conn->download_path[0] ? user_conn->download_path
                                                : user_conn->ctx->args->env_cfg.download_path);
                printf("[stats] all download streams finished: %zu bytes, time=%.3f ms, speed=%.3f Mbps\n",
                    user_conn->download_total_bytes, duration_ms, mbps);
                printf("[stats] GET timing=first_body_to_last_body excludes_async_flush_fsync=1\n");
                xqc_mini_cli_diag_maybe_log(user_conn, "download_done", 1);
                xqc_mini_cli_stop_async_writer(user_conn);
                close(user_conn->download_fd);
                user_conn->download_fd = -1;
                xqc_h3_conn_close(user_conn->ctx->engine, &user_conn->cid);
                xqc_engine_main_logic(user_conn->ctx->engine);
            }
        }
        
        
    }

    return XQC_OK;
}

int
xqc_mini_cli_h3_request_write_notify(xqc_h3_request_t *h3_request, void *h3s_user_data)
{
    int ret = 0;
    xqc_mini_cli_user_stream_t *user_stream = (xqc_mini_cli_user_stream_t *)h3s_user_data;
    
    ret = xqc_mini_cli_request_send(h3_request, user_stream);
    
    //printf("[stats] finish h3 request write notify!:%"PRIu64"\n", xqc_h3_stream_id(h3_request));
    
    return ret;
}

void
xqc_mini_cli_set_event_timer(xqc_usec_t wake_after, void *user_data)
{
    xqc_mini_cli_ctx_t *ctx = (xqc_mini_cli_ctx_t *) user_data;
    //printf("xqc_engine_wakeup_after %llu us, now %llu\n", wake_after, xqc_now());

    struct timeval tv;
    tv.tv_sec = wake_after / 1000000;
    tv.tv_usec = wake_after % 1000000;
    event_add(ctx->ev_engine, &tv);
}

ssize_t
xqc_mini_cli_write_socket(const unsigned char *buf, size_t size, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, void *conn_user_data)
{
    return xqc_mini_cli_write_socket_ex(0, buf, size, peer_addr, peer_addrlen, conn_user_data);
}

ssize_t
xqc_mini_cli_write_socket_ex(uint64_t path_id, const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data)
{
    const int max_write_failures = 1;
    int fd = -1;
    ssize_t res = 0;
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)conn_user_data;
    
    xqc_mini_cli_user_path_t *user_path = NULL;
    xqc_mini_cli_user_path_t *fallback_path = NULL;
    
    for (int i = 0; i < MAX_PATH_CNT; i++) {
        if (!user_conn->paths[i].is_active
            || user_conn->paths[i].consecutive_write_failures >= max_write_failures) {
            continue;
        }
        if (user_conn->paths[i].path_id == path_id) {
            user_path = &user_conn->paths[i];
            break;
        }
    }

    if (user_path == NULL) {
        int start = user_conn->next_fallback_path_index;
        if (start < 0 || start >= MAX_PATH_CNT) {
            start = 0;
        }

        for (int n = 0; n < MAX_PATH_CNT; n++) {
            int idx = (start + n) % MAX_PATH_CNT;
            if (!user_conn->paths[idx].is_active
                || user_conn->paths[idx].consecutive_write_failures >= max_write_failures) {
                continue;
            }
            fallback_path = &user_conn->paths[idx];
            user_conn->next_fallback_path_index = (idx + 1) % MAX_PATH_CNT;
            break;
        }
        user_path = fallback_path;
    }

    if (user_path == NULL || !user_path->is_active || user_path->fd < 0) {
        xqc_mini_cli_try_rebuild_paths(user_conn);
        xqc_mini_cli_schedule_path_retry(user_conn, XQC_MINI_PATH_RETRY_FAST_INTERVAL_USEC);
        return XQC_SOCKET_EAGAIN;
    }

    fd = user_path->fd;

    do {
        set_sys_errno(0);
        res = sendto(fd, buf, size, 0, peer_addr, peer_addrlen);
        if (res < 0) {
        
            // struct sockaddr_in *la = (struct sockaddr_in*)user_path->local_addr;
            // struct sockaddr_in *pa = (struct sockaddr_in*)user_path->peer_addr;
            // char local_ip[INET_ADDRSTRLEN];
            // char peer_ip[INET_ADDRSTRLEN];

            // inet_ntop(AF_INET, &la->sin_addr, local_ip, sizeof local_ip);
            // inet_ntop(AF_INET, &pa->sin_addr, peer_ip, sizeof peer_ip);

            //  printf("xqc_mini_cli_write_socket err %zd %s, fd: %d, path_id: %"PRIu64", address_path: %s,peer_address:%s\n",
            //      res, strerror(get_sys_errno()), fd, user_path->path_id,local_ip,peer_ip);
            
            if (get_sys_errno() == EAGAIN) {
                //user_conn->ctx->args->net_cfg.last_socket_time = xqc_now();
                res = XQC_SOCKET_EAGAIN;
            }
        }
    } while ((res < 0) && (get_sys_errno() == EINTR));

    // printf("[report] xqc_mini_cli_write_socket_ex success size=%lu\n", size);
    if (res >= 0) {
        user_path->consecutive_write_failures = 0;
        __atomic_store_n(&user_conn->ctx->args->net_cfg.last_socket_time,
                         xqc_now(), __ATOMIC_RELEASE);
    }
    else if (res != XQC_SOCKET_EAGAIN) {
        printf("[error] xqc_mini_cli_write_socket_ex failed, path_id=%"PRIu64", err=%s\n",
            user_path->path_id, strerror(get_sys_errno()));
        user_path->consecutive_write_failures++;
        if (user_path->consecutive_write_failures >= max_write_failures
            && user_path->path_id != XQC_MINI_PATH_ID_INVALID)
        {
            uint64_t dead_path_id = user_path->path_id;
            printf("[warn] path_id=%"PRIu64" reached write failure threshold, closing path\n",
                dead_path_id);
            xqc_mini_cli_stop_path_rx_thread(user_path);

            user_path->is_active = 0;
            user_path->prepared = 0;
            user_path->path_id = XQC_MINI_PATH_ID_INVALID;
            user_path->consecutive_write_failures = 0;

            if (user_path->ev_socket) {
                event_del(user_path->ev_socket);
                event_free(user_path->ev_socket);
                user_path->ev_socket = NULL;
            }
            if (user_path->fd >= 0) {
                close(user_path->fd);
                user_path->fd = -1;
            }

            if (user_conn->active_path_cnt > 0) {
                user_conn->active_path_cnt--;
            }
            if (user_conn->ctx->args->quic_cfg.multipath
                && user_conn->handshake_finished && user_conn->active_path_cnt > 0)
            {
                xqc_int_t ret = xqc_conn_close_path(user_conn->ctx->engine,
                                                    &user_conn->cid, dead_path_id);
                if (ret != XQC_OK) {
                    printf("[diag_rx_path_state] action=close_failed path_id=%" PRIu64
                           " ret=%d reason=write_failure\n", dead_path_id, ret);
                }
                xqc_mini_cli_try_rebuild_paths(user_conn);
                xqc_mini_cli_schedule_path_retry(user_conn,
                    XQC_MINI_PATH_RETRY_FAST_INTERVAL_USEC);
            } else {
                xqc_int_t ret = xqc_h3_conn_close(user_conn->ctx->engine,
                                                  &user_conn->cid);
                if (ret != XQC_OK) {
                    printf("[diag_rx_path_state] action=conn_close_failed ret=%d"
                           " reason=write_failure\n", ret);
                }
            }

            xqc_engine_main_logic(user_conn->ctx->engine);
            return XQC_SOCKET_EAGAIN;
        }
    }


    return res;
}

int
xqc_mini_cli_read_token(unsigned char *token, unsigned token_len)
{
    int fd = open(TOKEN_FILE, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    ssize_t n = read(fd, token, token_len);
    close(fd);
    return n;
}

void
xqc_mini_cli_save_token(const unsigned char *token, unsigned token_len, void *user_data)
{
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)user_data;
    printf("[stats] start xqc_mini_cli_save_token, use client ip as the key.\n");

    int fd = open(TOKEN_FILE, O_TRUNC | O_CREAT | O_WRONLY, 0666);
    if (fd < 0) {
        printf("save token error %s\n", strerror(get_sys_errno()));
        return;
    }

    ssize_t n = write(fd, token, token_len);
    if (n < token_len) {
        printf("save token error %s\n", strerror(get_sys_errno()));
        close(fd);
        return;
    }
    close(fd);
}

void
xqc_mini_cli_save_session_cb(const char * data, size_t data_len, void *user_data)
{
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)user_data;
    printf("[stats] start save_session_cb. \n");

    FILE * fp  = fopen(SESSION_TICKET_FILE, "wb");
    if (fp < 0) {
        printf("save session error %s\n", strerror(get_sys_errno()));
        return;
    }

    int write_size = fwrite(data, 1, data_len, fp);
    if (data_len != write_size) {
        printf("save _session_cb error\n");
        fclose(fp);
        return;
    }
    fclose(fp);
    return;
}


void
xqc_mini_cli_save_tp_cb(const char * data, size_t data_len, void * user_data)
{
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)user_data;
    printf("[stats] start save_tp_cb\n");

    FILE * fp = fopen(TRANSPORT_PARAMS_FILE, "wb");
    if (fp < 0) {
        printf("save transport callback error %s\n", strerror(get_sys_errno()));
        return;
    }

    int write_size = fwrite(data, 1, data_len, fp);
    if (data_len != write_size) {
        printf("save _tp_cb error\n");
        fclose(fp);
        return;
    }

    fclose(fp);
    return;
}


void
xqc_mini_cli_timeout_callback(int fd, short what, void *arg)
{
    int conn_timeout, ret;
    xqc_usec_t socket_idle_time;
    struct timeval tv;
    xqc_mini_cli_ctx_t *ctx;
    xqc_mini_cli_user_conn_t *user_conn;

    user_conn = (xqc_mini_cli_user_conn_t *)arg;
    ctx = user_conn->ctx;
    conn_timeout = ctx->args->net_cfg.conn_timeout;
    xqc_usec_t last_socket_time = __atomic_load_n(
        &ctx->args->net_cfg.last_socket_time, __ATOMIC_ACQUIRE);
    socket_idle_time = xqc_now() - last_socket_time;
    //printf("[stats] client connection idle time: %llu us, conn_timeout:%d,last_socket_time:%d\n", socket_idle_time,conn_timeout,last_socket_time);
    if (socket_idle_time < conn_timeout * 1000000) {
        user_conn->timeout_defer_count = 0;
        tv.tv_sec = conn_timeout;
        tv.tv_usec = 0;
        event_add(user_conn->ev_timeout, &tv);
        return;
    }

    if ((user_conn->active_requests > 0
            || user_conn->next_stream_index < user_conn->target_requests)
        && user_conn->completed_requests < user_conn->target_requests
        && user_conn->active_path_cnt > 0
        && user_conn->timeout_defer_count < 6)
    {
        user_conn->timeout_defer_count++;
        printf("[warn] idle timeout deferred (%d/6), active=%d completed=%d/%d paths=%d\n",
            user_conn->timeout_defer_count, user_conn->active_requests,
            user_conn->completed_requests, user_conn->target_requests,
            user_conn->active_path_cnt);
        if (user_conn->requests_launched
            && user_conn->active_requests < user_conn->max_concurrent_requests
            && user_conn->next_stream_index < user_conn->target_requests)
        {
            if (xqc_mini_cli_try_schedule_requests(user_conn) != XQC_OK) {
                printf("[warn] timeout retry schedule failed, active=%d next=%d target=%d\n",
                    user_conn->active_requests, user_conn->next_stream_index,
                    user_conn->target_requests);
            }
        }
        (void)xqc_h3_conn_send_ping(ctx->engine, &user_conn->cid, NULL);
        tv.tv_sec = (long)(XQC_MINI_TIMEOUT_DEFER_CHECK_USEC / 1000000);
        tv.tv_usec = (long)(XQC_MINI_TIMEOUT_DEFER_CHECK_USEC % 1000000);
        event_add(user_conn->ev_timeout, &tv);
        return;
    }

conn_close:
    printf("[stats] client process timeout, connection closing... \n");
    xqc_mini_cli_diag_maybe_log(user_conn, "timeout", 1);
    ret = xqc_h3_conn_close(ctx->engine, &user_conn->cid);
    if (ret) {
        printf("[error] xqc_conn_close error:%d\n", ret);
        return;
    }
}

int
xqc_mini_cli_conn_create_notify(xqc_connection_t *conn, const xqc_cid_t *cid, void *user_data, void *conn_proto_data)
{
    DEBUG;

    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *) user_data;
    xqc_conn_set_alp_user_data(conn, user_conn);

    printf("[stats] xqc_conn_is_ready_to_send_early_data:%d\n", xqc_conn_is_ready_to_send_early_data(conn));
    return XQC_OK;
}
void
xqc_mini_cli_path_retry_callback(int fd, short what, void *arg)
{
    xqc_mini_cli_user_conn_t *user_conn = (xqc_mini_cli_user_conn_t *)arg;
    struct timeval tv;

    if (user_conn == NULL || user_conn->ctx == NULL) {
        return;
    }
    if (xqc_mini_cli_handle_failed_rx_threads(user_conn)) {
        return;
    }
    if (!user_conn->handshake_finished) {
        xqc_mini_cli_schedule_path_retry(user_conn, XQC_MINI_PATH_RETRY_INTERVAL_USEC);
        return;
    }


    xqc_mini_cli_try_rebuild_paths(user_conn);

    xqc_usec_t now = xqc_now();
    if (user_conn->handshake_finished
        && user_conn->active_path_cnt < xqc_mini_cli_get_target_path_count(user_conn)
        && (user_conn->last_path_probe_ping_time == 0
            || now - user_conn->last_path_probe_ping_time >= XQC_MINI_PATH_PROBE_PING_INTERVAL_USEC))

    {
        xqc_int_t ping_ret = xqc_h3_conn_send_ping(user_conn->ctx->engine,
                                                   &user_conn->cid, NULL);
        if (ping_ret != XQC_OK) {
            printf("[warn] path-probe ping failed, ret=%d\n", ping_ret);
        }else {
            user_conn->last_path_probe_ping_time = now;
        }
    }



    if (user_conn->handshake_finished && !user_conn->requests_launched) {
        int should_wait = user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && !xqc_mini_cli_paths_ready_for_requests(user_conn)
            && user_conn->path_wait_rounds_after_handshake
                < XQC_MINI_PATH_WAIT_MAX_ROUNDS;

        if (should_wait) {
            user_conn->path_wait_rounds_after_handshake++;
            if (user_conn->path_wait_rounds_after_handshake
                < XQC_MINI_PATH_WAIT_MAX_ROUNDS)
            {
                printf("[diag_path_gate] waiting ready=%d target=%d round=%d/%d\n",
                    xqc_mini_cli_get_ready_path_count(user_conn),
                    xqc_mini_cli_get_target_path_count(user_conn),
                    user_conn->path_wait_rounds_after_handshake,
                    XQC_MINI_PATH_WAIT_MAX_ROUNDS);
                xqc_mini_cli_schedule_path_retry(user_conn,
                    XQC_MINI_PATH_RETRY_INTERVAL_USEC);
                return;
            }
        }

        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET) {
            printf("[diag_path_gate] launch ready=%d target=%d waited_rounds=%d fallback=%d\n",
                xqc_mini_cli_get_ready_path_count(user_conn),
                xqc_mini_cli_get_target_path_count(user_conn),
                user_conn->path_wait_rounds_after_handshake,
                xqc_mini_cli_paths_ready_for_requests(user_conn) ? 0 : 1);
        }
        if (xqc_mini_cli_launch_requests(user_conn) != XQC_OK) {
            printf("[error] launch requests in path retry callback failed\n");
        }
    }

    if (user_conn->requests_launched
        && user_conn->active_requests < user_conn->max_concurrent_requests
        && user_conn->next_stream_index < user_conn->target_requests)
    {
        if (xqc_mini_cli_try_schedule_requests(user_conn) != XQC_OK) {
            printf("[warn] periodic request reschedule failed, active=%d next=%d target=%d\n",
                user_conn->active_requests, user_conn->next_stream_index,
                user_conn->target_requests);
        }
    }

    xqc_mini_cli_schedule_path_retry(user_conn, XQC_MINI_PATH_RETRY_INTERVAL_USEC);
}
