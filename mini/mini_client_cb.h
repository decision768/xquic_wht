#ifndef XQC_MINI_CLIENT_CB_H
#define XQC_MINI_CLIENT_CB_H
#include <fcntl.h>
#include <inttypes.h>
#include "mini_client.h"

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

#define XQC_MAX_BUFF_SIZE 4*1024*1024
// 引擎定时器事件回调，驱动引擎主逻辑
void xqc_mini_cli_engine_cb(int fd, short what, void *arg);
// 打开日志文件
int xqc_mini_cli_open_log_file(void *arg);
// 关闭日志文件
void xqc_mini_cli_close_log_file(void *arg);
// 写入日志内容
void xqc_mini_cli_write_log_file(xqc_log_level_t lvl, const void *buf, size_t size, void *engine_user_data);
// 打开 TLS keylog 文件
int xqc_mini_cli_open_keylog_file(void *arg);
// 关闭 TLS keylog 文件
void xqc_mini_cli_close_keylog_file(void *arg);
// 写入 qlog 日志事件
void xqc_mini_cli_write_qlog_file(qlog_event_importance_t imp, const void *buf, size_t size, void *engine_user_data);
// TLS keylog 回调
void xqc_mini_cli_keylog_cb(const xqc_cid_t *scid, const char *line, void *engine_user_data);
// HTTP/3 连接创建回调
int xqc_mini_cli_h3_conn_create_notify(xqc_h3_conn_t *conn, const xqc_cid_t *cid, void *user_data);
// HTTP/3 连接关闭回调
int xqc_mini_cli_h3_conn_close_notify(xqc_h3_conn_t *conn, const xqc_cid_t *cid, void *user_data);
// HTTP/3 握手完成回调
void xqc_mini_cli_h3_conn_handshake_finished(xqc_h3_conn_t *h3_conn, void *user_data);
// HTTP/3 请求创建回调
int xqc_mini_cli_h3_request_create_notify(xqc_h3_request_t *h3_request, void *h3s_user_data);
// HTTP/3 请求关闭中回调（带错误码）
void xqc_mini_cli_h3_request_closing_notify(xqc_h3_request_t *h3_request, 
    xqc_int_t err, void *h3s_user_data);
// HTTP/3 请求关闭回调
int xqc_mini_cli_h3_request_close_notify(xqc_h3_request_t *h3_request, void *user_data);
// HTTP/3 请求读事件回调
int xqc_mini_cli_h3_request_read_notify(xqc_h3_request_t *h3_request, 
    xqc_request_notify_flag_t flag, void *h3s_user_data);
// HTTP/3 请求写事件回调
int xqc_mini_cli_h3_request_write_notify(xqc_h3_request_t *h3_request, void *h3s_user_data);

// 设置引擎定时器触发时间
void xqc_mini_cli_set_event_timer(xqc_usec_t wake_after, void *user_data);
// 发送 UDP 数据包
ssize_t xqc_mini_cli_write_socket(const unsigned char *buf, size_t size, const struct sockaddr *peer_addr,
    socklen_t peer_addrlen, void *conn_user_data);
// 根据路径 ID 发送 UDP 数据包
ssize_t xqc_mini_cli_write_socket_ex(uint64_t path_id, const unsigned char *buf, size_t size,
    const struct sockaddr *peer_addr, socklen_t peer_addrlen, void *conn_user_data);
// 读取 token 文件数据
int xqc_mini_cli_read_token(unsigned char *token, unsigned token_len);
// 保存 token 到文件
void xqc_mini_cli_save_token(const unsigned char *token, unsigned token_len, void *user_data);
// 保存 session ticket
void xqc_mini_cli_save_session_cb(const char * data, size_t data_len, void *user_data);
// 保存 transport parameters
void xqc_mini_cli_save_tp_cb(const char * data, size_t data_len, void * user_data);

// 连接超时回调
void xqc_mini_cli_timeout_callback(int fd, short what, void *arg);

void xqc_mini_cli_path_retry_callback(int fd, short what, void *arg);

// QUIC 连接创建回调
int xqc_mini_cli_conn_create_notify(xqc_connection_t *conn, const xqc_cid_t *cid, void *user_data, void *conn_proto_data);

#endif