# NetIns UDP API 使用说明

## 概述

NetIns UDP 是面向 XQUIC UDP/QUIC 的独立客户端连接与状态查询接口。它既可以读取
`mini_client` 和 `mini_server` 已经建立的真实 QUIC 连接，也可以由第三方创建和
关闭一条独立 QUIC/H3 客户端连接，并提供连接状态、多路径状态、每条路径地址、
网卡名称、收发字节、RTT、丢包率、在途数据量和区间速率等信息。独立客户端还可
通过 HTTP/3 stream API 发起 GET/POST 请求、发送请求正文并读取响应正文。



公共头文件：

```c
#include <xquic/netins_udp.h>
```

## 目录

- [接口工作方式](#接口工作方式)
- [API 函数说明](#api-函数说明)
- [输出数据说明](#输出数据说明)
- [客户端示例](#客户端示例)
- [服务端示例](#服务端示例)
- [编译和使用](#编译和使用)
- [验证方法](#验证方法)
- [注意事项](#注意事项)

---

## 接口工作方式

XQUIC 连接建立后，宿主程序为该连接注册一个 NetIns UDP 状态对象。mini 程序在
XQUIC/Libevent 引擎线程中刷新快照，独立客户端在自己的 `poll` 引擎线程中刷新；
当前刷新周期均约为 500 ms。第三方线程调用公开 API 时只复制这份加锁保护的快照，
不会直接进入 XQUIC 引擎。

典型查询流程如下：

```text
XQUIC 真实连接
    -> 宿主程序定期刷新状态快照
    -> netins_udp 进程内注册表
    -> 第三方枚举 connection_id
    -> 第三方读取连接和每路径状态
```

第三方有两种读取方式：

1. 简单轮询：枚举 `connection_id`，调用 `netins_udp_get_snapshot_by_id()`。
2. 长期观察：调用 `netins_udp_open()` 持有句柄，再反复调用
   `netins_udp_get_snapshot()`，最后使用 `netins_udp_close()` 释放句柄。

推荐每 500 ms 或 1 s 查询一次。更高频率不会得到更细的传输采样，而且会增加无效
调用。

除读取已有连接外，本版本还提供独立客户端建连接口。它不启动或复用
`mini_client`，而是为第三方对象单独创建 UDP socket、XQUIC engine 和后台
`poll` 线程。第三方调用 `netins_udp_stream_send/recv` 时只访问有界内存队列，
所有 XQUIC 请求创建、协议发送和回调读取仍由这个后台线程串行完成。

---

## API 函数说明

### 0. 独立客户端连接

#### `netins_udp_client_config_init()`

初始化独立客户端配置的版本、超时、socket 缓冲区和初始拥塞窗口默认值。

```c
void netins_udp_client_config_init(netins_udp_client_config_t *config);
```

调用后必须填写 `server_address` 和 `server_port`。`server_name` 用于 TLS SNI，
为空时使用 `server_address`。

#### `netins_udp_client_create()`

创建尚未连接的客户端对象。该函数不创建网络线程。

```c
int netins_udp_client_create(
    const netins_udp_client_config_t *config,
    netins_udp_client_t **out_client);
```

#### `netins_udp_client_connect()`

创建 UDP socket 和 XQUIC/H3 engine，启动后台线程并等待握手完成。

```c
int netins_udp_client_connect(netins_udp_client_t *client);
```

成功返回 `NETINS_UDP_OK`。超时或连接失败返回 `NETINS_UDP_ERROR`，详细原因通过
客户端错误接口取得。连接超时后后台握手仍可能继续，可再次调用
`netins_udp_client_wait_connected()` 或直接关闭客户端。

`connect()` 成功表示 QUIC/H3 握手完成，不保证所有附加路径都已经验证完成。双路径
程序应继续查询快照，直到 `active_path_count` 达到期望值，或由自己的业务超时结束等待。
指定网卡时底层使用 Linux `SO_BINDTODEVICE`，运行账户可能需要相应网络权限。
`verify_certificate=1` 使用 TLS 库的默认信任链并拒绝不可信证书；当前版本不提供
自定义 CA 文件加载接口。

#### `netins_udp_client_wait_connected()`

```c
int netins_udp_client_wait_connected(
    netins_udp_client_t *client, uint32_t timeout_ms);
```

等待已启动连接完成握手。`timeout_ms=0` 只检查当前状态，不阻塞。

#### 独立客户端状态接口

```c
int netins_udp_client_get_state(netins_udp_client_t *client);

uint64_t netins_udp_client_get_connection_id(
    netins_udp_client_t *client);

int netins_udp_client_get_snapshot(
    netins_udp_client_t *client,
    netins_udp_snapshot_t *snapshot);
```

客户端建立的连接会自动注册到现有进程内状态注册表，因此既可以直接从客户端对象
取得快照，也可以使用 `netins_udp_list_connections()` 查询。

#### 独立客户端错误和关闭接口

```c
int netins_udp_client_get_last_error_code(
    netins_udp_client_t *client);

int netins_udp_client_get_last_error_msg(
    netins_udp_client_t *client,
    char *buffer, size_t buffer_len);

void netins_udp_client_close(netins_udp_client_t *client);
```

`netins_udp_client_close()` 会请求关闭 H3 连接、停止后台线程、销毁 XQUIC engine、
关闭所有路径 socket，并释放客户端对象。

**独立建连示例：**

```c
netins_udp_client_config_t config;
netins_udp_client_config_init(&config);

snprintf(config.server_address, sizeof(config.server_address),
    "%s", "10.12.54.143");
config.server_port = 8443;
snprintf(config.server_name, sizeof(config.server_name),
    "%s", "test.xquic.com");

snprintf(config.interface_names[0],
    sizeof(config.interface_names[0]), "%s", "rmnet_data0");
snprintf(config.interface_names[1],
    sizeof(config.interface_names[1]), "%s", "usb0");
config.interface_count = 2;
config.enable_multipath = 1;

netins_udp_client_t *client = NULL;
int ret = netins_udp_client_create(&config, &client);
if (ret == NETINS_UDP_OK) {
    ret = netins_udp_client_connect(client);
}

if (ret == NETINS_UDP_OK) {
    netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
    ret = netins_udp_client_get_snapshot(client, &snapshot);
}

netins_udp_client_close(client);
```

#### HTTP/3 stream 收发接口

一条 `netins_udp_stream_t` 对应一个真实 HTTP/3 请求。GET 没有请求正文；POST 在
打开 stream 前必须填写准确的 `request_body_length`。当前实现会为 POST 自动加入
本项目 `mini_server` 所需的单分片上传头：`x-stream-index=0`、
`x-stream-count=1`、`x-stream-offset=0` 和 `x-total-length`。

```c
void netins_udp_stream_config_init(netins_udp_stream_config_t *config);

int netins_udp_stream_open(
    netins_udp_client_t *client,
    const netins_udp_stream_config_t *config,
    netins_udp_stream_t **out_stream);

int netins_udp_stream_wait_open(
    netins_udp_stream_t *stream, uint32_t timeout_ms);

int64_t netins_udp_stream_send(
    netins_udp_stream_t *stream, const void *data, size_t size,
    int fin, uint32_t timeout_ms);

int64_t netins_udp_stream_recv(
    netins_udp_stream_t *stream, void *buffer, size_t capacity,
    int *fin, uint32_t timeout_ms);

int netins_udp_stream_get_info(
    netins_udp_stream_t *stream, netins_udp_stream_info_t *out);

int netins_udp_stream_close(
    netins_udp_stream_t *stream, uint32_t timeout_ms);
```

调用约定：

- `send()` 和 `recv()` 的正数返回值是本次实际接受或交付的正文大小。
- `NETINS_UDP_WOULD_BLOCK` 表示队列暂时满/空或等待超时，调用方可以重试。
- `send()` 允许部分接受，调用方必须继续发送尚未接受的数据。
- POST 正文累计达到 `request_body_length` 后自动发送 FIN；提前 FIN 或超长正文返回错误。
- `recv()` 返回 `0` 且 `fin=1` 表示响应正文已结束。
- 调用 `netins_udp_client_close()` 前，应先关闭该客户端创建的所有 stream。

**GET 示例：**

```c
netins_udp_stream_config_t request;
netins_udp_stream_config_init(&request);
snprintf(request.method, sizeof(request.method), "GET");
snprintf(request.path, sizeof(request.path), "/upload/ccc.bin");

netins_udp_stream_t *stream = NULL;
int ret = netins_udp_stream_open(client, &request, &stream);
if (ret == NETINS_UDP_OK) {
    ret = netins_udp_stream_wait_open(stream, 5000);
}

unsigned char buffer[64 * 1024];
int fin = 0;
while (ret == NETINS_UDP_OK && !fin) {
    int64_t n = netins_udp_stream_recv(
        stream, buffer, sizeof(buffer), &fin, 5000);
    if (n < 0) {
        ret = (int)n;
        break;
    }
    /* 使用 buffer[0..n)；例如写入调用方自己的文件。 */
}
(void)netins_udp_stream_close(stream, 5000);
```

**POST 正文循环：**

```c
netins_udp_stream_config_t request;
netins_udp_stream_config_init(&request);
snprintf(request.method, sizeof(request.method), "POST");
snprintf(request.path, sizeof(request.path), "/upload/ccc.bin");
request.request_body_length = body_size;

netins_udp_stream_t *stream = NULL;
int ret = netins_udp_stream_open(client, &request, &stream);
size_t offset = 0;
while (ret == NETINS_UDP_OK && offset < body_size) {
    int64_t n = netins_udp_stream_send(stream, body + offset,
        body_size - offset, 1, 5000);
    if (n < 0) {
        ret = (int)n;
        break;
    }
    offset += (size_t)n;
}
```

默认发送队列上限为 4 MiB，默认接收队列上限为 16 MiB，可在 stream 配置中调整。
这两个队列是背压边界，不是传输总大小限制。

可在构建目录中生成独立建连测试程序：

```sh
cmake -DNETINS_UDP_BUILD_CONNECT_EXAMPLE=ON ..
make -j netins_udp_connect_cli

./netins_udp_connect_cli \
    10.12.54.143 8443 test.xquic.com rmnet_data0 usb0
```

成功时程序输出 `connected`、进程内 `connection_id`、连接状态和当前路径数量，
随后正常关闭连接。只测试默认路由时省略最后两个网卡参数。

### 1. API 版本和返回值

#### `netins_udp_get_api_version()`

获取当前动态库实现的 API 版本。

```c
uint32_t netins_udp_get_api_version(void);
```

**返回值：**

- 当前返回 `NETINS_UDP_API_VERSION`，本版本为 `1`。

**使用示例：**

```c
if (netins_udp_get_api_version() != NETINS_UDP_API_VERSION) {
    fprintf(stderr, "netins_udp API version mismatch\n");
}
```

#### 公共返回码

| 返回码 | 数值 | 含义 |
| --- | ---: | --- |
| `NETINS_UDP_OK` | 0 | 调用成功 |
| `NETINS_UDP_ERROR` | -1 | 参数或内部通用错误 |
| `NETINS_UDP_NOT_FOUND` | -2 | 连接、路径或下标不存在 |
| `NETINS_UDP_BUFFER_TOO_SMALL` | -3 | ID 数组或输出缓冲区容量不足 |
| `NETINS_UDP_VERSION_MISMATCH` | -4 | API 版本或结构体大小不匹配 |
| `NETINS_UDP_WOULD_BLOCK` | -5 | 队列暂时满/空或等待超时，可重试 |
| `NETINS_UDP_CLOSED` | -6 | stream 或连接已经关闭 |

---

### 2. 连接枚举和句柄管理

#### `netins_udp_list_connections()`

枚举当前进程内仍然存活并已注册的 XQUIC 连接。

```c
int netins_udp_list_connections(uint64_t *connection_ids,
    size_t capacity, size_t *connection_count);
```

**参数：**

- `connection_ids`：输出 ID 数组；只查询数量时传 `NULL`。
- `capacity`：数组能够容纳的 `uint64_t` 元素数量。
- `connection_count`：返回当前需要的元素数量，不能为空。

**返回值：**

- 成功：`NETINS_UDP_OK`。
- 容量不足：`NETINS_UDP_BUFFER_TOO_SMALL`，同时通过 `connection_count` 返回
  当前所需容量。
- 参数错误：`NETINS_UDP_ERROR`。

**推荐用法：**

```c
size_t count = 0;
int ret = netins_udp_list_connections(NULL, 0, &count);
if (ret != NETINS_UDP_OK || count == 0) {
    return;
}

uint64_t *ids = calloc(count, sizeof(*ids));
if (ids == NULL) {
    return;
}

size_t actual = 0;
ret = netins_udp_list_connections(ids, count, &actual);
if (ret == NETINS_UDP_BUFFER_TOO_SMALL) {
    /* 两次调用之间可能建立了新连接，应扩大数组后重试。 */
}
```

`connection_id` 只是当前进程注册表中的编号，不是 QUIC Connection ID，也不能在
进程重启后继续使用。

#### `netins_udp_open()`

根据 `connection_id` 打开一个活动连接的状态句柄。

```c
netins_udp_t *netins_udp_open(uint64_t connection_id);
```

**参数：**

- `connection_id`：由 `netins_udp_list_connections()` 或快照中的
  `info.connection_id` 获得。

**返回值：**

- 成功：返回带独立引用的句柄。
- 连接不存在或已从注册表移除：返回 `NULL`。

**说明：**

- 返回的句柄必须使用 `netins_udp_close()` 释放。
- 若在真实连接关闭前已经成功打开句柄，连接关闭后仍可读取最终的 `CLOSED` 或
  `ERROR` 快照。

#### `netins_udp_close()`

释放调用方持有的状态句柄。

```c
void netins_udp_close(netins_udp_t *netins);
```

**参数：**

- `netins`：`netins_udp_open()` 或 `netins_udp_new()` 返回的句柄；可以为 `NULL`。

**重要说明：**

- 本函数只释放 NetIns UDP 句柄引用，不会关闭 UDP socket 或 QUIC 连接。
- 释放后不得继续使用该指针。
- 同一个句柄引用只能释放一次。

#### `netins_udp_new()`

创建一个尚未绑定 XQUIC 连接的内部适配器。

```c
netins_udp_t *netins_udp_new(void);
```

该函数主要供 `mini_client` 和 `mini_server` 的宿主集成代码使用。普通第三方查询
现有连接时不需要调用它，应使用 `netins_udp_list_connections()` 和
`netins_udp_open()`。

---

### 3. 快照和状态查询

#### `netins_udp_get_snapshot_by_id()`

根据 `connection_id` 一次取得完整连接快照，无需长期持有句柄。这是第三方轮询时
最推荐的接口。

```c
int netins_udp_get_snapshot_by_id(uint64_t connection_id,
    netins_udp_snapshot_t *out);
```

**参数：**

- `connection_id`：当前进程内的活动连接编号。
- `out`：完整快照输出，调用前必须使用初始化宏。

**使用示例：**

```c
netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
int ret = netins_udp_get_snapshot_by_id(connection_id, &snapshot);
if (ret == NETINS_UDP_OK) {
    printf("connection=%llu state=%s paths=%u\n",
        (unsigned long long)snapshot.info.connection_id,
        netins_udp_conn_state_name(snapshot.info.connection_state),
        snapshot.path_count);
}
```

#### `netins_udp_get_snapshot()`

使用已打开的句柄复制一份完整、线程安全的缓存快照。

```c
int netins_udp_get_snapshot(netins_udp_t *netins,
    netins_udp_snapshot_t *out);
```

**参数：**

- `netins`：有效状态句柄。
- `out`：完整快照输出，必须使用 `NETINS_UDP_SNAPSHOT_INITIALIZER` 初始化。

**返回值：**

- 返回 `netins_udp_result_t` 中的状态码。

#### `netins_udp_get_info()`

只获取连接身份、生命周期和多路径状态。

```c
int netins_udp_get_info(netins_udp_t *netins,
    netins_udp_info_t *out);
```

调用前必须使用：

```c
netins_udp_info_t info = NETINS_UDP_INFO_INITIALIZER;
```

#### `netins_udp_get_statis()`

只获取所有路径汇总后的累计统计和区间速率。

```c
int netins_udp_get_statis(netins_udp_t *netins,
    netins_udp_statis_t *out);
```

调用前必须使用：

```c
netins_udp_statis_t statis = NETINS_UDP_STATIS_INITIALIZER;
```

`statis` 是当前稳定 API 名称的一部分，第三方代码应保持该拼写。

#### `netins_udp_get_path_statis()`

根据快照数组下标获取一条路径的状态和统计。

```c
int netins_udp_get_path_statis(netins_udp_t *netins,
    size_t path_index, netins_udp_path_statis_t *out);
```

**参数：**

- `path_index`：`paths[]` 数组下标，范围为 `[0, info.path_count)`。
- `out`：路径统计输出，必须使用
  `NETINS_UDP_PATH_STATIS_INITIALIZER` 初始化。

**注意：**

`path_index` 不是 XQUIC 的 `path_id`。第三方必须通过输出结构体中的 `path_id`
识别真实路径，不能假设数组下标永远等于 `path_id`。

---

### 4. 日志回调

#### `netins_udp_set_logger()`

设置或取消当前状态句柄的日志回调。

```c
int netins_udp_set_logger(netins_udp_t *netins,
    netins_udp_logger_cb logger, void *user_data);
```

日志回调类型：

```c
typedef void (*netins_udp_logger_cb)(int level,
    const char *message, void *user_data);
```

日志级别：

- `NETINS_UDP_LOG_LEVEL_DEBUG`
- `NETINS_UDP_LOG_LEVEL_INFO`
- `NETINS_UDP_LOG_LEVEL_WARN`
- `NETINS_UDP_LOG_LEVEL_ERROR`

**使用示例：**

```c
static void udp_logger(int level, const char *message, void *user_data)
{
    FILE *fp = user_data;
    fprintf(fp != NULL ? fp : stderr, "[netins_udp][%d] %s\n",
        level, message);
}

netins_udp_set_logger(handle, udp_logger, stderr);
```

日志回调可能在 XQUIC/Libevent 引擎线程执行，因此回调中不能长时间阻塞，也不应
执行复杂磁盘 I/O 或重新进入传输逻辑。传入 `NULL` 可取消日志回调。

---

### 5. 错误处理

#### `netins_udp_get_last_error_code()`

获取当前句柄最后一次 API 错误码。

```c
int netins_udp_get_last_error_code(netins_udp_t *netins);
```

返回 0 表示没有 API 错误，其余通常为 `errno` 风格错误码。

#### `netins_udp_get_last_error_msg()`

将最后一次 API 错误说明复制到调用方缓冲区。

```c
int netins_udp_get_last_error_msg(netins_udp_t *netins,
    char *buffer, size_t buffer_len);
```

**使用示例：**

```c
char error_msg[NETINS_UDP_ERROR_MSG_LEN];
if (netins_udp_get_last_error_msg(handle,
        error_msg, sizeof(error_msg)) == NETINS_UDP_OK)
{
    fprintf(stderr, "netins_udp error: %s\n", error_msg);
}
```

`last_error_code` 表示 NetIns UDP API 本身的错误；`info.conn_error` 表示 XQUIC
连接错误。两者不是同一个概念。

---

### 6. 状态名称转换

以下函数将枚举值转换成只读英文名称，返回值不需要释放：

```c
const char *netins_udp_conn_state_name(int state);
const char *netins_udp_path_state_name(int state);
const char *netins_udp_app_path_status_name(int status);
```

典型输出包括 `CONNECTED`、`ACTIVE`、`AVAILABLE`、`STANDBY` 和 `FROZEN`。

---

## 输出数据说明

### 1. 输出结构体初始化

每次调用 getter 前都必须使用匹配的初始化宏：

```c
netins_udp_info_t info = NETINS_UDP_INFO_INITIALIZER;
netins_udp_statis_t statis = NETINS_UDP_STATIS_INITIALIZER;
netins_udp_path_statis_t path = NETINS_UDP_PATH_STATIS_INITIALIZER;
netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
```

初始化宏会设置 `struct_size` 和 `api_version`，用于检查调用方与动态库之间的 ABI
兼容性。不要只用 `memset()` 代替初始化宏。

### 2. 连接信息 `netins_udp_info_t`

| 字段 | 含义 |
| --- | --- |
| `connection_id` | 当前进程内连接编号，不是 QUIC CID |
| `role` | `CLIENT` 或 `SERVER` |
| `connection_state` | `NEW`、`CONNECTING`、`CONNECTED`、`CLOSING`、`CLOSED` 或 `ERROR` |
| `handshake_finished` | 1 表示 QUIC/H3 握手完成 |
| `use_udp` | 当前实现固定为 1 |
| `use_quic` | 当前实现固定为 1 |
| `multipath_mode` | 0 未启用；1 Single PNS；2 Multiple PNS |
| `mp_state` | 0 未尝试第二路径；1 至少两条路径验证成功；2 已尝试但尚未成功 |
| `conn_error` | XQUIC 连接错误码，正常时通常为 0 |
| `path_count` | 当前快照中输出的路径数量 |
| `active_path_count` | 传输状态为 `ACTIVE` 的路径数 |
| `validating_path_count` | 正在验证的路径数 |
| `available_path_count` | 应用状态为 `AVAILABLE` 的路径数 |
| `standby_path_count` | 应用状态为 `STANDBY` 的路径数 |
| `frozen_path_count` | 应用状态为 `FROZEN` 的路径数 |
| `alpn` | 协商完成的应用协议，例如 `h3` |
| `conn_info` | XQUIC 连接诊断文本，只用于展示 |
| `extern_conn_info` | XQUIC 扩展诊断文本，只用于展示 |

`conn_info` 和 `extern_conn_info` 不是稳定机器解析格式。第三方程序应使用结构化字段
做判断，不应依赖这两个字符串中的字段顺序。

### 3. 连接统计 `netins_udp_statis_t`

| 字段 | 单位 | 含义 |
| --- | --- | --- |
| `snapshot_time_us` | us | 单调时钟快照时间，不是 Unix 时间戳 |
| `sample_interval_us` | us | 本次与上次内部刷新之间的时间间隔 |
| `bytes_sent` | 字节 | 携带 STREAM/DATAGRAM 的已编码发送字节 |
| `bytes_wire_sent` | 字节 | 所有 QUIC 包的线上发送字节，包括 ACK、PING 等控制包 |
| `bytes_delivered` | 字节 | 发送端通过 ACK 交付统计确认的累计字节 |
| `bytes_retransmitted` | 字节 | 重传包字节，是总发送量的子集 |
| `bytes_recv` | 字节 | 收到的携带 STREAM/DATAGRAM 的已编码包字节，可能含重复数据 |
| `bytes_recv_effective` | 字节 | 接收端接受的非重复 STREAM 载荷字节 |
| `bytes_sent_reinject` | 字节 | 多路径重注入发送字节，是 `bytes_sent` 的子集 |
| `bytes_recv_reinject` | 字节 | 收到的重注入副本包字节，是 `bytes_recv` 的子集 |
| `bytes_recv_effective_reinject` | 字节 | 由重注入副本有效交付的 STREAM 载荷字节 |
| `packets_sent` | 包 | 累计发送 QUIC 包数 |
| `packets_recv` | 包 | 累计接收 QUIC 包数 |
| `packets_lost` | 包 | 被丢包检测标记为丢失的包数 |
| `packets_spurious_lost` | 包 | 后续又被确认的误判丢包数 |
| `datagrams_lost` | 个 | 被判定丢失的 QUIC DATAGRAM 数量 |
| `srtt_us` | us | 初始路径平滑 RTT |
| `min_rtt_us` | us | 初始路径最小 RTT |
| `inflight_bytes` | 字节 | 所有路径尚未被 ACK 确认的在途字节总量 |
| `send_mbps` | Mbit/s | `bytes_sent` 的采样区间速率 |
| `wire_send_mbps` | Mbit/s | `bytes_wire_sent` 的采样区间速率 |
| `delivery_mbps` | Mbit/s | `bytes_delivered` 的采样区间速率 |
| `recv_mbps` | Mbit/s | `bytes_recv` 的采样区间速率 |
| `effective_recv_mbps` | Mbit/s | `bytes_recv_effective` 的采样区间速率 |
| `loss_rate` | 比例 | `packets_lost / packets_sent`，通常为 0.0 到 1.0 |

注意：

- `loss_rate=0.01` 表示约 1%，不是 0.01%。
- `*_reinject` 和 `bytes_retransmitted` 已包含在相应总量中，不能再次相加。
- 第一次快照没有前一个采样点，区间速率为 0 是正常现象。
- `bytes_recv_effective` 是所有流上已接受的 STREAM 载荷，不保证精确等于文件正文，
  其中可能包含 HTTP/3 帧头或其他流数据。
- 连接级 `srtt_us` 和 `min_rtt_us` 当前取初始路径；比较两条路径时应读取每路径
  `srtt_us`。

### 4. 路径统计 `netins_udp_path_statis_t`

| 字段 | 含义 |
| --- | --- |
| `path_id` | XQUIC 路径编号，不等于数组下标 |
| `transport_state` | `INIT`、`VALIDATING`、`ACTIVE`、`CLOSING` 或 `CLOSED` |
| `app_status` | `NONE`、`STANDBY`、`AVAILABLE` 或 `FROZEN` |
| `interface_name` | 客户端网卡名，例如 `rmnet_data0`、`usb0`；服务端可能为空 |
| `local_address` | 本地 `IP:port`，IPv6 格式为 `[IP]:port` |
| `peer_address` | 对端 `IP:port`，IPv6 格式为 `[IP]:port` |
| `packets_sent` / `packets_recv` | 本路径累计收发包数 |
| `bytes_sent` / `bytes_wire_sent` | 本路径数据发送字节和线上发送字节 |
| `bytes_delivered` | 本路径发送端确认交付字节 |
| `bytes_sent_reinject` | 本路径重注入发送字节子集 |
| `bytes_sent_retransmitted` | 本路径重传发送字节子集 |
| `bytes_recv` | 本路径接收的已编码数据包字节 |
| `bytes_recv_reinject` | 本路径接收重注入副本字节子集 |
| `bytes_recv_effective` | 本路径接受的非重复 STREAM 载荷字节 |
| `bytes_recv_effective_reinject` | 本路径由重注入副本有效交付的字节 |
| `srtt_us` | 本路径平滑 RTT，单位微秒 |
| `send_mbps` | 本路径数据发送区间速率 |
| `wire_send_mbps` | 本路径线上发送区间速率 |
| `delivery_mbps` | 本路径确认交付区间速率 |
| `recv_mbps` | 本路径数据接收区间速率 |
| `effective_recv_mbps` | 本路径有效接收区间速率 |

`paths[]` 可能同时包含 `INIT`、`VALIDATING`、`ACTIVE` 或 `CLOSING` 路径。判断
当前可用路径时，不能只检查 `path_count`，还应检查 `transport_state` 和
`app_status`。

### 5. 常用指标选择

| 使用场景 | 推荐指标 |
| --- | --- |
| GET 客户端实际接收速率 | `statis.effective_recv_mbps` |
| GET 服务端确认交付速率 | `statis.delivery_mbps` |
| POST 服务端实际接收速率 | `statis.effective_recv_mbps` |
| POST 客户端确认交付速率 | `statis.delivery_mbps` |
| 观察全部线上开销 | `statis.wire_send_mbps` |
| 判断各路径数据占比 | 每路径 `bytes_delivered` 或 `bytes_recv_effective` |
| 比较路径时延 | 每路径 `srtt_us` |
| 判断发送端是否积压 | `inflight_bytes` 结合 `delivery_mbps` |

例如，发送端按确认交付字节计算两条路径占比：

```text
path_i 占比 = path_i.bytes_delivered / 所有路径 bytes_delivered 之和
```

接收端则优先使用 `bytes_recv_effective` 计算有效数据占比。

---

## 客户端示例

以下代码应编译进 `mini_client` 所在进程或同一宿主组件中。它枚举客户端连接并打印
连接级 GET 接收速率和每路径状态。

```c
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <xquic/netins_udp.h>

static void print_client_snapshot(const netins_udp_snapshot_t *snapshot)
{
    size_t i;

    printf("[udp-client] id=%" PRIu64
           " state=%s handshake=%d mp_mode=%d mp_state=%d"
           " paths=%u active=%u recv=%.3fMbps effective=%.3fMbps"
           " loss=%.4f inflight=%" PRIu64 "\n",
        snapshot->info.connection_id,
        netins_udp_conn_state_name(snapshot->info.connection_state),
        snapshot->info.handshake_finished,
        snapshot->info.multipath_mode,
        snapshot->info.mp_state,
        snapshot->path_count,
        snapshot->info.active_path_count,
        snapshot->statis.recv_mbps,
        snapshot->statis.effective_recv_mbps,
        snapshot->statis.loss_rate,
        snapshot->statis.inflight_bytes);

    for (i = 0; i < snapshot->path_count; ++i) {
        const netins_udp_path_statis_t *path = &snapshot->paths[i];

        printf("  path_id=%" PRIu64
               " interface=%s transport=%s app=%s"
               " local=%s peer=%s srtt=%" PRIu64
               "us effective=%.3fMbps recv_bytes=%" PRIu64 "\n",
            path->path_id,
            path->interface_name[0] != '\0' ? path->interface_name : "unknown",
            netins_udp_path_state_name(path->transport_state),
            netins_udp_app_path_status_name(path->app_status),
            path->local_address,
            path->peer_address,
            path->srtt_us,
            path->effective_recv_mbps,
            path->bytes_recv_effective);
    }
}

void third_party_poll_udp_client(void)
{
    uint64_t *ids = NULL;
    size_t count = 0;
    size_t actual = 0;
    size_t i;
    int ret;

    ret = netins_udp_list_connections(NULL, 0, &count);
    if (ret != NETINS_UDP_OK || count == 0) {
        return;
    }

    ids = calloc(count, sizeof(*ids));
    if (ids == NULL) {
        return;
    }

    ret = netins_udp_list_connections(ids, count, &actual);
    if (ret != NETINS_UDP_OK) {
        free(ids);
        return;
    }

    for (i = 0; i < actual; ++i) {
        netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;

        ret = netins_udp_get_snapshot_by_id(ids[i], &snapshot);
        if (ret != NETINS_UDP_OK
            || snapshot.info.role != NETINS_UDP_ROLE_CLIENT)
        {
            continue;
        }

        print_client_snapshot(&snapshot);
    }

    free(ids);
}
```

可从现有客户端定时器或第三方监控线程每 500 ms 或 1 s 调用
`third_party_poll_udp_client()`。不要在独立进程中运行这段代码。

---

## 服务端示例

服务端使用同一套 API。主要区别是筛选 `NETINS_UDP_ROLE_SERVER`，并通常观察 GET
发送方向的 `delivery_mbps` 和每路径 `bytes_delivered`。

```c
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <xquic/netins_udp.h>

void third_party_poll_udp_server(void)
{
    uint64_t *ids = NULL;
    size_t count = 0;
    size_t actual = 0;
    size_t i;
    int ret;

    ret = netins_udp_list_connections(NULL, 0, &count);
    if (ret != NETINS_UDP_OK || count == 0) {
        return;
    }

    ids = calloc(count, sizeof(*ids));
    if (ids == NULL) {
        return;
    }

    ret = netins_udp_list_connections(ids, count, &actual);
    if (ret != NETINS_UDP_OK) {
        free(ids);
        return;
    }

    for (i = 0; i < actual; ++i) {
        netins_udp_snapshot_t snapshot = NETINS_UDP_SNAPSHOT_INITIALIZER;
        uint64_t delivered_total = 0;
        size_t path_index;

        ret = netins_udp_get_snapshot_by_id(ids[i], &snapshot);
        if (ret != NETINS_UDP_OK
            || snapshot.info.role != NETINS_UDP_ROLE_SERVER)
        {
            continue;
        }

        for (path_index = 0;
             path_index < snapshot.path_count;
             ++path_index)
        {
            delivered_total += snapshot.paths[path_index].bytes_delivered;
        }

        printf("[udp-server] id=%" PRIu64
               " state=%s paths=%u active=%u"
               " delivery=%.3fMbps wire=%.3fMbps\n",
            snapshot.info.connection_id,
            netins_udp_conn_state_name(snapshot.info.connection_state),
            snapshot.path_count,
            snapshot.info.active_path_count,
            snapshot.statis.delivery_mbps,
            snapshot.statis.wire_send_mbps);

        for (path_index = 0;
             path_index < snapshot.path_count;
             ++path_index)
        {
            const netins_udp_path_statis_t *path =
                &snapshot.paths[path_index];
            double share = delivered_total == 0 ? 0.0
                : 100.0 * (double)path->bytes_delivered
                    / (double)delivered_total;

            printf("  path_id=%" PRIu64
                   " state=%s peer=%s srtt=%" PRIu64
                   "us delivered=%" PRIu64
                   " share=%.2f%% delivery=%.3fMbps\n",
                path->path_id,
                netins_udp_path_state_name(path->transport_state),
                path->peer_address,
                path->srtt_us,
                path->bytes_delivered,
                share,
                path->delivery_mbps);
        }
    }

    free(ids);
}
```

服务端通常不能稳定获得 Linux 接收接口名称，因此 `interface_name` 可能为空。服务端
应优先用 `path_id`、`local_address` 和 `peer_address` 区分路径。

---

## 编译和使用

### 1. 所需文件

- 公共头文件：`include/xquic/netins_udp.h`
- API 实现：`src/netins_udp/netins_udp.c`
- 独立客户端建连与 HTTP/3 收发实现：`src/netins_udp/netins_udp_client.c`
- 查询示例：`api/examples/netins_udp_snapshot_example.c`
- 独立建连示例：`api/examples/netins_udp_connect_example.c`
- 内部接口：`src/netins_udp/netins_udp_internal.h`
- 单元测试：`tests/netins_udp/test_netins_udp.c`
- 真实连接日志检查：`tests/netins_udp/verify_real_connection.py`

### 2. CMake 集成

NetIns UDP 已编译进本项目的 `libxquic.so` 和 `libxquic-static.a`。第三方监控源码
应加入运行真实连接的宿主目标，例如：

```cmake
target_sources(mini_client PRIVATE
    third_party_udp_monitor.c
)

target_sources(mini_server PRIVATE
    third_party_udp_monitor.c
)
```

第三方源码中包含：

```c
#include <xquic/netins_udp.h>
```

若作为宿主程序内部模块单独编译目标，应链接宿主正在使用的同一份 XQUIC 库：

```cmake
target_link_libraries(your_host_component PRIVATE xquic)
target_include_directories(your_host_component PRIVATE
    ${PROJECT_SOURCE_DIR}/include
)
```

不要让插件再静态链接一份独立 `netins_udp.c` 或 `libxquic-static.a`，否则插件会得到
另一份空注册表，无法看到宿主的真实连接。

### 3. 安装

正常执行：

```sh
cmake --install build
```

会安装：

- `include/xquic/netins_udp.h`
- `libxquic.so` 和 `libxquic-static.a`
- NetIns UDP 说明文档和示例源码

### 4. 可选单元测试

```sh
mkdir build_api_test
cd build_api_test
cmake -DNETINS_UDP_BUILD_TESTS=ON ..
make -j
ctest --output-on-failure -R netins_udp_unit_test
```

交叉编译时，应在目标设备上运行生成的测试程序。

---

## 验证方法

### 1. 基础接口验证

确认以下行为：

1. `netins_udp_get_api_version()` 返回 `1`。
2. 没有连接时，`netins_udp_list_connections()` 返回数量 0。
3. 建立真实 QUIC 连接后，能够枚举到非无效 `connection_id`。
4. 握手完成后，`connection_state=CONNECTED` 且 `handshake_finished=1`。
5. 双路径运行时，`path_count >= 2`，并能看到两条路径的地址和计数器变化。
6. GET 时客户端 `bytes_recv_effective` 增长，服务端 `bytes_delivered` 增长。
7. POST 时客户端 `bytes_delivered` 增长，服务端 `bytes_recv_effective` 增长。
8. 连接关闭后，新枚举不再返回该 ID；关闭前持有的句柄可读取最终状态。

### 2. 真实连接日志验证

客户端和服务端各运行一次真实传输并保存日志：

```sh
./mini_client > client.log 2>&1
./mini_server > server.log 2>&1
```

使用项目提供的检查脚本：

```sh
python3 tests/netins_udp/verify_real_connection.py client.log server.log
```

只有在明确执行单路径测试时才使用：

```sh
python3 tests/netins_udp/verify_real_connection.py \
    --allow-single-path client.log server.log
```

### 3. 建议第三方验收项

- API 查询不会改变原有 GET/POST 的成功率和文件校验结果。
- 连续轮询不会使 XQUIC 引擎线程出现明显 CPU 增长。
- 客户端能识别 `rmnet_data0`、`usb0` 等已配置接口名称。
- 双路径每条路径的包数、字节数和速率会随真实传输变化。
- 断开一条路径时，路径状态和活跃路径数量能够反映变化。
- 进程退出或连接关闭后不存在悬空查询和崩溃。

---

## 注意事项

### 1. 同进程限制

NetIns UDP 注册表只存在于创建连接的当前进程中。独立 API 客户端可以查询自己
创建的连接，但不能自动读取另一个 `mini_client` 或 `mini_server` 进程中的连接。
跨进程读取需要额外实现 Unix domain socket、共享内存或其他 IPC 服务，本 API
当前不包含该能力。

### 2. 连接控制与只读状态的边界

`netins_udp_client_*` 可以创建和关闭它自己拥有的客户端连接。状态 getter 仍然
只读；`netins_udp_stream_*` 只控制该独立客户端创建的 HTTP/3 请求。当前不提供
以下能力：

- 修改拥塞控制、调度器或 ACK 参数
- 控制 mini_client/mini_server 已有连接
- 手工冻结或切换某一条路径

`netins_udp_close()` 只释放查询句柄，不关闭传输连接。
`netins_udp_client_close()` 则会关闭由该客户端对象创建的真实连接。

### 3. 线程安全

公开查询函数可以从第三方线程调用。接口通过互斥锁复制缓存快照，不会从第三方
线程直接调用 XQUIC 引擎。`stream_send/recv` 同样只操作加锁的有界队列；独立客户
端的 XQUIC engine 始终只由其私有后台线程驱动。单个 stream 可以由发送线程和接收
线程分别使用，但客户端关闭与 stream API 调用应由业务层按“先关闭 stream、再关闭
client”的顺序同步。日志回调可能运行在引擎线程，必须保持轻量。

### 4. 查询频率

mini 宿主和独立客户端当前都约每 500 ms 刷新一次状态。建议查询周期为 500 ms
或 1 s。频率远高于 500 ms 时，连续多次结果可能完全相同。

### 5. 资源管理

- `netins_udp_open()` 成功后必须调用 `netins_udp_close()`。
- `netins_udp_client_create()` 成功后必须调用 `netins_udp_client_close()`。
- 不要重复关闭同一个句柄。
- 关闭后不要继续使用原指针。
- `netins_udp_get_snapshot_by_id()` 不要求调用方持有句柄，更适合简单轮询。

### 6. 结构体兼容性

- getter 的输出结构体必须使用匹配的 `NETINS_UDP_*_INITIALIZER` 初始化。
- 不要读取 `reserved` 字段。
- 不要依赖结构体未公开的内存布局。
- 应在启动时检查 `netins_udp_get_api_version()`。

### 7. 指标解释

- 累计字节和包数从连接创建开始增长。
- `*_mbps` 是相邻内部快照之间的区间速率，不是从连接开始计算的全程平均值。
- `wire_send_mbps` 包含控制包，不能直接当作应用文件 goodput。
- `effective_recv_mbps` 更接近接收侧有效 STREAM 载荷速率，但不是文件层校验结果。
- 精确文件吞吐量仍应使用应用层实际文件字节数和应用层计时计算。

### 8. 路径识别

- 使用 `path_id` 识别路径，不要把 `path_index` 当成稳定 ID。
- 客户端接口名来自配置和路径映射，服务端接口名可能为空。
- 判断路径是否真正可用时，同时检查 `transport_state` 和 `app_status`。

### 9. 故障隔离

mini 程序中的状态适配器分配、绑定或刷新定时器失败时，只会禁用状态查询，不会
主动终止 mini 的 QUIC 数据传输。独立客户端的 socket、engine 或状态绑定失败时，
`netins_udp_client_connect()` 会返回错误，因为这些资源属于该客户端自己的连接。

