#ifndef XQC_MINI_SERVER_CB_H
#define XQC_MINI_SERVER_CB_H

#include <fcntl.h>
#include <xquic/xquic_typedef.h>

#ifndef XQC_SYS_WINDOWS
#include <unistd.h>
#include <sys/wait.h>
#else
#include "../tests/getopt.h"
#pragma comment(lib,"ws2_32.lib")
#pragma comment(lib,"event.lib")
#pragma comment(lib, "Iphlpapi.lib")
#pragma comment(lib, "Bcrypt.lib")
#endif

#include "mini_server.h"
#include <inttypes.h>

#define REQ_BUF_SIZE 4*1024*1024

typedef struct xqc_mini_svr_user_stream_s {
    xqc_h3_request_t           *h3_request;
    int                          recv_body_fd;
    FILE                        *send_body_fp;
    size_t                       expected_content_length;
    int                          fin_received;

    int                          metadata_parsed;
    int                          stream_index;
    int                          stream_count;
    size_t                       stream_offset;
    size_t                       total_file_size;
    int                          file_generation;
    REQUEST_METHOD               method;

    struct timeval start_time;
    struct timeval end_time;

    struct timeval send_start_time;
    struct timeval send_end_time;
    int                         send_started;
    int                         send_logged;

    // uint64_t            send_offset;
    int                         header_sent;
    int                         header_recvd;
    size_t                      send_body_len;
    size_t                      recv_body_len;
    char                       *recv_buf;

    size_t                      response_body_len;
    size_t                      response_body_sent;
    size_t                      response_body_offset;
    size_t                      buffered_len;
    size_t                      buffered_sent;
    unsigned char               send_cache[REQ_BUF_SIZE];
    int                         use_generated_response;
    size_t                      requested_generated_length;
    int                         response_prepared;
    int                         use_file_response;
    int                         response_status;
    char                        *response_content_type;
    char                        response_status_str[4];
    char                        response_content_length[32];
    char                        static_response[256];
    size_t                      static_response_len;
    char                        request_path[PATH_LEN];
    char                        resolved_path[PATH_LEN];
} xqc_mini_svr_user_stream_t;


/* engine callbacks */
// 引擎定时器事件回调，驱动引擎主逻辑
void xqc_mini_svr_engine_cb(int fd, short what, void *arg);

// 设置引擎定时器触发时间
void xqc_mini_svr_set_event_timer(xqc_msec_t wake_after, void *arg);

// 打开日志文件
int xqc_mini_svr_open_log_file(void *arg);

// 写入日志内容
void xqc_mini_svr_write_log_file(xqc_log_level_t lvl, const void *buf, size_t size, void *arg);

// 关闭日志文件
void xqc_mini_svr_close_log_file(void *arg);

// 写入 qlog 日志事件
void xqc_mini_svr_write_qlog_file(qlog_event_importance_t imp, const void *buf, size_t size, void *arg);

// 打开 TLS keylog 文件
int xqc_mini_svr_open_keylog_file(void *arg);

// TLS keylog 回调
void xqc_mini_svr_keylog_cb(const xqc_cid_t *scid, const char *line, void *arg);

// 关闭 TLS keylog 文件
void xqc_mini_svr_close_keylog_file(void *arg);

// 连接接收回调（server_accept）
int xqc_mini_svr_accept(xqc_engine_t *engine, xqc_connection_t *conn, const xqc_cid_t *cid,
    void *eng_user_data);

// 发送 UDP 数据包
ssize_t xqc_mini_svr_write_socket(const unsigned char *buf, size_t size, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, void *arg);

// 根据路径 ID 发送 UDP 数据包
ssize_t xqc_mini_svr_write_socket_ex(uint64_t path_id, const unsigned char *buf, size_t size, 
    const struct sockaddr *peer_addr,socklen_t peer_addrlen, void *arg);

// 连接 CID 更新通知
void xqc_mini_svr_conn_update_cid_notify(xqc_connection_t *conn, const xqc_cid_t *retire_cid,
    const xqc_cid_t *new_cid, void *user_data);

/* h3 callbacks */
// HTTP/3 连接创建回调
int xqc_mini_svr_h3_conn_create_notify(xqc_h3_conn_t *h3_conn, const xqc_cid_t *cid,
    void *conn_user_data);

// HTTP/3 连接关闭回调
int xqc_mini_svr_h3_conn_close_notify(xqc_h3_conn_t *h3_conn, const xqc_cid_t *cid,
    void *conn_user_data);

// HTTP/3 握手完成回调
void xqc_mini_svr_h3_conn_handshake_finished(xqc_h3_conn_t *h3_conn, void *conn_user_data);

// HTTP/3 请求创建回调
int xqc_mini_svr_h3_request_create_notify(xqc_h3_request_t *h3_request, void *strm_user_data);

// HTTP/3 请求关闭回调
int xqc_mini_svr_h3_request_close_notify(xqc_h3_request_t *h3_request, void *strm_user_data);

// HTTP/3 请求读事件回调
int xqc_mini_svr_h3_request_read_notify(xqc_h3_request_t *h3_request, xqc_request_notify_flag_t flag,
    void *strm_user_data);

// HTTP/3 请求写事件回调
int xqc_mini_svr_h3_request_write_notify(xqc_h3_request_t *h3_request, void *strm_user_data);

// 发送响应体数据
int xqc_mini_svr_send_body(xqc_mini_svr_user_stream_t *user_stream);

#endif
