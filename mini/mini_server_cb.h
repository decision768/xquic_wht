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

struct xqc_mini_svr_user_conn_s;

typedef struct xqc_mini_svr_path_diag_s {
    int                          valid;
    uint64_t                     path_id;
    uint8_t                      path_state;
    uint8_t                      path_app_status;
    uint64_t                     path_srtt;
    uint64_t                     path_send_bytes;
    uint64_t                     path_recv_bytes;
    uint64_t                     path_pkt_send_count;
    uint64_t                     path_pkt_recv_count;
    uint64_t                     path_recv_effective_bytes;
} xqc_mini_svr_path_diag_t;

typedef struct xqc_mini_svr_get_diag_snapshot_s {
    int                          valid;
    int                          both_active;
    int                          active_path_cnt;
    int                          mp_state;
    int                          enable_mp;
    uint32_t                     send_count;
    uint32_t                     recv_count;
    uint32_t                     lost_count;
    uint32_t                     spurious_loss_count;
    uint64_t                     srtt;
    uint64_t                     min_rtt;
    uint64_t                     inflight_bytes;
    uint64_t                     total_app_bytes;
    uint64_t                     standby_path_app_bytes;
    xqc_mini_svr_path_diag_t     paths[XQC_MAX_PATHS_COUNT];
} xqc_mini_svr_get_diag_snapshot_t;

typedef struct xqc_mini_svr_user_stream_s {
    struct xqc_mini_svr_user_stream_s *pending_next;
    struct xqc_mini_svr_user_conn_s   *user_conn;
    xqc_h3_request_t           *h3_request;
    unsigned int                 pending_read_flags;
    int                          pending_for_serve;
    int                          recv_body_fd;
    FILE                        *send_body_fp;
    int                          send_body_fd;
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
    uint64_t                    file_read_count;
    uint64_t                    file_read_bytes;
    uint64_t                    file_read_max_us;
    uint64_t                    h3_send_count;
    uint64_t                    h3_send_bytes;
    uint64_t                    h3_send_max_us;
    /*
     * Internal processing latency samples. These fields are private to mini_server
     * and do not change the public XQUIC/NetIns ABI.
     */
    uint64_t                    recv_aggregate_sample_count;
    uint64_t                    recv_aggregate_sample_bytes;
    uint64_t                    recv_aggregate_total_ns;
    uint64_t                    recv_aggregate_min_ns;
    uint64_t                    recv_aggregate_max_ns;
    uint64_t                    send_split_sample_count;
    uint64_t                    send_split_sample_bytes;
    uint64_t                    send_split_total_ns;
    uint64_t                    send_split_min_ns;
    uint64_t                    send_split_max_ns;
    int                         get_path_diag_started;
    xqc_mini_svr_get_diag_snapshot_t get_path_diag_start;
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

void xqc_mini_svr_ready_to_create_path_notify(const xqc_cid_t *scid, void *conn_user_data);

int xqc_mini_svr_path_created_notify(xqc_connection_t *conn, const xqc_cid_t *scid,
    uint64_t path_id, void *conn_user_data);

void xqc_mini_svr_path_removed_notify(const xqc_cid_t *scid, uint64_t path_id,
    void *conn_user_data);

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
void xqc_mini_svr_release_connection_state(
    struct xqc_mini_svr_user_conn_s *user_conn);
int xqc_mini_svr_resume_pending_requests(
    struct xqc_mini_svr_user_conn_s *user_conn);

#endif
