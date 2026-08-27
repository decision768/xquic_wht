# NetIns UDP 数据流 API：阶段 0 与阶段 1 设计

本文冻结第一版数据流 API 的产品语义，并说明单 H3 Extended Bytestream（单 BS）
POC 的实现和验证方法。当前代码位于独立分支 `codex/data-stream-api`。

## 1. 隔离边界

- 原 `netins_udp_transfer()` 文件 GET/POST API、4 MiB 文件分块、并发数和按偏移
  写文件逻辑保持不变。
- 数据流连接使用专用 `h3-ext` ALPN；第一版不允许在同一连接中混用文件任务和
  数据流任务。
- `enable_h3_ext` 只由流模式开启；文件 API 和现有 Mini 程序仍使用默认关闭状态。
- 阶段 1 只生成内存测试数据，不读取、覆盖或创建业务数据文件。

## 2. 第一版范围

第一版提供客户端到服务端的单向逻辑字节流：

```text
producer -> write() -> bounded send queue -> one H3 Bytestream -> on_data()
```

传输具备以下语义：

- 发送前不要求知道总长度；
- 数据产生一段即可提交一段；
- 单 BS 内可靠、有序、不重复交付由 QUIC 保证；
- BS 的 QUIC 数据包仍可使用当前多路径调度器；
- `write()` 边界不是接收端消息边界；
- 发送方 `finish()` 产生 QUIC FIN；
- 接收方读到全部连续数据及 FIN 后返回完成回执；
- 发送方收到有效回执后，`finish()` 才返回成功。

第一版不包含多 lane、跨连接续传、自动重连以及流与文件任务的连接内混用。这些能力
不会改变本节定义的基础 `write/finish` 语义。

## 3. 阶段 2 的公开 API 契约与实现

阶段 1 不向 `netins_udp.h` 暴露尚未实现的链接符号。阶段 2 已按以下契约实现正式头文件。
由于普通文件连接使用 `h3`，而 Bytestream 连接必须在握手时协商 `h3-ext`，双方还必须在
`connect()`/`listen()` 之前显式设置 `NETINS_UDP_OPT_STREAM_MODE=1`。未设置时仍使用原来的
`h3` 文件协议，不改变文件 API 的 ALPN 行为。

```c
typedef struct netins_udp_stream_s netins_udp_stream_t;

typedef enum netins_udp_stream_flag_e {
    NETINS_UDP_STREAM_FLAG_NONE = 0,
    NETINS_UDP_STREAM_FLAG_NONBLOCK = 1U << 0
} netins_udp_stream_flag_t;

typedef void (*netins_udp_stream_data_cb)(
    netins_udp_stream_t *stream,
    const void *data,
    size_t data_len,
    uint64_t offset,
    void *user_data);

typedef struct netins_udp_stream_config_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint32_t lane_count;          /* 阶段 0/1 历史草案；当前公共 API 已移除 */
    uint32_t flags;
    size_t send_queue_size;
    size_t receive_queue_size;
    netins_udp_stream_data_cb on_data;
    void *user_data;
    uint64_t reserved[6];
} netins_udp_stream_config_t;

typedef struct netins_udp_stream_result_s {
    uint32_t struct_size;
    uint32_t api_version;
    uint64_t accepted_bytes;
    uint64_t peer_received_bytes;
    uint64_t elapsed_us;
    uint64_t reserved[5];
} netins_udp_stream_result_t;

int netins_udp_stream_open(
    netins_udp_t *connected_client,
    const netins_udp_stream_config_t *config,
    netins_udp_stream_t **out_stream);

int netins_udp_stream_accept(
    netins_udp_t *accepted_connection,
    const netins_udp_stream_config_t *config,
    netins_udp_stream_t **out_stream);

int netins_udp_stream_write(
    netins_udp_stream_t *stream,
    const void *data,
    size_t data_len,
    uint32_t timeout_ms,
    size_t *accepted);

int netins_udp_stream_flush(
    netins_udp_stream_t *stream,
    uint32_t timeout_ms);

int netins_udp_stream_finish(
    netins_udp_stream_t *stream,
    uint32_t timeout_ms,
    netins_udp_stream_result_t *result);

int netins_udp_stream_reset(netins_udp_stream_t *stream, uint64_t error_code);
void netins_udp_stream_close(netins_udp_stream_t *stream);
```

冻结的调用语义如下：

- `write()` 成功表示字节已经复制或转移到 SDK 的有界发送队列，不表示对端已经收到；
- `accepted` 允许部分接收。队列已满时等待至 `timeout_ms`，超时后返回已接收长度和
  `ETIMEDOUT`；为保持现有 NetIns API 的约定，`timeout_ms == 0` 表示无限等待；需要
  非阻塞行为时使用 `NETINS_UDP_STREAM_FLAG_NONBLOCK`，队列满时返回 `EAGAIN`；
- `data_len == 0` 是合法空操作，不代表流结束；
- `flush()` 只等待本地发送队列排空，不提供对端交付证明；
- `finish()` 禁止后续写入，排空本地队列，发送 FIN，并等待对端完成回执；
- `on_data()` 的内存仅在回调期间有效；正式实现已把它移到有界 delivery worker，回调
  不得关闭或等待同一 stream，持续过慢会耗尽接收队列而不是阻塞 XQUIC 事件线程；
- 一个 stream 允许一个应用发送线程；XQUIC engine 始终由 SDK 内部 IO 所有者线程驱动；
- 所有队列必须有上限，不能用无限内存掩盖生产速度高于网络速度的问题。

阶段 2 的实现约束：

- API version 1 是单向逻辑传输：`open()` 端写入，`accept()` 端通过 `on_data()` 接收；
- 双方都调用 `finish()`：发送方等待经过长度和摘要校验的对端回执，接收方等待 FIN 并
  提交回执；
- 内部工作线程独占 XQUIC/libevent，应用线程只操作有界发送队列；通过 pipe 事件唤醒，
  不使用固定 sleep 轮询；
- 阶段 2 的编译门只允许 `lane_count=1`，但线上帧已经携带 session、lane、逻辑 offset
  和总长度，为阶段 3 的多 lane 重排保留兼容格式；
- 一个连接同时只允许一个逻辑数据流，且不能和文件任务混用。

阶段 3 曾解除阶段 2 的单 lane 编译门并允许 `lane_count=1..8`；当前交付公共 API 已收敛为固定 4 lane。DATA record 按逻辑
offset 在各 lane 间轮询分流，接收端使用有界乱序缓存连续交付；详细实现和测试方法见
`NETINS_UDP_STREAM_API_阶段2与3实现.md`。

## 4. 阶段 1 POC 协议

POC 正向数据不增加 DATA 帧头，生成的字节直接写入一个 Bytestream。客户端发送 FIN 后，
服务端在同一双向 BS 的反向方向发送 40 字节完成回执并发送 FIN。

回执使用网络字节序：

| 偏移 | 长度 | 字段 |
|---:|---:|---|
| 0 | 8 | magic：`NSBSACK1` |
| 8 | 4 | version：1 |
| 12 | 4 | status |
| 16 | 8 | total_bytes |
| 24 | 8 | FNV-1a 64 digest |
| 32 | 8 | first_bad_offset；无错误时为 `UINT64_MAX` |

测试数据由 `seed + logical offset` 确定，不依赖每次 `write()` 或接收回调的分段边界。
因此服务端可以边收边检查每个字节，同时通过 digest 验证完整结果。

## 5. 构建与运行

在现有 Linux/AARCH64 构建参数上增加：

```bash
-DXQC_ENABLE_STREAM_POC=ON
```

构建目标：

```bash
cmake --build BUILD_DIR --target \
  mini_stream_poc_protocol_test \
  mini_stream_poc_server \
  mini_stream_poc_client
```

先执行无网络协议测试：

```bash
BUILD_DIR/mini/mini_stream_poc_protocol_test
```

回环端到端示例：

```bash
BUILD_DIR/mini/mini_stream_poc_server \
  --listen 0.0.0.0 --port 8443 --expect-bytes 64M --seed 7

BUILD_DIR/mini/mini_stream_poc_client \
  --host 127.0.0.1 --port 8443 --bytes 64M --chunk 256K --seed 7
```

服务端默认完成一个连接后退出。`--expect-bytes 0` 表示服务端事先不知道最终长度；
此时仍会在 FIN 后把实际长度和 digest 返回给客户端。使用 `--keep-running` 可持续监听。

成功标准：

- 双方 `stream_poc_result success=1`；
- `bytes` 和 `digest` 完全一致；
- 客户端在发送全部数据前，服务端已经持续触发读回调；
- 客户端只有收到合法回执和反向 FIN 后才报告成功；
- 原 Mini API 文件 GET/POST 构建与回归结果不变。

## 6. 阶段 1 的已知限制

- POC 直接在 XQUIC 事件线程中产生数据，用于验证 H3-ext/BS 主链路；阶段 2 才加入
  生产线程到 IO 线程的有界 Ring Buffer 和正式背压 API。
- POC 是 `lane_count = 1`，尚不验证跨 lane 分流和重排。
- POC 的 FNV-1a 用作快速回归标识；确定性逐字节模式检查才是本测试的主要完整性判据。
- 真正吞吐结论必须在目标 Linux/AARCH64 依赖、真实双网卡和受控丢包/时延环境中测量。
