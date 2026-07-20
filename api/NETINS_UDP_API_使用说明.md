# NetIns UDP API 使用说明

## 概述

NetIns UDP 是面向 XQUIC UDP/QUIC 连接的只读状态查询接口。它用于读取
`mini_client` 和 `mini_server` 已经建立的真实 QUIC 连接，向第三方提供连接状态、
多路径状态、每条路径地址、网卡名称、收发字节、RTT、丢包率、在途数据量和区间
速率等信息。

NetIns UDP 与 MPTCP 版本 NetIns 的定位不同：

- NetIns UDP 不创建 UDP socket，也不提供 `connect`、`listen`、`send` 或 `recv`。
- QUIC 连接仍由原有 XQUIC 客户端、服务端和 Libevent 线程管理。
- 第三方只读取缓存快照，不能通过本接口修改、关闭或调度真实 QUIC 连接。
- 状态注册表位于 `mini_client` 或 `mini_server` 进程内部，不提供跨进程查询。
- 所有公开函数名使用 `netins_udp_` 前缀，以便与 TCP/MPTCP 接口区分。

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

XQUIC 连接建立后，宿主程序为该连接注册一个 NetIns UDP 状态对象。宿主程序当前
每 500 ms 在 XQUIC/Libevent 引擎线程中刷新一次缓存快照。第三方线程调用公开 API
时只复制这份加锁保护的快照，不会直接进入 XQUIC 引擎。

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

---

## API 函数说明

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
- 查询示例：`api/examples/netins_udp_snapshot_example.c`
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

NetIns UDP 注册表只存在于 `mini_client` 或 `mini_server` 当前进程中。另起一个
独立可执行程序，即使链接同名动态库，也不能自动读取其他进程中的连接。跨进程
读取需要额外实现 Unix domain socket、共享内存或其他 IPC 服务，本 API 当前不包含
该能力。

### 2. 只读能力

本 API 只查询状态，不提供以下能力：

- 创建或关闭真实 QUIC 连接
- 主动发送或接收业务数据
- 修改拥塞控制、调度器或 ACK 参数
- 启用、冻结或切换路径

`netins_udp_close()` 只释放查询句柄，不关闭传输连接。

### 3. 线程安全

公开查询函数可以从第三方线程调用。接口通过互斥锁复制缓存快照，不会从第三方
线程直接调用 XQUIC 引擎。日志回调可能运行在引擎线程，必须保持轻量。

### 4. 查询频率

宿主当前约每 500 ms 刷新一次状态。建议查询周期为 500 ms 或 1 s。频率远高于
500 ms 时，连续多次结果可能完全相同。

### 5. 资源管理

- `netins_udp_open()` 成功后必须调用 `netins_udp_close()`。
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

NetIns UDP 的适配器分配、绑定或刷新定时器失败时，只会禁用该连接的状态查询，
不会主动终止 QUIC 数据传输。第三方应把“查询不到状态”和“传输连接失败”作为两类
不同故障处理。

