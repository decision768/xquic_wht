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
    int target_paths = user_conn->ctx->args->net_cfg.multi_interface_cnt;
    if (target_paths <= 0) {
        target_paths = MAX_PATH_CNT;
    }
    if (target_paths > MAX_PATH_CNT) {
        target_paths = MAX_PATH_CNT;
    }
    if (user_conn->active_path_cnt < target_paths) {
        printf("[stats] waiting for additional path ids from peer, path retry timer continues\n");
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
    printf("[stats] stream %d close notify, completed %d/%d, cwnd_blocked:%"PRIu64"\n",
        user_stream->stream_index, user_conn->completed_requests,
        user_conn->target_requests, stats.cwnd_blocked_ms);

    if (user_conn->completed_requests >= user_conn->target_requests) {
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
                user_conn->download_expected_bytes = strtoull(total_buf, NULL, 10);
            }
        }

        user_stream->response_status = status_code;

        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && status_code >= 200 && status_code < 300
            && user_conn->download_fp == NULL) {
            const char *download_path = user_conn->ctx->args->env_cfg.download_path;
            printf("[stats] response status: %d, download_path: %s\n",
                status_code, download_path);
            user_conn->download_fp = fopen(download_path, "wb+");
            if (user_conn->download_fp == NULL) {
                perror("fopen");
                return XQC_ERROR;
            }
            setvbuf(user_conn->download_fp, NULL, _IOFBF, 4 * 1024 * 1024);
            strncpy(user_conn->download_path, download_path,
                sizeof(user_conn->download_path) - 1);
            user_conn->download_path[sizeof(user_conn->download_path) - 1] = '\0';
            if (user_conn->download_start_time == 0) {
                user_conn->download_start_time = xqc_now();
            }
            printf("[stats] response body will be stored in %s\n", download_path);
        }
        user_stream->start_time = xqc_now();
        if (fin) {
            /* only header in request */
            user_stream->recv_fin = 1;
            printf("[stats] h3 request read header finish \n");
            if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET){
                xqc_h3_conn_close(user_conn->ctx->engine, &user_conn->cid);
                xqc_engine_main_logic(user_conn->ctx->engine);
            }
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
                if (progress_int != user_conn->download_progress_percent) {
                    user_conn->download_progress_percent = progress_int;
                    printf("\r[download] %d%%", user_conn->download_progress_percent);
                    fflush(stdout);
                }
            }

        }

        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && user_conn->download_fp != NULL && read > 0) {
            if (fseek(user_conn->download_fp, (long)(user_stream->recv_offset + prev_len),
                    SEEK_SET) != 0) {
                perror("fseek");
                return XQC_ERROR;
            }

            size_t written = fwrite(recv_buff, 1, (size_t)read,
                user_conn->download_fp);
            if (written != (size_t)read) {
                perror("fwrite");
                return XQC_ERROR;
            }
        }
    } while (read > 0 && !fin);

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
            && user_stream->expected_content_length > 0) {
            printf("\n");
        }
        if (user_conn->ctx->args->req_cfg.method == REQUEST_METHOD_GET
            && user_conn->download_fp != NULL) {
            user_conn->download_total_bytes += user_stream->recv_body_len;
            user_conn->download_finished_streams++;
            if (user_conn->download_finished_streams >= user_conn->target_requests) {
                fflush(user_conn->download_fp);
                xqc_usec_t download_end_time = xqc_now();
                double duration_ms = (user_conn->download_start_time > 0)
                    ? (download_end_time - user_conn->download_start_time) / 1000.0
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
                fclose(user_conn->download_fp);
                user_conn->download_fp = NULL;
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
        return -1;
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
        user_conn->ctx->args->net_cfg.last_socket_time = xqc_now();
    }
    else if (res != XQC_SOCKET_EAGAIN) {
        printf("[error] xqc_mini_cli_write_socket_ex failed, path_id=%"PRIu64", err=%s\n",
            user_path->path_id, strerror(get_sys_errno()));
        user_path->consecutive_write_failures++;
        if (user_path->consecutive_write_failures >= max_write_failures
            && user_path->path_id != XQC_MINI_PATH_ID_INVALID)
        {
            printf("[warn] path_id=%"PRIu64" reached write failure threshold, closing path\n",
                user_path->path_id);
            user_path->is_active = 0;
            if (user_conn->active_path_cnt > 0) {
                user_conn->active_path_cnt--;
            }
            xqc_conn_close_path(user_conn->ctx->engine, &user_conn->cid, user_path->path_id);
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
    xqc_usec_t last_socket_time = ctx->args->net_cfg.last_socket_time;
    socket_idle_time = xqc_now() - last_socket_time;
    //printf("[stats] client connection idle time: %llu us, conn_timeout:%d,last_socket_time:%d\n", socket_idle_time,conn_timeout,last_socket_time);
    if (socket_idle_time < conn_timeout * 1000000) {
        tv.tv_sec = conn_timeout;
        tv.tv_usec = 0;
        event_add(user_conn->ev_timeout, &tv);
        return;
    }

conn_close:
    printf("[stats] client process timeout, connection closing... \n");
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

    xqc_mini_cli_try_rebuild_paths(user_conn);

     if (user_conn->handshake_finished
        && user_conn->requests_launched
        && user_conn->active_path_cnt > 1)
    {
        xqc_int_t ping_ret = xqc_h3_conn_send_ping(user_conn->ctx->engine,
                                                   &user_conn->cid, NULL);
        if (ping_ret != XQC_OK) {
            printf("[warn] periodic mp ping failed, ret=%d\n", ping_ret);
        }
    }



     if (user_conn->handshake_finished && !user_conn->requests_launched) {
        int target_paths = user_conn->ctx->args->net_cfg.multi_interface_cnt;
        if (target_paths <= 0) {
            target_paths = MAX_PATH_CNT;
        }
        if (target_paths > MAX_PATH_CNT) {
            target_paths = MAX_PATH_CNT;
        }

        if (user_conn->active_path_cnt >= target_paths
            || user_conn->path_wait_rounds_after_handshake >= 2) {
            if (user_conn->active_path_cnt < target_paths) {
                printf("[warn] launch requests with %d/%d active paths after waiting\n",
                    user_conn->active_path_cnt, target_paths);
            }
            if (xqc_mini_cli_launch_requests(user_conn) != XQC_OK) {
                printf("[error] launch requests in path retry callback failed\n");
            }
        } else {
            user_conn->path_wait_rounds_after_handshake++;
        }
    }

    tv.tv_sec = 2;
    tv.tv_usec = 0;
    event_add(user_conn->ev_path_retry, &tv);
}

