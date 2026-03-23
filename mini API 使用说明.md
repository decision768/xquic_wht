# mini API 使用说明

## 概述

mini 模块是一个基于 xquic 的精简 HTTP/3 客户端/服务端示例，支持：

- QUIC / HTTP3 基本收发流程
- 多流并发传输
- 多路径传输与调度器切换
- GET 下载与 POST 上传
- 基于配置文件的快速启动

对外暴露的接口主要是“初始化 -> 建连/建监听 -> 启动事件循环 -> 回收资源”这一套流程型 API。

## 目录

- [核心数据结构](#核心数据结构)
- [客户端 API 说明](#客户端-api-说明)
- [服务端 API 说明](#服务端-api-说明)
- [配置文件说明](#配置文件说明)
- [客户端示例](#客户端示例)
- [服务端示例](#服务端示例)
- [编译和使用](#编译和使用)
- [注意事项](#注意事项)

---

## 核心数据结构

### 1. 客户端参数 `xqc_mini_cli_args_t`

用于汇总客户端全部配置，包含四部分：

- `net_cfg`：网络参数，如服务端地址、端口、网卡、多路径缓冲区、分片大小
- `quic_cfg`：QUIC 参数，如拥塞控制、多路径开关、调度器、TLS 相关信息
- `env_cfg`：环境参数，如日志、证书、上传下载文件路径
- `req_cfg`：HTTP 请求参数，如 `GET/POST`、URL、Host、Path

补充字段：

- `req_stream_cnt`：请求流数量
- `send_data_len`：POST 发送长度；为 `0` 时通常按输入文件大小发送

### 2. 服务端参数 `xqc_mini_svr_args_t`

用于汇总服务端配置，包含：

- `net_cfg`：监听地址、端口、超时、收发缓冲等
- `quic_cfg`：多路径、调度器、拥塞控制、Session Ticket Key
- `env_cfg`：证书、日志、数据目录、默认发送文件、上传输出路径

### 3. 客户端上下文 `xqc_mini_cli_ctx_t`

保存客户端运行时上下文：

- `event_base *eb`：libevent 事件循环
- `xqc_engine_t *engine`：xquic 客户端引擎
- 日志 / keylog 输出句柄
- 指向当前参数 `args`

### 4. 服务端上下文 `xqc_mini_svr_ctx_t`

保存服务端运行时上下文：

- `event_base *eb`
- `xqc_engine_t *engine`
- 当前 socket fd 类型信息
- 日志相关句柄
- 指向当前参数 `args`

### 5. 客户端连接 `xqc_mini_cli_user_conn_t`

描述一次客户端连接及其请求状态，包含：

- `h3_conn`：HTTP/3 连接
- `paths[MAX_PATH_CNT]`：多路径信息
- 并发请求调度状态
- 上传 / 下载统计
- 超时、路径重试、接收队列等事件对象

### 6. 服务端连接 `xqc_mini_svr_user_conn_t`

描述服务端单个用户连接，包含：

- 连接 ID
- socket / 本地地址 / 对端地址
- 超时事件
- 多路径状态记录

---

## 客户端 API 说明

### 1. 参数与环境初始化

#### `xqc_mini_cli_init_args()`
初始化客户端默认参数。

```c
void xqc_mini_cli_init_args(xqc_mini_cli_args_t *args);
```

说明：

- 设置默认服务端地址 `10.12.54.143:8443`
- 默认启用多路径：`multipath = 1`
- 默认调度器：`balanced`
- 默认拥塞控制：`BBR`
- 默认方法：`POST`
- 默认上传文件：`client_sent.txt`
- 默认下载文件：`client_received.bin`
- 默认流数：`1`

#### `xqc_mini_cli_init_env()`
初始化客户端环境。

```c
int xqc_mini_cli_init_env(xqc_mini_cli_ctx_t *ctx, xqc_mini_cli_args_t *args);
```

说明：

- 先调用 `xqc_mini_cli_init_args()`
- 再尝试加载读取 `client_config.txt`
- 最后初始化运行时上下文 `ctx`

返回值：

- 成功：`XQC_OK`
- 失败：`XQC_ERROR`

#### `xqc_mini_cli_parse_cmd_args()：该函数目前未使用，仅用于后续补充`
解析客户端命令行参数。

```c
int xqc_mini_cli_parse_cmd_args(xqc_mini_cli_args_t *args, int argc, char *argv[]);
```

支持参数：

- `-i`：绑定网卡名，可多次指定
- `-s`：并发流数
- `-a`：服务端地址
- `-p`：服务端端口
- `-m`：多路径调度器，支持 `minrtt`、`backup`、`balanced`、`rap`、`act`、`bw`
- `-M`：HTTP 方法，支持 `GET`、`POST`
- `-u`：请求路径 / URL
- `-o`：下载输出文件
- `-l`：POST 发送长度

注意：

- 当前 `mini_client_main.c` 中这一步被注释掉了，默认不会自动解析命令行参数
- 如需启用，需要在主流程中恢复对该函数的调用

#### `init_args()`
客户端主程序中使用的封装初始化函数。

```c
int init_args(xqc_mini_cli_args_t **args, xqc_mini_cli_ctx_t *ctx, int argc, char *argv[]);
```

说明：

- 分配 `xqc_mini_cli_args_t`
- 调用 `xqc_mini_cli_init_env()`
- 创建 xquic engine
- 初始化 engine context

返回值：

- 成功：`XQC_OK`
- 失败：`XQC_ERROR`

---

### 2. 引擎与连接创建

#### `xqc_mini_cli_init_xquic_engine()`
创建并初始化客户端 xquic 引擎。

```c
int xqc_mini_cli_init_xquic_engine(xqc_mini_cli_ctx_t *ctx, xqc_mini_cli_args_t *args);
```

说明：

- 获取客户端默认引擎配置
- 初始化 SSL 配置
- 注册 engine callback 和 transport callback
- 调用 `xqc_engine_create()` 创建客户端引擎

#### `xqc_mini_cli_init_conn_settings()`
初始化连接层配置。

```c
void xqc_mini_cli_init_conn_settings(xqc_conn_settings_t *settings, xqc_mini_cli_args_t *args);
```

说明：

- 根据配置选择拥塞控制算法
- 根据配置选择多路径调度器
- 设置 QUIC 版本、窗口、多路径开关、ACK 策略、重传注入等参数

#### `xqc_mini_cli_user_conn_create()`
创建客户端用户连接对象。

```c
xqc_mini_cli_user_conn_t *xqc_mini_cli_user_conn_create(xqc_mini_cli_ctx_t *ctx);
```

说明：

- 创建连接级状态对象
- 初始化接收队列通知管道
- 初始化超时事件、路径重试事件
- 初始化默认 path0

返回值：

- 成功：返回 `xqc_mini_cli_user_conn_t *`
- 失败：返回 `NULL`

#### `create_connection()`
客户端主程序中的连接创建封装。

```c
int create_connection(xqc_mini_cli_user_conn_t **user_conn, xqc_mini_cli_ctx_t *ctx);
```

说明：

- 调用 `xqc_mini_cli_user_conn_create()`
- 关联 `ctx`

返回值：

- 成功：`XQC_OK`
- 失败：`XQC_ERROR`

---

### 3. 请求发送与调度

#### `xqc_mini_cli_send_h3_req()`
发送一个 HTTP/3 请求流。

```c
int xqc_mini_cli_send_h3_req(xqc_mini_cli_user_conn_t *user_conn,
    xqc_mini_cli_user_stream_t *user_stream, int stream_index);
```

说明：

- 创建并格式化请求头
- 根据 `GET/POST` 决定是否发送请求体
- 为每个流设置片段偏移、长度、流索引等信息

#### `xqc_mini_cli_try_schedule_requests()`
按并发上限调度请求流。

```c
int xqc_mini_cli_try_schedule_requests(xqc_mini_cli_user_conn_t *user_conn);
```

说明：

- 当 `active_requests < max_concurrent_requests` 时持续发起新流
- 每次创建一个 `xqc_mini_cli_user_stream_t`
- 内部调用 `xqc_mini_cli_send_h3_req()`

#### `xqc_mini_cli_launch_requests()`
启动首批请求。

```c
int xqc_mini_cli_launch_requests(xqc_mini_cli_user_conn_t *user_conn);
```

说明：

- 启动请求调度
- 实际会进一步调用 `xqc_mini_cli_try_schedule_requests()`

#### `xqc_mini_cli_main_process()`
客户端主流程入口。

```c
int xqc_mini_cli_main_process(xqc_mini_cli_user_conn_t *user_conn, xqc_mini_cli_ctx_t *ctx);
```

说明：

- 创建 QUIC / HTTP3 连接
- 进入“握手完成后再发请求”的工作模式
- 请求并不是在该函数内立即全量发送，而是等待握手与路径状态满足后，由回调触发调度

---

### 4. 事件循环与资源释放

#### `start_event()`
启动客户端事件循环。

```c
void start_event(xqc_mini_cli_ctx_t *ctx);
```

说明：

- 调用 `event_base_dispatch(ctx->eb)`
- 进入 libevent 事件循环，驱动网络读写和 xquic 主逻辑

#### `xqc_mini_cli_on_connection_finish()`
连接结束时执行收尾逻辑。

```c
void xqc_mini_cli_on_connection_finish(xqc_mini_cli_user_conn_t *user_conn);
```

#### `xqc_mini_cli_free_ctx()`
释放客户端上下文。

```c
void xqc_mini_cli_free_ctx(xqc_mini_cli_ctx_t *ctx);
```

#### `xqc_mini_cli_free_user_conn()`
释放客户端连接对象。

```c
void xqc_mini_cli_free_user_conn(xqc_mini_cli_user_conn_t *user_conn);
```

#### `close_all()`
客户端主程序中的统一释放函数。

```c
void close_all(xqc_mini_cli_ctx_t *ctx, xqc_mini_cli_user_conn_t *user_conn);
```

说明：

- 销毁 xquic engine
- 执行连接结束回调
- 释放 `ctx`
- 释放 `user_conn`

---

## 服务端 API 说明

### 1. 参数与环境初始化

#### `xqc_mini_svr_init_args()`
初始化服务端默认参数。

```c
void xqc_mini_svr_init_args(xqc_mini_svr_args_t *args);
```

说明：

- 默认监听 `10.12.54.143:8443`
- 默认启用多路径
- 默认调度器为 `bw`
- 默认拥塞控制为 `BBR`
- 默认数据目录为当前目录 `.`
- 默认返回文件为 `server_data.bin`
- 默认证书：`server.crt`
- 默认私钥：`server.key`

#### `xqc_mini_svr_init_env()`
初始化服务端环境。

```c
int xqc_mini_svr_init_env(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_args_t *args);
```

说明：

- 先加载默认参数
- 再尝试读取 `server_config.txt`
- 最后初始化服务端上下文

#### `xqc_mini_svr_parse_cmd_args()`
解析服务端命令行参数。

```c
int xqc_mini_svr_parse_cmd_args(xqc_mini_svr_args_t *args, int argc, char *argv[]);
```

支持参数：

- `-a`：监听地址
- `-p`：监听端口
- `-f`：默认发送文件
- `-d`：数据目录

注意：

- 当前 `mini_server_main.c` 中这一步同样被注释掉了
- 实际运行时主要依赖 `server_config.txt`

#### `init_args()`
服务端主程序中的封装初始化函数。

```c
int init_args(xqc_mini_svr_args_t **args, xqc_mini_svr_ctx_t *ctx, int argc, char *argv[]);
```

说明：

- 分配 `xqc_mini_svr_args_t`
- 初始化环境
- 创建 xquic server engine
- 初始化 engine context

---

### 2. 引擎与监听创建

#### `xqc_mini_svr_init_xquic_engine()`
创建并初始化服务端引擎。

```c
int xqc_mini_svr_init_xquic_engine(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_args_t *args);
```

说明：

- 获取服务端默认引擎配置
- 加载证书、私钥、Session Ticket Key
- 注册服务端 engine / transport callback
- 创建 xquic server engine

#### `xqc_mini_svr_init_conn_settings()`
初始化服务端连接层参数。

```c
void xqc_mini_svr_init_conn_settings(xqc_engine_t *engine, xqc_mini_svr_args_t *args);
```

说明：

- 设置拥塞控制
- 设置多路径调度器
- 初始化连接层窗口、ACK、multipath 等参数

#### `xqc_mini_svr_create_user_conn()`
创建服务端用户连接对象并初始化监听 socket。

```c
xqc_mini_svr_user_conn_t *xqc_mini_svr_create_user_conn(xqc_mini_svr_ctx_t *ctx);
```

说明：

- 创建用户连接状态
- 初始化 IPv4 监听 socket
- 注册 socket 事件
- 接收后续客户端数据包并驱动 xquic 处理

#### `connection_socket()`
服务端主程序中的封装函数。

```c
xqc_mini_svr_user_conn_t *connection_socket(xqc_mini_svr_ctx_t *ctx);
```

说明：

- 直接调用 `xqc_mini_svr_create_user_conn()`

---

### 3. 事件循环与资源释放

#### `start_event()`
启动服务端事件循环。

```c
void start_event(xqc_mini_svr_ctx_t *ctx);
```

#### `xqc_mini_svr_free_ctx()`
释放服务端上下文。

```c
void xqc_mini_svr_free_ctx(xqc_mini_svr_ctx_t *ctx);
```

#### `xqc_mini_svr_free_user_conn()`
释放服务端用户连接对象。

```c
void xqc_mini_svr_free_user_conn(xqc_mini_svr_user_conn_t *user_conn);
```

#### `close_all()`
服务端主程序中的统一清理函数。

```c
void close_all(xqc_mini_svr_ctx_t *ctx, xqc_mini_svr_user_conn_t *user_conn);
```

说明：

- 销毁 xquic engine
- 释放服务端上下文
- 释放用户连接对象

---

## 配置文件说明

### 1. 客户端配置文件

客户端启动时会尝试读取 `client_config.txt`。

支持字段：

- `server_addr`：服务器地址
- `server_port`：服务器端口
- `stream_cnt`：并发流帧数目
- `stream_range_size`：单个流帧负载大小
- `method`：方法POST/GET
- `cc`：拥塞算法，cubic/bbr
- `mp_sched`：调度算法：minrtt/bw/backup
- `interface` / `interfaces`：网卡名称
- `kernel_sndbuf`：内核发送缓冲区大小
- `kernel_revbuf`：内核接收缓冲区大小
- `user_send_buf_size`：用户态发送缓冲区大小
- `user_recv_buf_size`：用户态接收缓冲区大小
- `download_path`：下载目的文件到本地的位置
- `download_target`：下载目的文件的地址
- `upload_path` / `send_file`：上传的文件地址
- `use_zlog`：zlog日志是否启动
- `zlog_conf`：zlog配置文件地址
- `zlog_category`：zlog分类器

示例：

```ini
server_addr=10.12.54.143
server_port=8443
method=POST
stream_cnt=4
stream_range_size=4*1024*1024
mp_sched=bw
interfaces=eth0,wlan0
upload_path=client_sent.txt
download_path=client_received.bin

#目前可以取消zlog配置，将use_zlog=0
use_zlog=1
zlog_conf=zlog.conf
zlog_category=xquic_mini_client
```

### 2. 服务端配置文件

服务端启动时会尝试读取 `server_config.txt`。

支持字段：

- `listen_addr` / `server_addr`：监听地址
- `listen_port` / `server_port`：监听端口`
- `mp_sched`：调度算法，最好和客户端一致：minrtt/bw/backup
- `kernel_sndbuf`：内核发送缓冲区大小
- `kernel_revbuf`：内核接收缓冲区大小
- `user_send_buf_size`：用户态发送缓冲区大小
- `user_recv_buf_size`：用户态接收缓冲区大小
- `upload_path` / `upload_file`：客户端上传文件到本地的地址
- `use_zlog`：zlog日志是否启动
- `zlog_conf`：zlog配置文件地址
- `zlog_category`：zlog分类器

示例：

```ini
listen_addr=10.12.54.143
listen_port=8443
mp_sched=bw
upload_path=server_recv.bin

use_zlog=1
zlog_conf=zlog.conf
zlog_category=xquic_mini_server
```

---

## 客户端示例

客户端主流程可概括为：

```c
#include "mini_client.h"

int main(int argc, char *argv[]) {
    xqc_mini_cli_ctx_t cli_ctx = {0}, *ctx = &cli_ctx;
    xqc_mini_cli_args_t *args = NULL;
    xqc_mini_cli_user_conn_t *user_conn = NULL;

    if (init_args(&args, ctx, argc, argv) != XQC_OK) {
        goto exit;
    }

    if (create_connection(&user_conn, ctx) != XQC_OK) {
        goto exit;
    }

    if (xqc_mini_cli_main_process(user_conn, ctx) != XQC_OK) {
        goto exit;
    }

    start_event(ctx);

exit:
    close_all(ctx, user_conn);
    return 0;
}
```

工作流程：

1. 初始化默认参数、配置文件和上下文
2. 创建 xquic engine
3. 创建用户连接和默认 path
4. 建立 HTTP/3 连接
5. 握手完成后按并发配置调度请求
6. 进入 libevent 循环，处理读写、超时、路径重试和回调
7. 传输完成后统一释放资源

---

## 服务端示例

服务端主流程可概括为：

```c
#include "mini_server.h"

int main(int argc, char *argv[]) {
    xqc_mini_svr_ctx_t svr_ctx = {0}, *ctx = &svr_ctx;
    xqc_mini_svr_args_t *args = NULL;
    xqc_mini_svr_user_conn_t *user_conn = NULL;

    if (init_args(&args, ctx, argc, argv) != XQC_OK) {
        goto exit;
    }

    user_conn = connection_socket(ctx);
    if (user_conn == NULL) {
        goto exit;
    }

    start_event(ctx);

exit:
    close_all(ctx, user_conn);
    return 0;
}
```

工作流程：

1. 初始化服务端默认参数和环境
2. 创建 xquic server engine
3. 创建监听 socket 并注册事件
4. 等待客户端包到达
5. 由 xquic 回调处理连接、请求和数据收发
6. 事件循环退出后统一释放资源

---

## 编译和使用

### 1. 编译

mini 模块依赖：

- xquic
- libevent
- zlog
- OpenSSL（由 xquic 依赖链决定，目前是Tongsuo库）

该代码通常作为 xquic 工程的一部分通过 CMake 构建，编译方式应以所在工程的 `CMakeLists.txt` 为准。

### 2. 运行前准备

- 服务端需准备 `server.crt` 和 `server.key`
- 如需 0-RTT，服务端需准备 `session_ticket.key`
- 客户端和服务端默认都会尝试读取配置文件
- 若启用 zlog，需保证 `zlog.conf` 存在且分类名称正确

### 3. 启动建议

- 先启动服务端
- 再启动客户端
- 优先通过 `client_config.txt` / `server_config.txt` 配置参数
- 如果要使用命令行参数，需要先恢复 `main` 中对解析函数的调用

---

## 注意事项

### 1. 当前版本更依赖配置文件

- 客户端 `mini_client_main.c` 中 `xqc_mini_cli_parse_cmd_args()` 被注释
- 服务端 `mini_server_main.c` 中 `xqc_mini_svr_parse_cmd_args()` 也被注释
- 因此现阶段“支持命令行解析”和“实际主流程会调用命令行解析”并不完全等价

### 2. 默认启用了多路径

- 客户端默认 `multipath = 1`
- 客户端默认调度器为 `balanced`
- 服务端默认调度器为 `bw`
- 若环境网卡、多地址或路由条件不满足，多路径效果可能不明显

### 3. 流分片与 4MB 传输

- 客户端和服务端都支持 `stream_range_size`
- 当前默认值为 `4 * 1024 * 1024`
- 多流发送时，每个流会按该大小切分文件区间

### 4. GET 与 POST 语义

- 客户端默认方法是 `POST`
- `GET` 更偏向下载场景
- `POST` 更偏向上传场景
- 具体文件读写、响应拼接与分片处理在回调文件 `mini_client_cb.c` / `mini_server_cb.c` 中完成

### 5. 资源释放

- 事件循环退出后应统一调用 `close_all()`
- 不应在已释放的 `ctx`、`engine`、`user_conn` 上继续操作

### 6. 日志输出

- 当 `use_zlog=1` 时使用 zlog
- 客户端默认日志文件为 `clog.log`，keylog 为 `ckeys.log`
- 服务端默认日志文件为 `slog.log`

---
