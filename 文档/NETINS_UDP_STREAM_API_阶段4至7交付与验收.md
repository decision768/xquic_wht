# NetIns UDP 数据流 API：阶段 4 至 7 交付与验收

本文承接阶段 2 的正式单 lane API 和阶段 3 的多 lane 聚合协议，说明阶段 4～7 的
可靠性、背压测试、性能观测、回归保护及最终目标机验收方法。本交付封装已经固定为 4 lane，
客户端和服务端命令均不再接受 lane 数参数。公开定义以
[`include/xquic/netins_udp.h`](include/xquic/netins_udp.h) 为准。

6.4 将数据流内部线协议升级为 version 3，以同时携带 payload CREDIT 和 record CREDIT；
数据流两端必须使用相同的 6.4 版本。文件 API 的 H3 请求/响应协议不受影响。

## 1. 本轮交付范围

| 阶段 | 已完成内容 |
|---|---|
| 4：可靠性 | 取消后确定的 `ECANCELED` 语义、幂等失败完成时间、连接/协议错误状态、应用 reset 错误码可查询 |
| 5：测试能力 | 固定 4 lane 的边界长度、本地队列背压、慢回调累计 CREDIT、延迟 accept、连接中断和自动验收脚本 |
| 6：性能与观测 | 唤醒合并、有界队列/重排高水位、记录计数、逐 lane 收发字节和 demo 吞吐输出 |
| 7：交付收口 | API 初始化器契约测试、28 个公开符号检查、文件/流连接模式隔离守卫、统一验收入口 |

API v1 仍明确不包含跨 QUIC 连接断点续传、自动重连、双向应用数据流，以及同一连接内同时
运行文件任务和数据流。这些不是阶段 4～7 的遗漏，而是当前冻结的版本边界。

本次可靠背压握手把内部 stream wire protocol 升级为 v2。客户端与服务端必须使用本分支
同一版本构建；旧的 v1 stream demo 与 v2 不互通，但公开 C API 版本和 28 个 ABI 符号不变。

## 2. 运行结构

```text
生产者反复写入可复用 Buffer
        │ netins_udp_stream_write()
        ▼
有界发送队列（按 payload 字节背压）
        │ 每条 DATA record 最大 64 KiB
        │ 等待接收端 stream_accept 后的累计 CREDIT
        ▼
轮询分流到固定 lane 0 ... lane 3
        │ QUIC 保证每条 lane 内可靠有序
        ▼
接收端有界跨-lane重排（按逻辑 offset）
        │ 只交付连续区间
        ▼
on_data(buffer, length, offset)
        │ 全 lane FIN + 总长度 + FNV-1a 摘要
        ▼
lane 0 返回统一完成回执
```

应用不需要准备文件，也不需要一次性持有完整数据。`write()` 返回后，已经计入 `accepted`
的部分已被 SDK 复制，原 Buffer 可以立即改写或复用。未被 accepted 的尾部仍归调用方负责。

## 3. 生产者集成模板

连接前启用流模式：

```c
int enabled = 1;
netins_udp_set_opt(client, NETINS_UDP_OPT_STREAM_MODE,
    &enabled, sizeof(enabled));
netins_udp_connect(client, server_ip, port);
```

创建 4 lane 流并持续消费上游 Buffer：

```c
netins_udp_stream_config_t cfg = NETINS_UDP_STREAM_CONFIG_INITIALIZER;
cfg.send_queue_size = 8 * 1024 * 1024;
cfg.receive_queue_size = 8 * 1024 * 1024;

netins_udp_stream_t *stream = NULL;
if (netins_udp_stream_open(client, &cfg, &stream) != 0) {
    /* 处理错误 */
}

while (producer_next_buffer(&data, &length)) {
    size_t consumed = 0;
    while (consumed < length) {
        size_t accepted = 0;
        int rc = netins_udp_stream_write(stream,
            data + consumed, length - consumed, 30000, &accepted);

        /* 无论 rc 是否成功，都必须先处理 accepted。 */
        consumed += accepted;
        if (rc != 0) {
            if (errno == EAGAIN || errno == ETIMEDOUT) {
                /* 按业务策略重试尚未 accepted 的尾部。 */
                continue;
            }
            /* 连接、协议或取消错误：停止生产并关闭。 */
            break;
        }
    }
}

netins_udp_stream_result_t result =
    NETINS_UDP_STREAM_RESULT_INITIALIZER;
if (netins_udp_stream_finish(stream, 60000, &result) == 0) {
    /* result.peer_received_bytes 已由对端回执确认。 */
}
netins_udp_stream_close(stream);
```

非阻塞模式设置 `cfg.flags = NETINS_UDP_STREAM_FLAG_NONBLOCK`。队列无空间时返回 `-1`、
`errno=EAGAIN`；同一次调用仍可能已经 accepted 一部分数据。

## 4. 完成、取消和错误语义

| 操作/状态 | 结果 |
|---|---|
| `flush()` 成功 | 本地队列已提交给 XQUIC；不证明对端已收到 |
| 发送端 `finish()` 成功 | 对端确认总长度和摘要一致，所有 lane 的反向 FIN 已完成 |
| 接收端 `finish()` 成功 | 数据已交付应用，回执及反向 FIN 已被 QUIC ACK，可安全立即 close |
| 本地 `reset(code)` | 未完成流进入 RESET；后续 write/flush/finish 返回取消错误 |
| lane 数/session/offset/FIN 不一致 | 流进入 ERROR，通常为 `EPROTO` |
| 连接或 lane 提前关闭 | 流进入 ERROR，通常为 `ECONNRESET` |
| 等待超时 | 本次调用返回 `ETIMEDOUT`；读取 accepted/结果后由应用决定重试或 reset |
| 累计逻辑 offset 无法继续用 `uint64_t` 表示 | `write()` 返回 `EOVERFLOW`，不再接受会回绕的数据 |

`reset(code)` 的 `code` 可通过 `netins_udp_stream_get_snapshot()` 的 `app_error_code` 在本端
查询。API v1 只通过关闭让对端感知失败，不保证把该 64 位应用错误码传到对端。

## 5. 性能和内存边界

- `send_queue_size` 是待提交 payload 的队列预算；默认 8 MiB，最小 64 KiB。每个 record
  至少按 256 字节内部额度计费，避免连续 1 字节 `write()` 产生数百万个队列节点；
- `receive_queue_size` 限制尚未连续交付的接收 payload；默认 8 MiB，最小 64 KiB。接收端
  同时按 `receive_queue_size / 256` 限制未交付 record 数；
- 客户端只在 4 条 lane 上先发送各 40 字节握手头；收到服务端
  `netins_udp_stream_accept()` 完成后发出的首个累计 CREDIT 前，不发送 payload 或 FIN。
  因而 accept 前临时区每条 lane 最多只保存一个握手头，不再用固定 8 MiB 缓存数据；
- 接收端初始授予 payload 和 record 两类累计 CREDIT，每次 `on_data()` 返回后按已交付字节
  及 record 数同步推进。发送端按跨 lane 的全局逻辑 offset 顺序同时占用两类 CREDIT，
  再由已获得额度的各 lane 独立向
  XQUIC 提交。这样小接收窗口不会被某条 lane 的高 offset record 抢占并形成永久等待，同时
  仍保留多 lane 并行提交；
- 每个 DATA record 额外有 24 字节线上头和一个本地队列节点；
- 应用传入 Buffer 不计入 SDK 队列，`write()` 只复制 accepted 的字节；
- `on_data()` 在独立 delivery worker 上执行且不持有 XQUIC/stream 锁；内存只在回调期间
  有效，业务要异步处理时必须自行复制；回调长期慢于网络会通过 CREDIT 停止发送端继续
  提交，不会无限增长内存或卡住 QUIC 事件线程；
- 多 lane 可能增加乱序占用。固定 4 lane 的生产配置不建议使用只有 64 KiB 的接收队列；
- pipe 唤醒已合并：一批 record 在 I/O 线程处理前只保留一个待处理唤醒，不再逐 record 写管道。

吞吐不是由 lane 数单独保证。本交付固定使用 4 lane，性能验收应保持 lane 不变，比较
`rmnet_data0` 单网卡、`usb0` 单网卡与双网卡聚合结果，避免把 lane 变化混入网卡聚合结论。

## 6. 快照字段怎样看

```c
netins_udp_stream_snapshot_t snapshot =
    NETINS_UDP_STREAM_SNAPSHOT_INITIALIZER;
netins_udp_stream_get_snapshot(stream, &snapshot);
```

| 字段 | 判断用途 |
|---|---|
| `state` / `last_error` / `app_error_code` | 当前生命周期和失败原因 |
| `accepted_bytes` | 发送端已复制，或接收端已连续交付的字节 |
| `peer_received_bytes` | 完成回执确认的字节；完成前通常为 0 |
| `send_queue_bytes/capacity/high_water` | 生产速度是否持续高于网络消费速度 |
| `reorder_bytes/capacity/high_water` | 跨 lane 乱序数据与待回调数据的合计接收内存压力 |
| `records_enqueued/submitted` | 发送记录是否全部交给 XQUIC |
| `records_received/delivered` | 收到的记录是否全部按序交付 |
| `preaccept_lane_count` | 仅接收端有意义；调用 `stream_accept()` 开始时已完整缓存的 lane header 数量 |
| `lane_payload_sent[i]` | 发送端第 i 条 lane 实际承载量 |
| `lane_payload_received[i]` | 接收端第 i 条 lane 实际承载量 |

完成成功时应满足：发送端 `records_enqueued == records_submitted`，接收端
`records_received == records_delivered`，逐 lane 字节之和等于总字节数，两个当前队列值均为 0。

## 7. 构建本轮目标

在已有 AARCH64 和 Linux x86_64 CMake 参数上保留 `-DXQC_ENABLE_MINI_API=ON`，然后分别构建：

```bash
cmake --build BUILD_DIR --target \
  netins_udp_stream_client \
  netins_udp_stream_server \
  mini_stream_protocol_test \
  mini_stream_reorder_test \
  netins_udp_stream_api_contract_test \
  --parallel "$(nproc)"
```

完整链接必须使用与目标架构一致的真实 Tongsuo/BabaSSL、Libevent 和 zlog。AARCH64 客户端
二进制不能在 x86_64 编译机直接运行。

## 8. 一条命令做同机回环验收

在原生 Linux 构建机执行快速验收：

```bash
python3 tests/stream_api_acceptance.py \
  --bin-dir BUILD_DIR/mini \
  --library BUILD_DIR/libxquic.so
```

完整模式额外执行 64 MiB 的固定 4 lane 用例：

```bash
python3 tests/stream_api_acceptance.py \
  --bin-dir BUILD_DIR/mini \
  --library BUILD_DIR/libxquic.so \
  --full
```

最终发布模式必须同时提供真实动态库和文件 API 回归命令；缺少任一项会拒绝以“发布验收”
名义运行：

```bash
python3 tests/stream_api_acceptance.py \
  --release --full \
  --bin-dir BUILD_DIR/mini \
  --library BUILD_DIR/libxquic.so \
  --file-regression-command "./run_file_api_smoke_test.sh"
```

统一入口依次检查：

1. 公共头文件与导出脚本一致，指定库时再检查动态库实际符号；
2. 普通文件连接仍走 `xqc_h3_connect()`，流连接才走 H3 Extended；
3. 协议、重排和公开结构体初始化器单测；
4. 0、1、65535、65536、65537 字节边界；
5. 连续 1024 次 1 字节 `write()`，验证小 record 内存计费和累计 record CREDIT；
6. 固定 4 lane、逐 lane 统计总和和足量数据下的 lane 利用；
7. 64 KiB 非阻塞发送队列产生的部分写和 `EAGAIN`；
8. 64 KiB 接收队列、20 ms 慢回调和 2 MiB 数据强制累计 CREDIT 反复推进，并检查接收高水位；
9. 非空流与空流各延迟 500 ms 调用 `stream_accept()`，并要求 `preaccept_lanes=4`，确定四个
   完整 lane header 都在 accept 前到达，再验证 payload/FIN 等待初始 CREDIT；
10. 两个 accepted connection 并发传输；
11. 服务端确实收到大流 payload 后被终止，客户端必须失败并报告非零 transport/protocol 错误；
12. 接收端 `finish()` 后立即 close 不得破坏发送端完成回执。

脚本会自己生成确定性字节模式，不创建输入文件，也不把完整测试数据放入内存。

## 9. 主板到云服务器直接测试

同机回环通过后，在云服务器先启动接收端。例如 4 GiB、固定 4 lane、seed 7：

```bash
./netins_udp_stream_server 0.0.0.0 18460 4294967296 7 \
  8388608 600000
```

客户端参数中不再包含 lane 数。先测 `rmnet_data0` 单网卡：

```bash
./netins_udp_stream_client SERVER_IP 18460 4294967296 7 \
  8388608 8388608 0 600000 600000 \
  --interface rmnet_data0 --scheduler adaptive
```

每轮服务端只接收一个客户端，完成后重新启动服务端。然后保持字节数、seed、队列和
scheduler 不变，分别运行：

```bash
# usb0 单网卡
./netins_udp_stream_client SERVER_IP 18460 4294967296 7 \
  8388608 8388608 0 600000 600000 \
  --interface usb0 --scheduler adaptive

# rmnet_data0 + usb0 双路径聚合
./netins_udp_stream_client SERVER_IP 18460 4294967296 7 \
  8388608 8388608 0 600000 600000 \
  --interface rmnet_data0 --interface usb0 --scheduler adaptive
```

也可以把 `adaptive` 换成 `minrtt` 或 `rap` 做独立对比，但同一组单网卡/聚合数据必须使用
相同 scheduler。客户端启动时会输出实际的 `lanes=4`、scheduler、接口数量和每个接口名；
单网卡用例必须看到 `interfaces=1`，聚合用例必须看到 `interfaces=2`。

两端都必须出现：

```text
[stream_api_result] ... success=1 ...
```

客户端还会输出 `throughput_mbps`、`eagain`、`partial`，双方都会输出每条 lane 的字节数。
每种网卡模式建议执行 3～5 次并取吞吐中位数。应用层聚合效率可按
`聚合吞吐 /（rmnet 单网卡吞吐 + usb 单网卡吞吐）× 100%` 计算。板端可同时运行
`sar -n DEV 1`，或读取 `/sys/class/net/接口名/statistics/tx_bytes`，用于确认数据确实经过
指定网卡；最终有效吞吐以客户端 `throughput_mbps` 为准。

本地发送队列背压测试（4 lane、64 KiB 发送队列、非阻塞 flag=1）：

```bash
./netins_udp_stream_client SERVER_IP 18460 8388608 7 \
  65536 8388608 1 60000 60000
```

成功且输出 `eagain>0`、`partial>0`，证明业务写入侧触发了本地队列背压。它不等同于
接收端累计 CREDIT 测试。

接收端 CREDIT 循环测试使用 2 MiB 数据、64 KiB 接收窗口和每次回调 20 ms 延迟。初始额度
只能覆盖一个最大 DATA record，完整传输必须在 `on_data()` 返回后反复推进 CREDIT：

```bash
./netins_udp_stream_server 0.0.0.0 18461 2097152 8 \
  65536 60000 --callback-delay-ms 20

./netins_udp_stream_client SERVER_IP 18461 2097152 8 \
  8388608 65536 0 60000 60000 --scheduler adaptive
```

成功时服务端 `records_delivered` 应大于 1，且 `reorder_high_water` 必须大于 0、不超过
65536。自动验收脚本会检查这些条件。

延迟 accept 测试在 QUIC 连接 accept 后等待 500 ms，再挂接逻辑流：

```bash
./netins_udp_stream_server 0.0.0.0 18462 1048576 9 \
  8388608 60000 --accept-delay-ms 500

./netins_udp_stream_client SERVER_IP 18462 1048576 9 \
  8388608 8388608 0 60000 60000 --scheduler adaptive
```

等待期间客户端只能发送四个 40 字节 lane header；payload 和 FIN 必须等初始 CREDIT。自动
验收要求服务端快照输出 `preaccept_lanes=4`，因此不会只因“等待过 500 ms 且最终成功”而误判
为覆盖了 accept 前暂存路径；还会执行一个 0 字节延迟 accept 用例，覆盖“只有 FIN”的边界。
两个 delay 参数默认均为 0，只用于正确性验收，吞吐性能测试不得启用。

## 10. 文件 API 回归

流模式是建连前选项，普通文件 API 默认仍使用原 H3 连接。代码守卫会检查两种模式没有被
合并，但正式发布前仍必须在真实环境运行原 GET/POST 测试：

```bash
python3 tests/mini_api_benchmark/mini_api_benchmark.py \
  --iterations 1 --order rmnet,agg,usb,
```

至少要求单路径 GET、单路径 POST、双路径聚合 GET/POST 均成功，传输字节和原测试基线一致。
文件回归和数据流必须使用不同连接；不要在设置了 `NETINS_UDP_OPT_STREAM_MODE=1` 的连接上
调用 `netins_udp_transfer()`。

同机已有文件回归包装命令时，也可以让统一入口在流测试后执行：

```bash
python3 tests/stream_api_acceptance.py \
  --bin-dir BUILD_DIR/mini \
  --file-regression-command "./run_file_api_smoke_test.sh"
```

## 11. 最终放行标准

- 真实依赖下两种架构完整编译、链接成功，ABI 检查为 28 个公开函数；
- 快速与 `--full` 回环验收全部 PASS；
- 主板到云服务器固定 4 lane 的字节、摘要、逐 lane 总和正确；
- 本地队列背压用例看到 `EAGAIN` 和部分写；慢回调 CREDIT 用例完成且接收高水位不越界；
- 断网或服务端提前结束时不能误报成功；
- 原文件 API GET/POST 回归通过；
- 关闭流和连接后无崩溃、死锁和残留测试进程。

当前开发电脑能完成源码严格编译、对象目标、ABI、协议/重排和契约测试；真实网络吞吐、
多路径聚合收益、依赖完整链接和长时间 RSS 必须在上述目标 Linux/AARCH64 环境完成后才能
宣布最终上线验收通过。
