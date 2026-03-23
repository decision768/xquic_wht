# \# mini 客户端与服务器函数文档



本文档基于 `mini_client*.c/.h` 与 `mini_server*.c/.h` 整理，重点覆盖 mini 版本客户端、服务器的主流程函数、核心回调函数，以及最后需要理解的关键结构体。



## \## 1. 客户端函数文档



### \### 1.1 启动与主流程函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `init_args` | 分配客户端参数对象，初始化环境、XQUIC 引擎和引擎上下文，是客户端启动前的总初始化入口。 | `xqc_mini_cli_args_t **args`：输出参数对象；`xqc_mini_cli_ctx_t *ctx`：客户端上下文；`int argc, char *argv[]`：命令行参数。 | `XQC_OK` 表示成功，`XQC_ERROR` 表示失败。 |

| `create_connection` | 创建客户端连接对象 `user_conn`，为后续建立 QUIC/HTTP3 连接做准备。 | `xqc_mini_cli_user_conn_t **user_conn`：输出连接对象；`xqc_mini_cli_ctx_t *ctx`：客户端上下文。 | `XQC_OK` / `XQC_ERROR`。 |

| `start_event` | 启动 libevent 事件循环，正式进入网络事件处理阶段。 | `xqc_mini_cli_ctx_t *ctx`：客户端上下文。 | 无返回值。 |

| `close_all` | 统一销毁引擎、连接相关事件和上下文资源。 | `xqc_mini_cli_ctx_t *ctx`：客户端上下文；`xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_main_process` | 客户端主处理入口，负责初始化 XQUIC 连接、配置超时和路径重试事件，并触发请求发送准备。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象；`xqc_mini_cli_ctx_t *ctx`：客户端上下文。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_launch_requests` | 在连接可用后统一发起首批请求，必要时会先等待多路径准备完成。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_try_schedule_requests` | 按并发窗口继续补发后续请求流，直到达到 `max_concurrent_requests` 或全部请求发完。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_wait_for_target_paths` | 在正式发流前等待目标路径数建立完成，主要用于多路径场景。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象；`int wait_ms`：最大等待时间。 | 无返回值。 |



### \### 1.2 初始化与配置函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `xqc_mini_cli_init_args` | 填充客户端默认参数，包括服务器地址、端口、调度器、拥塞控制、日志路径、上传下载文件路径等。 | `xqc_mini_cli_args_t *args`：客户端参数对象。 | 无返回值。 |

| `xqc_mini_cli_init_env` | 初始化客户端整体运行环境，内部会调用默认参数初始化、配置文件加载和上下文初始化。 | `xqc_mini_cli_ctx_t *ctx`：客户端上下文；`xqc_mini_cli_args_t *args`：客户端参数。 | `XQC_OK` / 错误码。 |

| `xqc_mini_cli_init_ctx` | 初始化运行上下文，创建事件循环、初始化日志和 keylog 输出。 | `xqc_mini_cli_ctx_t *ctx`：客户端上下文；`xqc_mini_cli_args_t *args`：客户端参数。 | `0` 或 `XQC_ERROR`。 |

| `xqc_mini_cli_init_engine_ssl_config` | 组装 XQUIC 引擎级 SSL 配置，设置 cipher suites 和 TLS groups。 | `xqc_engine_ssl_config_t *ssl_cfg`：输出 SSL 配置；`xqc_mini_cli_args_t *args`：客户端参数。 | 无返回值。 |

| `xqc_mini_cli_init_callback` | 初始化引擎回调和传输层回调，绑定定时器、写 socket、token 保存、多路径回调等。 | `xqc_engine_callback_t *cb`：输出引擎回调；`xqc_transport_callbacks_t *tcb`：输出传输回调；`xqc_mini_cli_args_t *args`：客户端参数。 | 无返回值。 |

| `xqc_mini_cli_init_xquic_engine` | 创建客户端 XQUIC 引擎，并注册引擎事件。 | `xqc_mini_cli_ctx_t *ctx`：客户端上下文；`xqc_mini_cli_args_t *args`：客户端参数。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_init_alpn_ctx` | 初始化 HTTP/3 的 ALPN 上下文和对应回调。 | `xqc_mini_cli_ctx_t *ctx`：客户端上下文。 | `XQC_OK` / 错误码。 |

| `xqc_mini_cli_init_engine_ctx` | 初始化引擎级上下文，目前主要完成 ALPN/HTTP3 上下文注册。 | `xqc_mini_cli_ctx_t *ctx`：客户端上下文。 | `XQC_OK` / 错误码。 |

| `xqc_mini_cli_init_conn_settings` | 初始化连接级参数，如拥塞控制、多路径调度、ACK 频率、接收窗口等。 | `xqc_conn_settings_t *settings`：输出连接设置；`xqc_mini_cli_args_t *args`：客户端参数。 | 无返回值。 |

| `xqc_mini_cli_get_sched_cb` | 根据字符串配置选择多路径调度器回调。 | `xqc_mini_cli_args_t *args`：客户端参数。 | 对应的 `xqc_scheduler_callback_t`。 |

| `xqc_mini_cli_get_cc_cb` | 根据配置选择拥塞控制算法回调。 | `xqc_mini_cli_args_t *args`：客户端参数。 | 对应的 `xqc_cong_ctrl_callback_t`。 |

| `xqc_mini_cli_init_0rtt` | 从本地文件读取 session ticket、transport parameters、token，用于 0-RTT/会话恢复。 | `xqc_mini_cli_args_t *args`：客户端参数。 | 无返回值。 |

| `xqc_mini_cli_init_conn_ssl_config` | 初始化连接级 SSL 配置，将 0-RTT 相关数据挂入连接配置。 | `xqc_conn_ssl_config_t *conn_ssl_config`：输出连接 SSL 配置；`xqc_mini_cli_args_t *args`：客户端参数。 | 无返回值。 |

| `xqc_mini_cli_parse_cmd_args` | 解析命令行参数并写入客户端配置。当前代码中保留，但主流程默认未启用。 | `xqc_mini_cli_args_t *args`：客户端参数；`int argc, char *argv[]`：命令行参数。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_load_config_file` | 从 `client_config.txt` 读取配置项并覆盖默认值，例如地址、接口、buffer、大文件分片大小等。 | `xqc_mini_cli_args_t *args`：客户端参数；`const char *path`：配置文件路径。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_trim_space` | 去掉配置文件行首尾空白字符。 | `char *text`：待处理字符串。 | 去空白后的字符串指针。 |

| `xqc_mini_cli_apply_interface_list` | 解析多网卡接口列表并写入 `multi_interface` 配置。 | `xqc_mini_cli_args_t *args`：客户端参数；`const char *value`：接口列表字符串。 | 无返回值。 |

| `xqc_mini_cli_convert_text_to_sockaddr` | 把文本 IP 和端口转换成 `sockaddr` 结构。 | `int type`：地址族；`const char *addr_text`：IP 文本；`unsigned int port`：端口；`struct sockaddr **saddr`：输出地址；`socklen_t *saddr_len`：输出长度。 | 无返回值。 |



### \### 1.3 连接、路径与 socket 管理函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `xqc_mini_cli_user_conn_create` | 创建客户端连接对象，初始化路径、接收队列、通知事件等资源。 | `xqc_mini_cli_ctx_t *ctx`：客户端上下文。 | 成功返回连接对象指针，失败返回 `NULL`。 |

| `xqc_mini_cli_init_xquic_connection` | 创建真实的 QUIC 连接，配置连接参数、SSL 参数并发起连接建立。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_try_rebuild_paths` | 尝试补齐目标路径数，在多路径尚未完全建立时重复创建新 path。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_create_new_path` | 为已有连接申请一个新的 multipath path。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | `XQC_OK` / 错误码。 |

| `xqc_mini_cli_get_target_path_count` | 计算当前连接期望建立的路径总数。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | 目标路径数。 |

| `xqc_mini_cli_find_inactive_path` | 在路径数组中查找尚未激活的 path 槽位。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | 可用 path 指针，失败返回 `NULL`。 |

| `xqc_mini_cli_prepare_user_path` | 初始化单个 path 的本地/对端地址、接口信息和状态。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象；`xqc_mini_cli_user_path_t *path`：待初始化路径。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_init_socket` | 为某条 path 创建 UDP socket，设置非阻塞和 buffer。 | `xqc_mini_cli_user_path_t *user_path`：路径对象。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_socket_write_handler` | 处理 socket 可写事件，目前主要用于占位和调试。 | `xqc_mini_cli_user_path_t *user_path`：路径对象；`int fd`：socket。 | 无返回值。 |

| `xqc_mini_cli_socket_read_handler` | 处理 socket 收包，或把数据交给接收线程/接收队列继续处理。 | `xqc_mini_cli_user_path_t *user_path`：路径对象；`int fd`：socket。 | 无返回值。 |

| `xqc_mini_cli_socket_event_callback` | libevent 的 socket 事件分发回调，按读写类型转发给对应 handler。 | `int fd`：socket；`short what`：事件类型；`void *arg`：路径对象。 | 无返回值。 |

| `xqc_mini_cli_bind_to_interface` | 把 socket 绑定到指定网卡接口，用于多网卡多路径实验。 | `int fd`：socket；`const char *interface_name`：接口名；`int family`：地址族。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_set_local_addr` | 根据网卡接口或回退地址，为路径设置本地源地址。 | `xqc_mini_cli_user_path_t *path`：路径对象。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_fill_wildcard_local_addr` | 本地地址获取失败时，回退成通配地址。 | `struct sockaddr *local_addr`：地址对象；`socklen_t *addrlen`：长度。 | 无返回值。 |

| `xqc_mini_cli_format_addr_port` | 把地址结构格式化成 `ip:port` 字符串。 | `const struct sockaddr *addr`：地址；`socklen_t addrlen`：长度；`char *buf`：输出缓冲区；`size_t buflen`：缓冲区大小。 | 无返回值。 |

| `xqc_mini_cli_dump_path_bindings` | 打印当前各 path 的本地地址、接口与 path id 绑定关系。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_conn_ready_to_create_path` | 连接通知回调，表示可以申请新 path 时触发。 | `const xqc_cid_t *cid`：连接 ID；`void *conn_user_data`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_path_removed` | 路径被移除后的状态清理回调。 | `const xqc_cid_t *cid`：连接 ID；`uint64_t path_id`：路径 ID；`void *conn_user_data`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_free_user_conn` | 释放连接对象、路径 socket、线程、队列和事件资源。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_on_connection_finish` | 连接结束时统一删除事件、关闭 socket，做收尾清理。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | 无返回值。 |



### \### 1.4 请求与收发数据函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `xqc_mini_cli_format_h3_req` | 组装 HTTP/3 请求头，包含方法、路径、长度以及多流分片元数据。 | `xqc_http_header_t *headers`：输出头数组；`xqc_mini_cli_req_config_t *req_cfg`：请求配置；`size_t body_len`：body 长度；`xqc_mini_cli_user_stream_t *user_stream`：流对象。 | 成功返回 header 个数，失败返回 `XQC_ERROR`。 |

| `xqc_mini_cli_send_h3_req` | 创建 HTTP/3 request，发送请求头，并在 POST 时触发 body 发送。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象；`xqc_mini_cli_user_stream_t *user_stream`：流对象；`int stream_index`：流序号。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_request_send` | 按分片和发送缓冲循环发送请求 body，支持 EAGAIN 续发。 | `xqc_h3_request_t *h3_request`：请求对象；`xqc_mini_cli_user_stream_t *user_stream`：流对象。 | `XQC_OK` 或负值错误码。 |

| `xqc_mini_cli_compute_stream_segment` | 按流索引计算该流对应的文件偏移和长度。 | `const xqc_mini_cli_user_conn_t *user_conn`：连接对象；`int stream_index`：流索引；`size_t *offset`：输出偏移；`size_t *length`：输出长度。 | 无返回值。 |

| `xqc_mini_cli_get_stream_range_size` | 获取每个流默认或配置指定的分片大小。 | `const xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | 分片大小。 |

| `xqc_mini_cli_notify_upload_finished` | 统计单流上传完成，并在全部流发送结束时输出总吞吐。 | `xqc_mini_cli_user_stream_t *user_stream`：流对象。 | 无返回值。 |



### \### 1.5 接收线程与队列函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `xqc_mini_cli_rx_thread_main` | 单路径接收线程主函数，负责从 socket 拉包并推入连接接收队列。 | `void *arg`：通常为 `xqc_mini_cli_user_path_t *`。 | 线程返回值指针。 |

| `xqc_mini_cli_start_path_rx_thread` | 启动某条 path 的接收线程。 | `xqc_mini_cli_user_path_t *path`：路径对象。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_stop_path_rx_thread` | 停止并回收 path 接收线程。 | `xqc_mini_cli_user_path_t *path`：路径对象。 | 无返回值。 |

| `xqc_mini_cli_drain_rx_queue` | 从连接级接收队列中取出数据包并交给 XQUIC 处理。 | `xqc_mini_cli_user_conn_t *user_conn`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_rx_queue_event_callback` | 接收队列事件回调，用于驱动 `drain_rx_queue`。 | `int fd`：通知 fd；`short what`：事件类型；`void *arg`：连接对象。 | 无返回值。 |



### \### 1.6 客户端回调函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `xqc_mini_cli_engine_cb` | 引擎定时器事件回调，调用 `xqc_engine_main_logic` 推进协议栈。 | `int fd`，`short what`，`void *arg`：其中 `arg` 为客户端上下文。 | 无返回值。 |

| `xqc_mini_cli_set_event_timer` | 根据 XQUIC 给出的唤醒时间，重新设置 libevent 定时器。 | `xqc_usec_t wake_after`：多久后唤醒；`void *user_data`：客户端上下文。 | 无返回值。 |

| `xqc_mini_cli_open_log_file` | 打开普通日志文件。 | `void *arg`：客户端上下文。 | 文件描述符；失败返回负值。 |

| `xqc_mini_cli_close_log_file` | 关闭普通日志文件。 | `void *arg`：客户端上下文。 | 无返回值。 |

| `xqc_mini_cli_write_log_file` | 写入普通日志或 zlog。 | `xqc_log_level_t lvl`：日志级别；`const void *buf`：日志内容；`size_t size`：长度；`void *engine_user_data`：客户端上下文。 | 无返回值。 |

| `xqc_mini_cli_open_keylog_file` | 打开 TLS keylog 文件。 | `void *arg`：客户端上下文。 | 文件描述符；失败返回负值。 |

| `xqc_mini_cli_close_keylog_file` | 关闭 TLS keylog 文件。 | `void *arg`：客户端上下文。 | 无返回值。 |

| `xqc_mini_cli_write_qlog_file` | 写入 qlog 事件。 | `qlog_event_importance_t imp`：事件重要级；`const void *buf`：内容；`size_t size`：长度；`void *engine_user_data`：客户端上下文。 | 无返回值。 |

| `xqc_mini_cli_keylog_cb` | TLS keylog 回调，把密钥材料写入 keylog 文件。 | `const xqc_cid_t *scid`：连接 ID；`const char *line`：keylog 内容；`void *engine_user_data`：上下文。 | 无返回值。 |

| `xqc_mini_cli_conn_create_notify` | QUIC 连接创建通知回调，绑定应用层用户数据。 | `xqc_connection_t *conn`：连接；`const xqc_cid_t *cid`：连接 ID；`void *user_data`：连接对象；`void *conn_proto_data`：协议数据。 | `XQC_OK`。 |

| `xqc_mini_cli_h3_conn_create_notify` | HTTP/3 连接创建回调，保存 h3 连接指针和 CID。 | `xqc_h3_conn_t *conn`：H3 连接；`const xqc_cid_t *cid`：连接 ID；`void *user_data`：连接对象。 | `XQC_OK`。 |

| `xqc_mini_cli_h3_conn_close_notify` | HTTP/3 连接关闭回调，结束事件循环。 | `xqc_h3_conn_t *conn`，`const xqc_cid_t *cid`，`void *user_data`。 | `XQC_OK`。 |

| `xqc_mini_cli_h3_conn_handshake_finished` | 握手完成回调，尝试补路径并发起请求。 | `xqc_h3_conn_t *h3_conn`：H3 连接；`void *user_data`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_h3_request_create_notify` | HTTP/3 请求创建通知，当前主要作为保留扩展点。 | `xqc_h3_request_t *h3_request`：请求；`void *h3s_user_data`：流对象。 | `0`。 |

| `xqc_mini_cli_h3_request_read_notify` | HTTP/3 请求读事件回调，处理响应头和响应体，支持多流下载拼接。 | `xqc_h3_request_t *h3_request`：请求；`xqc_request_notify_flag_t flag`：读事件类型；`void *h3s_user_data`：流对象。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_cli_h3_request_write_notify` | HTTP/3 请求写事件回调，在可继续发送时补发 body。 | `xqc_h3_request_t *h3_request`：请求；`void *h3s_user_data`：流对象。 | `xqc_mini_cli_request_send` 的返回值。 |

| `xqc_mini_cli_h3_request_close_notify` | HTTP/3 请求关闭回调，释放流资源，更新并发槽位，必要时关闭连接。 | `xqc_h3_request_t *h3_request`：请求；`void *user_data`：流对象。 | `0`。 |

| `xqc_mini_cli_write_socket` | 默认 socket 写回调，内部调用扩展版本。 | `const unsigned char *buf`：发送数据；`size_t size`：长度；`const struct sockaddr *peer_addr`：对端地址；`socklen_t peer_addrlen`：地址长度；`void *conn_user_data`：连接对象。 | 实际发送字节数或错误码。 |

| `xqc_mini_cli_write_socket_ex` | 按 path id 选择路径发送 UDP 包，支持路径故障转移和关闭失效 path。 | `uint64_t path_id`：目标 path；其余参数同上。 | 实际发送字节数、`XQC_SOCKET_EAGAIN` 或负值。 |

| `xqc_mini_cli_read_token` | 从本地文件读取 token。 | `unsigned char *token`：输出缓冲区；`unsigned token_len`：最大长度。 | 读取字节数；失败返回负值。 |

| `xqc_mini_cli_save_token` | 保存 token 到本地文件。 | `const unsigned char *token`：token 数据；`unsigned token_len`：长度；`void *user_data`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_save_session_cb` | 保存 session ticket 到本地文件。 | `const char *data`：ticket 数据；`size_t data_len`：长度；`void *user_data`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_save_tp_cb` | 保存 transport parameters 到本地文件。 | `const char *data`：数据；`size_t data_len`：长度；`void *user_data`：连接对象。 | 无返回值。 |

| `xqc_mini_cli_timeout_callback` | 连接超时定时器回调，长时间无 socket 活动时主动关闭连接。 | `int fd`，`short what`，`void *arg`：其中 `arg` 为连接对象。 | 无返回值。 |

| `xqc_mini_cli_path_retry_callback` | 周期性尝试重建路径，并在合适时发送 ping 或发起请求。 | `int fd`，`short what`，`void *arg`：连接对象。 | 无返回值。 |



## \## 2. 服务器函数文档



### \### 2.1 启动与主流程函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `init_args` | 分配服务端参数对象，初始化环境、XQUIC 引擎和引擎上下文。 | `xqc_mini_svr_args_t **args`：输出参数；`xqc_mini_svr_ctx_t *ctx`：服务端上下文；`int argc, char *argv[]`：命令行参数。 | `XQC_OK` / `XQC_ERROR`。 |

| `connection_socket` | 创建服务端用户连接对象并初始化监听 socket。 | `xqc_mini_svr_ctx_t *ctx`：服务端上下文。 | `xqc_mini_svr_user_conn_t *`，失败返回 `NULL`。 |

| `start_event` | 启动服务端 libevent 事件循环。 | `xqc_mini_svr_ctx_t *ctx`：服务端上下文。 | 无返回值。 |

| `close_all` | 服务端统一清理入口，销毁引擎和上下文资源。 | `xqc_mini_svr_ctx_t *ctx`：服务端上下文；`xqc_mini_svr_user_conn_t *user_conn`：用户连接。 | 无返回值。 |

| `main` | 服务端程序入口。 | `int argc, char *argv[]`：命令行参数。 | `0`。 |



### \### 2.2 初始化与配置函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `xqc_mini_svr_init_args` | 初始化服务端默认配置，包括监听地址、证书、日志、多路径和数据目录。 | `xqc_mini_svr_args_t *args`：服务端参数。 | 无返回值。 |

| `xqc_mini_svr_init_env` | 初始化服务端整体环境，内部包含默认配置、配置文件加载和上下文初始化。 | `xqc_mini_svr_ctx_t *ctx`：服务端上下文；`xqc_mini_svr_args_t *args`：服务端参数。 | `XQC_OK` / 错误码。 |

| `xqc_mini_svr_init_ctx` | 初始化服务端运行上下文，包括 event base、日志系统等。 | `xqc_mini_svr_ctx_t *ctx`：上下文；`xqc_mini_svr_args_t *args`：参数。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_svr_init_ssl_config` | 组装服务端 SSL 配置，设置证书、私钥、cipher、group 和 session ticket key。 | `xqc_engine_ssl_config_t *ssl_cfg`：输出 SSL 配置；`xqc_mini_svr_args_t *args`：服务端参数。 | 无返回值。 |

| `xqc_mini_svr_init_callback` | 初始化服务端引擎和传输层回调。 | `xqc_engine_callback_t *cb`：输出引擎回调；`xqc_transport_callbacks_t *tcb`：输出传输回调；`xqc_mini_svr_args_t *args`：服务端参数。 | 无返回值。 |

| `xqc_mini_svr_init_xquic_engine` | 创建服务端 XQUIC 引擎，并注册引擎事件。 | `xqc_mini_svr_ctx_t *ctx`：上下文；`xqc_mini_svr_args_t *args`：参数。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_svr_init_conn_settings` | 设置服务端连接级参数，如拥塞控制、多路径、ACK、接收窗口等。 | `xqc_engine_t *engine`：XQUIC 引擎；`xqc_mini_svr_args_t *args`：服务端参数。 | 无返回值。 |

| `xqc_mini_svr_init_alpn_ctx` | 初始化 HTTP/3 ALPN 上下文和 H3 回调。 | `xqc_engine_t *engine`：XQUIC 引擎。 | `XQC_OK` / 错误码。 |

| `xqc_mini_svr_init_engine_ctx` | 初始化引擎上下文，主要完成连接设置和 ALPN 注册。 | `xqc_mini_svr_ctx_t *ctx`：上下文；`xqc_mini_svr_args_t *args`：参数。 | `XQC_OK` / 错误码。 |

| `xqc_mini_svr_parse_cmd_args` | 解析服务端命令行参数，可设置监听地址、端口、默认文件和数据目录。 | `xqc_mini_svr_args_t *args`：参数；`int argc, char *argv[]`：命令行参数。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_svr_load_config_file` | 从 `server_config.txt` 读取配置并覆盖默认值。 | `xqc_mini_svr_args_t *args`：参数；`const char *path`：配置文件路径。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_svr_trim_space` | 去除配置文本首尾空白。 | `char *text`：待处理文本。 | 处理后的字符串指针。 |

| `xqc_mini_svr_get_cc_cb` | 根据配置选择拥塞控制回调。 | `xqc_mini_svr_args_t *args`：服务端参数。 | `xqc_cong_ctrl_callback_t`。 |

| `xqc_mini_svr_get_sched_cb` | 根据配置选择多路径调度器回调。 | `xqc_mini_svr_args_t *args`：服务端参数。 | `xqc_scheduler_callback_t`。 |



### \### 2.3 socket、连接与上下文管理函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `xqc_mini_svr_init_socket` | 创建 UDP socket，设置非阻塞、复用和收发 buffer，并完成 bind。 | `int family`：地址族；`struct sockaddr *local_addr`：本地地址；`socklen_t local_addrlen`：长度；`const xqc_mini_svr_net_config_t *cfg`：网络配置。 | 成功返回 fd，失败返回负值。 |

| `xqc_mini_svr_create_socket` | 依据配置生成监听地址并创建服务端 socket。 | `xqc_mini_svr_user_conn_t *user_conn`：用户连接；`xqc_mini_svr_net_config_t *cfg`：网络配置。 | `0` / `-1`。 |

| `xqc_mini_svr_create_user_conn` | 创建服务端用户连接对象，并将监听 socket 绑定到 libevent。 | `xqc_mini_svr_ctx_t *ctx`：服务端上下文。 | 成功返回对象指针，失败返回 `NULL`。 |

| `xqc_mini_svr_socket_write_handler` | 处理 socket 可写事件，当前主要用于调试信息。 | `xqc_mini_svr_user_conn_t *user_conn`：用户连接；`int fd`：socket。 | 无返回值。 |

| `xqc_mini_svr_socket_read_handler` | 处理 UDP 收包并把数据投递给 XQUIC 引擎。 | `xqc_mini_svr_user_conn_t *user_conn`：用户连接；`int fd`：socket。 | 无返回值。 |

| `xqc_mini_svr_socket_event_callback` | libevent socket 事件分发入口。 | `int fd`，`short what`，`void *arg`：其中 `arg` 为用户连接。 | 无返回值。 |

| `xqc_mini_svr_free_ctx` | 释放服务端上下文与参数资源。 | `xqc_mini_svr_ctx_t *ctx`：服务端上下文。 | 无返回值。 |

| `xqc_mini_svr_free_user_conn` | 释放服务端连接对象和地址资源。 | `xqc_mini_svr_user_conn_t *user_conn`：用户连接。 | 无返回值。 |

| `xqc_mini_cli_on_connection_finish` | 连接结束时删除超时事件和 socket 事件。该函数名虽然带 `cli`，但实际在服务端文件中承担服务端收尾职责。 | `xqc_mini_svr_user_conn_t *user_conn`：用户连接。 | 无返回值。 |



### \### 2.4 服务端回调与请求处理函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `xqc_mini_svr_engine_cb` | 引擎定时器事件回调，推动 XQUIC 主循环。 | `int fd`，`short what`，`void *arg`：其中 `arg` 为服务端上下文。 | 无返回值。 |

| `xqc_mini_svr_set_event_timer` | 根据引擎要求设置服务端定时器。 | `xqc_msec_t wake_after`：多久后唤醒；`void *arg`：服务端上下文。 | 无返回值。 |

| `xqc_mini_svr_open_log_file` | 打开普通日志文件。 | `void *arg`：服务端上下文。 | 文件描述符；失败返回负值。 |

| `xqc_mini_svr_write_log_file` | 写入普通日志或 zlog。 | `xqc_log_level_t lvl`：日志级别；`const void *buf`：日志内容；`size_t size`：长度；`void *arg`：上下文。 | 无返回值。 |

| `xqc_mini_svr_close_log_file` | 关闭普通日志文件。 | `void *arg`：服务端上下文。 | 无返回值。 |

| `xqc_mini_svr_write_qlog_file` | 写入 qlog 日志。 | `qlog_event_importance_t imp`：事件等级；`const void *buf`：内容；`size_t size`：长度；`void *arg`：上下文。 | 无返回值。 |

| `xqc_mini_svr_open_keylog_file` | 打开 keylog 文件。 | `void *arg`：服务端上下文。 | 文件描述符；失败返回负值。 |

| `xqc_mini_svr_keylog_cb` | TLS keylog 回调。 | `const xqc_cid_t *scid`：连接 ID；`const char *line`：keylog 内容；`void *arg`：上下文。 | 无返回值。 |

| `xqc_mini_svr_close_keylog_file` | 关闭 keylog 文件。 | `void *arg`：服务端上下文。 | 无返回值。 |

| `xqc_mini_svr_accept` | 服务端接受新 QUIC 连接时的回调，绑定连接级用户数据。 | `xqc_engine_t *engine`：引擎；`xqc_connection_t *conn`：连接；`const xqc_cid_t *cid`：连接 ID；`void *eng_user_data`：上下文。 | `XQC_OK` / 错误码。 |

| `xqc_mini_svr_write_socket` | 默认 UDP 发送回调。 | `const unsigned char *buf`：数据；`size_t size`：长度；`const struct sockaddr *peer_addr`：对端地址；`socklen_t peer_addrlen`：长度；`void *arg`：连接对象。 | 实际发送字节数或错误码。 |

| `xqc_mini_svr_write_socket_ex` | 带 path id 的 UDP 发送回调，用于 multipath 发包。 | `uint64_t path_id`：路径 ID；其余参数同上。 | 实际发送字节数或错误码。 |

| `xqc_mini_svr_conn_update_cid_notify` | 连接 CID 更新回调。 | `xqc_connection_t *conn`：连接；`const xqc_cid_t *retire_cid`：被废弃 CID；`const xqc_cid_t *new_cid`：新 CID；`void *user_data`：连接对象。 | 无返回值。 |

| `xqc_mini_svr_h3_conn_create_notify` | HTTP/3 连接创建回调，重置全局上传文件状态统计。 | `xqc_h3_conn_t *h3_conn`：H3 连接；`const xqc_cid_t *cid`：连接 ID；`void *conn_user_data`：连接对象。 | `0`。 |

| `xqc_mini_svr_h3_conn_close_notify` | HTTP/3 连接关闭回调。 | `xqc_h3_conn_t *h3_conn`，`const xqc_cid_t *cid`，`void *conn_user_data`。 | `0`。 |

| `xqc_mini_svr_h3_conn_handshake_finished` | H3 握手完成通知，通常用于打印状态或做连接级确认。 | `xqc_h3_conn_t *h3_conn`：H3 连接；`void *conn_user_data`：连接对象。 | 无返回值。 |

| `xqc_mini_svr_h3_request_create_notify` | HTTP/3 请求创建回调，为每个请求分配服务端流对象并绑定到 request。 | `xqc_h3_request_t *h3_request`：请求；`void *strm_user_data`：预留数据。 | `0` / 错误码。 |

| `xqc_mini_svr_h3_request_close_notify` | HTTP/3 请求关闭回调，释放流对象及文件句柄。 | `xqc_h3_request_t *h3_request`：请求；`void *strm_user_data`：流对象。 | `0`。 |

| `xqc_mini_svr_h3_request_read_notify` | 服务端核心读回调：解析请求头、处理 GET/POST、多流分片、写入文件并触发响应。 | `xqc_h3_request_t *h3_request`：请求；`xqc_request_notify_flag_t flag`：读事件类型；`void *strm_user_data`：流对象。 | `0` / `XQC_ERROR`。 |

| `xqc_mini_svr_send_response` | 组织 HTTP/3 响应头并调用 `xqc_mini_svr_send_body` 发送响应体。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象。 | `int`，通常为发送函数返回值。 |

| `xqc_mini_svr_send_body` | 实际发送响应体，支持静态文本、文件读取、生成数据三种模式。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象。 | `XQC_OK` / 错误码。 |

| `xqc_mini_svr_h3_request_write_notify` | 响应体可继续发送时的写回调。 | `xqc_h3_request_t *h3_request`：请求；`void *strm_user_data`：流对象。 | `xqc_mini_svr_send_body` 的返回值。 |



### \### 2.5 服务端内部辅助函数



| 函数 | 作用介绍 | 参数 | 返回值 |

| --- | --- | --- | --- |

| `xqc_mini_svr_get_stream_range_size` | 获取每流分片大小。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象。 | 分片大小。 |

| `xqc_mini_svr_log_response_send_stats` | 在响应发送完成后打印发送耗时和吞吐。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象。 | 无返回值。 |

| `xqc_mini_svr_now_us` | 获取当前微秒级时间。 | 无。 | 当前时间戳。 |

| `xqc_mini_svr_log_mp_active_state` | 打印 multipath 路径激活状态，并在特定场景下发送 ping。 | `xqc_mini_svr_user_conn_t *user_conn`：连接对象；`const char *scene`：场景标签。 | 无返回值。 |

| `xqc_mini_svr_time_cmp` | 比较两个 `timeval` 先后。 | `const struct timeval *a`，`const struct timeval *b`。 | `<0`、`0`、`>0`。 |

| `xqc_mini_svr_duration_ms` | 计算两个时间点之间的毫秒差。 | `const struct timeval *start`，`const struct timeval *end`。 | 时长（毫秒）。 |

| `xqc_mini_svr_get_conn_from_request` | 从 H3 request 反查所属用户连接。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象。 | 连接对象指针。 |

| `xqc_mini_svr_resolve_file_path` | 把 GET 请求路径解析成实际文件路径。 | `xqc_mini_svr_ctx_t *ctx`：上下文；`const char *request_path`：请求路径；`char *resolved`：输出路径；`size_t resolved_len`：缓冲区长度。 | 无返回值。 |

| `xqc_mini_svr_prepare_text_response` | 生成文本类响应体和响应状态。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象；`int status`：HTTP 状态码；`const char *fmt, ...`：格式化文本。 | 无返回值。 |

| `xqc_mini_svr_compute_stream_segment` | 根据流序号计算该流负责的文件偏移和长度。 | `int stream_count`：总流数；`int stream_index`：当前流号；`size_t total_size`：总长度；`size_t stream_range_size`：分片大小；`size_t *offset`：输出偏移；`size_t *length`：输出长度。 | 无返回值。 |

| `xqc_mini_svr_prepare_file_response` | 为 GET 请求准备文件响应，打开文件并设置响应长度、偏移和响应头语义。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_svr_fill_generated_chunk` | 生成模拟下载数据块。 | `unsigned char *buf`：输出缓冲区；`size_t len`：长度；`size_t offset`：全局偏移。 | 无返回值。 |

| `xqc_mini_svr_resolve_upload_path` | 解析 POST 上传文件最终落盘路径。 | `xqc_mini_svr_ctx_t *ctx`：上下文；`char *resolved`：输出路径；`size_t resolved_len`：缓冲区长度。 | 无返回值。 |

| `xqc_mini_svr_get_target_file_size` | 获取 GET 目标文件的真实长度。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象；`size_t *file_size`：输出长度。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_svr_reset_file_state` | 重置全局上传拼接状态，初始化目标文件。 | `int stream_count`：总流数；`size_t total_size`：总文件长度；`const char *output_path`：输出文件路径。 | 无返回值。 |

| `xqc_mini_svr_prepare_output_file` | 确保多流上传的目标文件已创建并可随机写入。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象。 | `XQC_OK` / `XQC_ERROR`。 |

| `xqc_mini_svr_update_upload_progress` | 更新多流上传全局进度。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象。 | 无返回值。 |

| `xqc_mini_svr_mark_stream_complete` | 标记单条上传流完成，并在全部流完成后打印整体统计。 | `xqc_mini_svr_user_stream_t *user_stream`：流对象。 | 无返回值。 |



## \## 3. 重要结构体说明



### \### 3.1 客户端相关结构体



| 结构体 | 说明 |

| --- | --- |

| `xqc_mini_cli_net_config_t` | 客户端网络配置。包含连接超时、目标服务器地址端口、多网卡接口列表、内核收发 buffer、用户层收发 buffer、单流分片大小等。 |

| `xqc_mini_cli_quic_config_t` | 客户端 QUIC/TLS 配置。包含 cipher、groups、0-RTT ticket/token/TP、多路径开关、调度器类型、拥塞控制类型等。 |

| `xqc_mini_cli_env_config_t` | 客户端运行环境配置。包含日志输出、zlog 配置、证书、keylog、上传文件路径、下载保存路径等。 |

| `xqc_mini_cli_req_config_t` | 单次请求的 HTTP/3 语义配置。包含方法、scheme、host、path、原始 URL。 |

| `xqc_mini_cli_args_t` | 客户端总配置对象，是网络、QUIC、环境、请求配置的聚合体，同时保存请求流数量和发送数据长度。 |

| `xqc_mini_cli_ctx_t` | 客户端全局运行上下文。包含 `event_base`、XQUIC 引擎、引擎事件、日志 fd、keylog fd、zlog category 等。 |

| `xqc_mini_cli_user_path_t` | 客户端单条 path 的状态对象。包含 socket fd、本地/对端地址、path id、接口名、事件、失败计数、接收线程等。 |

| `xqc_mini_cli_rx_packet_t` | 客户端接收队列中的数据包节点，用于接收线程和主线程之间传递 UDP 包。 |

| `xqc_mini_cli_user_conn_t` | 客户端连接级核心结构体。保存 CID、H3 连接、路径数组、活跃路径数、并发请求窗口、上传/下载统计、事件对象、接收队列和多路径调度状态。 |

| `xqc_mini_cli_user_stream_t` | 客户端单个 HTTP/3 请求流对象。保存流序号、发送文件句柄、接收文件句柄、发送缓冲区、分片偏移、已发/已收字节数、header 文本缓存、响应状态等。 |



### \### 3.2 服务端相关结构体



| 结构体 | 说明 |

| --- | --- |

| `xqc_mini_svr_net_config_t` | 服务端网络配置。包含监听 IP/端口、连接超时、内核收发 buffer、用户层 buffer、单流分片大小等。 |

| `xqc_mini_svr_quic_config_t` | 服务端 QUIC 配置。包含 cipher、groups、多路径开关、调度器、拥塞控制和 session ticket key。 |

| `xqc_mini_svr_env_config_t` | 服务端环境配置。包含日志、证书、keylog、数据目录、默认发送文件和上传落盘路径等。 |

| `xqc_mini_svr_args_t` | 服务端总配置对象，聚合网络、QUIC、环境配置。 |

| `xqc_mini_svr_ctx_t` | 服务端全局上下文。包含 `event_base`、当前 fd、XQUIC 引擎、引擎事件、日志 fd 和 zlog category。 |

| `xqc_mini_svr_user_conn_t` | 服务端连接级对象。包含 socket fd、本地/对端地址、事件、CID、上下文以及 multipath 状态日志时间戳。 |

| `xqc_mini_svr_user_stream_t` | 服务端单个 HTTP/3 请求流对象。既保存接收 POST 上传用的文件状态，也保存 GET 响应发送状态，包括请求元数据、多流偏移、总大小、响应缓存、响应路径、统计时间等。 |

| `xqc_mini_svr_file_state_t` | 服务端内部全局上传拼接状态。用于跟踪多流上传的总大小、已收大小、完成流数量、各流进度及目标输出文件。 |

