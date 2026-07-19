# V5.4 Direct Fair RX 与多路径诊断优化说明

## 1. 版本定位

本版本的分支版本号为 `V5.4`，代码基线来自 `v5.5.3`。本次修改聚焦客户端双链路 GET 接收路径，目标是消除旧 RX 架构中的跨路径耦合，并补齐路径生命周期和诊断统计。

本版本不修改以下传输行为：

- adaptive、RAP、BW 的选路策略，以及 BBR 拥塞控制参数；
- HTTP/3 GET 分片策略；
- 客户端异步写盘机制；
- 服务端 GET 文件 `pread()` 发送流程；
- POST 数据发送主流程。

## 2. 问题背景

旧客户端 GET 接收链路为：

```text
每路径 RX 线程
-> 每包 malloc/memcpy
-> 共享 RX FIFO
-> 共享通知状态/pipe
-> Libevent/XQUIC engine 线程
-> xqc_engine_finish_recv()
-> ACK 生成
```

两个 RX 线程共同使用一个 FIFO 和一套通知状态。一条路径已经持有通知权时，另一条路径虽然可以继续入队，却不能独立唤醒 engine。通知又要等生产线程把自己的 socket 读到 `EAGAIN` 后才发送，容易产生以下结果：

```text
跨路径等待
-> engine 唤醒延迟
-> finish_recv() 延迟或突发
-> ACK 反馈变晚
-> cwnd/inflight 更新不及时
-> 调度器降低对该路径的使用
-> 双链路 GET 聚合吞吐受限
```

POST 的主要大流量方向是客户端发送，旧客户端 RX 队列主要承载 ACK；GET 的主要大流量方向是客户端接收，因此该问题对 GET 更敏感。

## 3. Direct Fair RX 新架构

旧 RX 线程、共享 FIFO、队列锁、通知 pipe 和逐包内存复制已删除，替换为：

```text
两个持久化 EV_READ 事件
-> 同一个 Libevent 线程公平仲裁
-> 每次回调只执行一个公平轮
-> 每条路径在本轮最多读取 64 个 UDP 包
-> 每次回调结束后轮转下一次回调的起始路径
-> 非空回调调用一次 xqc_engine_finish_recv()
-> 返回 Libevent，未读空的 socket 依靠水平触发再次唤醒
```

固定接收策略：

```text
per_path_packets = 64
rounds_per_callback = 1（固定行为，不是独立控制参数）
```

Linux 主路径使用 `recvmmsg()`；其他平台使用最多 64 次非阻塞 `recvfrom()`。批量缓冲区在路径准备阶段从堆上分配并复用，不在回调栈上放置大数组。

启动时会输出：

```text
[resolved-rx] mode=direct_fair_recvmmsg per_path=64 rounds=1
```

非 Linux 平台的 mode 为 `direct_fair_recvfrom`。

## 4. 公平性和 engine 交付

任一路径的 `EV_READ` 事件都会进入同一个仲裁器。一次回调只执行一个公平轮：从 `rx_next_path_index` 指定的路径开始，依次服务两条路径；本次回调结束后交换起始路径，使下一次回调从另一条路径开始。

因此持续满载的快路径不能在单次回调中无限占用 engine；慢路径只要 socket 可读，就会在当前回调获得处理机会。未读空的 level-triggered socket 会再次触发 Libevent，不需要应用层主动重新通知，也不存在固定等待时间。

每次回调只要处理过至少一个包，就调用一次：

```c
xqc_engine_finish_recv(engine);
```

这样 ACK、定时器、HTTP/3 回调和拥塞控制反馈不会等待一个无界队列全部排空。

单路径模式下每次回调最多读取 64 个包；双路径模式下每次回调最多读取 128 个包。若任一路径仍有未读数据，Libevent 会在后续事件循环中继续触发回调。`callback_us` 等时间统计继续保留，但不再参与是否继续收包的控制。

单路径持续可读时，回调行为为：

```text
回调 N：A 最多读取 64 包 -> finish_recv() -> 返回 Libevent
回调 N+1：A 最多读取 64 包 -> finish_recv() -> 返回 Libevent
```

双路径持续可读时，回调行为为：

```text
回调 N：A 最多读取 64 包 -> B 最多读取 64 包 -> finish_recv()
回调 N+1：B 最多读取 64 包 -> A 最多读取 64 包 -> finish_recv()
```

例如 A 的内核队列有 100 包、B 有 1 包，第一次回调最多处理 A 的 64 包和 B 的 1 包；A 剩余数据使其 socket 继续保持可读，后续回调从 B 开始检查并继续处理 A，不等待 B 凑满批量，也不等待固定时间。

## 5. 路径生命周期和错误安全

路径资源通过统一接口释放，顺序固定为：

```text
标记路径不可用
-> event_del()/event_free()
-> 释放批量缓冲区
-> close(fd)
-> fd 置 -1、指针置 NULL、path_id 置无效值
```

如果路径删除发生在 RX 仲裁器内部，只设置 `rx_release_pending`；仲裁器退出后再释放事件、fd 和缓冲区，避免回调内释放正在使用的对象。

真实接收错误的处理顺序改为：

```text
请求 XQUIC 关闭对应路径
-> 检查关闭结果
-> 成功后同步应用层路径计数并释放 I/O
-> 关闭失败时关闭整个连接，不继续以错误状态重建路径
```

这避免了“应用层已经删除路径，但 XQUIC 仍认为路径 ACTIVE”的状态分裂。路径创建、删除、写失败、接收失败和重建过程使用以下前缀记录：

```text
[diag_rx_path_state]
```

## 6. 退出和释放顺序

最终诊断现在发生在 `xqc_engine_destroy()` 之前。engine 销毁后立即将 `ctx->engine` 置为 `NULL`，释放阶段不再查询已销毁的 engine。

H3 关闭回调完成最终统计后会清空客户端 `h3_conn` 标记，主流程退出时不会再针对已经从 engine 哈希表移除的 CID 输出一组全零 `diag_quic`。

客户端和服务端主函数还将 `user_conn = NULL` 的初始化移动到所有 `goto exit` 之前，避免初始化失败分支使用未初始化指针。

## 7. 客户端诊断日志

客户端每秒限频输出，并在下载完成、超时或连接关闭时输出最终汇总。

### 7.1 全局 RX 汇总

```text
[diag_rx][periodic]
mode=direct_fair_recvmmsg
arbiter_calls=... empty_calls=... fair_rounds=...
packets=... bytes=...
callback_us_p50=... p95=... p99=... max=...
finish_recv=... finish_gap_us_p50=... p95=... p99=... max=...
dispatch_delay_us_p50=... p95=... p99=... max=...
reentry_skips=... deferred_releases=...
socket_ev_read=... socket_ev_write=...
dispatch_metric=post_socket_read_dispatch_not_kernel_queue
```

`dispatch_delay_us` 只统计 socket 批量读取返回后，到数据包交给 XQUIC 前的用户态派发延迟。它不包含数据包在内核 socket 队列中的等待时间，不能直接当作内核 RX 排队延迟。

延迟直方图使用固定边界：

```text
50、100、250、500、1000、5000、20000 us、超过 20000 us
```

### 7.2 每路径 RX 汇总

```text
[diag_rx_path][periodic]
idx=... id=... interface=...
events=... batch_calls=... eagain=...
packets=... bytes=... batch_max=...
service_gap_us_p50=... p95=... p99=... max=...
process_errors=...
```

重点检查：

- 双链路两条路径的 `packets/bytes` 都持续增长；
- 慢路径不存在长时间零服务；
- `batch_max` 不超过 64；
- `finish_gap_us` 和 `callback_us` 不出现持续性大尖峰；
- `process_errors`、`reentry_skips` 和异常释放次数保持可解释。

原有 `[diag_quic]`、`[diag_path]`、`[diag_io]` 和下载结果关键日志继续保留。

## 8. GET/POST 指标口径修正

POST 继续保留脚本依赖的：

```text
all streams sent:
```

同时追加：

```text
metric=app_submit_not_remote_receive
```

它表示应用数据已提交给 XQUIC，不代表服务端已经收到，也不能与 GET 客户端下载完成速率直接比较。

服务端 GET 日志将单流累计耗时改为每个 H3 连接内的墙钟提交窗口：

```text
[diag_server_get_submit][reset]
[diag_server_get_submit][summary]
app_submit_mbps=...
metric=app_submit_not_delivered
```

每个 H3 连接拥有独立 CID、字节数、流数和时间状态。连接关闭时恢复监听级 user data 后再释放连接级统计对象，避免并发连接互相覆盖。

`app_submit_mbps` 只表示服务端把响应体提交给 XQUIC 的速度。实际网络收益应以客户端 GET 完成速率和文件校验为主；adaptive 还可以参考其关闭汇总中的 `delivered_mbps`，RAP 和 BW 不输出该字段，应结合 `[diag_ack_path]`、`[diag_bbr_*]` 和 `[diag_sched_*]` 分析。

## 9. 主要修改文件

| 文件 | 主要内容 |
|---|---|
| `mini/mini_client.c` | Direct Fair RX、`recvmmsg()`/`recvfrom()` 批量读取、公平仲裁、统一释放、错误处理和 RX 诊断。 |
| `mini/mini_client.h` | 批量缓冲、仲裁状态、路径诊断、全局直方图和延迟字段。 |
| `mini/mini_client_cb.c` | 写失败统一释放、最终诊断、H3 关闭状态和 POST 指标说明。 |
| `mini/mini_client_main.c` | 最终诊断/engine 销毁顺序及失败分支指针初始化。 |
| `mini/CMakeLists.txt` | Linux `mini_client` 增加 `_GNU_SOURCE`，启用 `recvmmsg()` 声明。 |
| `mini/mini_server.h` | H3 连接级 GET 提交统计和监听对象关联。 |
| `mini/mini_server.c` | mini 服务端调度器解析、诊断配置读取、启动日志和连接参数下发。 |
| `mini/mini_server_cb.c` | 每连接统计上下文、墙钟 GET 提交窗口及指标语义。 |
| `mini/mini_server_main.c` | 初始化失败分支的安全清理。 |
| `mini/client_config.txt`、`mini/server_config.txt` | `mp_diag` 和 `mp_diag_interval_ms` 的 mini 默认配置。 |
| `include/xquic/xquic.h`、`src/transport/xqc_conn.c` | 调度诊断参数、XQUIC 核心默认值和周期边界处理。 |
| `src/transport/scheduler/xqc_scheduler_common.c/.h` | 公共 CWND 评估、阻塞原因分类和统一调度周期/关闭日志。 |
| `src/transport/scheduler/xqc_scheduler_adaptive.c` | adaptive 选择、probe、fallback、warmup 快照和重注入原因诊断，不改变选路策略。 |
| `src/transport/scheduler/xqc_scheduler_rap.c` | RAP 轻量诊断状态、min-RTT/重注入选择计数和关闭汇总。 |
| `src/transport/scheduler/xqc_scheduler_bandwidth.c` | BW 路径 score、available bytes、available/standby/重注入选择诊断。 |
| `src/transport/xqc_send_ctl.c/.h` | ACK、BBR 样本、app-limited、CWND/pacing 阻塞、发送 burst 和 ACK_MP 生成统计。 |
| `src/transport/xqc_packet_out.c` | 在真实 ACK_MP 生成成功路径记录 ACK gap、generation delay 和 largest-ACK delta。 |
| `tests/unittest/xqc_scheduler_diag_test.c/.h` | CWND 阻塞分类、`check_cwnd=0`、边界值和 socket 发送缓冲上限测试。 |
| `tests/CMakeLists.txt`、`tests/unittest/main.c` | 在启用 CUnit 的测试构建入口中注册新增调度诊断单元测试。 |
| `CMakeLists.txt`、`cmake/CMakeLists.txt` | 同步 act、bw、fastprobe、adaptive 等调度器源文件清单，避免构建脚本覆盖后丢失调度器对象。 |

## 10. 最终静态 Review 结论

最终 review 已确认：

- 旧 RX 线程、共享 FIFO、通知 pipe 和相关字段没有残留调用；
- 路径事件在 `path_id/is_active` 初始化后才加入 Libevent；
- `EV_READ` 和 `EV_WRITE` 独立判断；
- 每次 RX 回调只执行一个公平轮，并跨回调交换起始路径；
- 每个非空 RX 回调只调用一次 `xqc_engine_finish_recv()`；
- 仲裁期间的路径释放采用延迟释放，不直接销毁正在执行的回调资源；
- engine 销毁后不再查询连接统计；
- 服务端 GET 提交统计为真正的 H3 连接级状态；
- 非文件 GET 不会污染文件 GET 的统计窗口；
- ACK_MP 生成统计只挂在真实 `xqc_write_ack_mp_to_one_packet()` 成功路径；
- 调度层与发送控制共用 `xqc_send_ctl_can_send()` 作为可发送判断的真值来源；
- `blocked_real` 和 `blocked_scheduled` 互斥，adaptive 的 CWND、重注入排除和其他资格拒绝不再混记；
- adaptive 的 CWND fallback 和 reinject fallback 分开计数，warmup 结束快照不会混入 BULK 数据；
- adaptive、RAP、BW 都提供统一的调度 tick、summary 和每路径汇总；
- 根目录和构建脚本模板的调度器源文件清单已同步；
- `git diff --check` 未发现补丁空白错误。

本机没有目标 Linux/ARM 编译和运行环境，因此本次只完成代码级静态 review，没有执行编译、ASan、网络传输或文件校验测试。

## 11. 目标环境验证建议

必须从干净构建目录重新配置，不能直接复用可能包含旧绝对路径的 `CMakeCache.txt`。还必须先确认实际使用的 CMake 入口：`xqc_build.sh` 会将 `cmake/CMakeLists.txt` 复制到根目录，该模板在 `XQC_ENABLE_TESTING=ON` 时加入 `tests` 和 `tests/unittest`；当前直接使用未覆盖的根 `CMakeLists.txt` 时，该选项只加入 mini，不会编译 CUnit。需要运行新增单元测试时，应使用目标构建脚本/模板，并在配置结果中确认已生成 CUnit 测试目标：

```bash
rm -rf build
mkdir build
cd build
cmake .. -DXQC_ENABLE_TESTING=ON
make -j
```

建议依次验证：

```text
1. 单链路接口 A，GET 100 MB，校验 SHA-256
2. 单链路接口 B，GET 100 MB，校验 SHA-256
3. 双链路 GET 100 MB，校验 SHA-256
4. 快路径队列 100 包、慢路径队列 1 包，确认单轮上限和跨回调起点轮换
5. 快路径满载、慢路径间歇突发
6. 传输中移除并重建任一路径
7. recvmmsg 正常、EAGAIN、EINTR 和真实错误
8. 下载完成、超时和连接关闭
9. 每种模式至少 10 次交替测试
10. `mp_diag=0`，确认不输出新增周期和关闭汇总
11. `mp_diag_interval_ms=100/250/5000`，确认周期生效；非法值回退 250 ms 且只警告一次
12. 分别使用 `mp_sched=adaptive`、`rap`、`bw` 执行单链路和双链路 GET
13. 确认三个调度器都输出 `[diag_sched_tick]`、`[diag_sched_summary]`、`[diag_sched_path]` 及对应专属汇总
14. 对比诊断开启/关闭的中位吞吐、客户端/服务端 CPU 和日志行数
```

重点验收：

- 文件哈希一致，无崩溃、UAF、double-free 或 fd 重复关闭；
- 双链路两条路径都有有效接收，不能出现可读路径长期零服务；
- RX 回调和 `finish_recv` 间隔 p99 在 CPU 有余量时以 250 us 为目标；
- 相比旧版本，主机侧可观测 RX 延迟下降，双链路 GET 吞吐提升或相同吞吐下 CPU 降低；
- 单链路和双链路中位吞吐回退均不超过 5%，且无新增内核 UDP 接收丢包；
- 服务端 `app_submit_mbps` 不再跨连接或重复累计；
- 实际网络收益以客户端 GET 完成速率为主；adaptive 可辅助参考 `delivered_mbps`，RAP/BW 使用 ACK、BBR 和调度诊断联合解释；
- 诊断开启相对关闭的中位吞吐回退目标不超过 3%，硬上限不超过 5%；
- 每端 100 MB 测试的新增诊断日志目标不超过 300 行。

如果 RX 回调和 `finish_recv` 延迟已经明显改善，但双链路 GET 吞吐仍没有提升，应停止扩大本补丁范围，另行分析 adaptive 带宽估算、ACK 交付率和路径调度，不在本版本继续混改调度器。

## 12. 回退方式

本版本直接替换旧共享队列 RX 实现，没有保留运行时或编译时开关。现场回退应使用修改前的旧版本二进制和对应静态库，不建议只替换 `mini_client` 而保留不匹配的库文件。

## 13. 多路径调度、CWND、ACK 和 pacing 诊断

本轮保持 RX=64、调度策略、BBR、warmup 和 probe 行为不变，只增加诊断。mini 客户端和服务端配置支持：

```text
mp_diag=1
mp_diag_interval_ms=250
```

mini 默认开启，周期范围为 100～5000 ms；非法周期回退到 250 ms 并警告一次。XQUIC 核心默认关闭。切换 `mp_sched=adaptive`、`rap` 或 `bw` 时，三者都输出相同口径的 `[diag_sched_tick]`、`[diag_sched_summary]` 和 `[diag_sched_path]`。

`xqc_scheduler_params_t` 增加了诊断开关和周期字段，没有新增对外公开 API；内部新增有效 CWND 计算和调度评估辅助函数。mini 和 XQUIC 静态库必须整体重新编译，不能混用修改前后的对象文件。

CWND 阻塞分为两个互斥原因：

- `blocked_real`：`bytes_in_flight + packet_size` 已超过有效 CWND，表示真实在途数据已占满窗口；
- `blocked_scheduled`：真实 inflight 尚能容纳该包，但加上 `path_schedule_bytes` 后超过有效 CWND，表示用户态路径发送队列预占窗口。

`effective_cwnd` 是 BBR/拥塞控制窗口和 socket 发送缓冲上限中的较小值。调度诊断和原发送控制共用 `xqc_send_ctl_can_send()` 作为可发送判断的真值来源；`headroom` 使用饱和减法，窗口不足时为 0，不发生无符号回绕。

`[diag_sched_path]` 中的 `checks`、`blocked_real`、`blocked_scheduled`，以及 adaptive 旧汇总中的 `blocked_checks`，都是调度检查次数，不是时长。`send_cwnd_block_duration_us` 和 `pacing_block_duration_us` 从 packet 第一次实际发送检查失败累计到该 packet 后续获准发送；它们是多包累计等待，不能直接除以 GET 墙钟耗时解释为阻塞比例。pacing 与 CWND 分开计数。

`first_block_cwnd`、`first_block_pacing` 和 `both_blocked_*_first` 记录同一 packet 首次观察到的阻塞顺序，用来判断 pacing 是否通常先于 CWND 生效。

每路径 send control 还输出：

```text
[diag_ack_path]  ACK帧、确认包/字节、同路径/跨路径ACK和间隔直方图
[diag_bbr_tick]  BBR模式、带宽样本、app-limited、CWND、inflight和pacing
[diag_bbr_path]  有效/无效样本及delivery-rate范围
[diag_send_path] 实际CWND/pacing阻塞、1ms发送burst和ACK_MP生成统计
```

`[diag_ack_path]` 统计发送端收到 ACK 的 inter-ACK gap 和 ACK delay，两个直方图的边界均为 `100、250、500、1000、2500、5000、10000、20000、50000、100000、+inf us`。`[diag_send_path]` 中的 `ack_gap_us_min/avg/max` 和 `ack_generation_delay_us_min/avg/max` 统计客户端生成 ACK_MP 的间隔和生成延迟，它们不是直方图。`mp_ack_on_any_path=0` 保持不变，same/cross 只用于验证 ACK 实际到达路径。

adaptive 额外输出 probe 考虑、选择和拒绝原因。`probe_denied_secondary_cwnd` 只统计真实 CWND 拒绝，`probe_denied_secondary_reinject` 表示重注入时原路径被排除，`probe_denied_secondary_other` 保留给其他资格拒绝；`fallback_real_inflight` 和 `fallback_scheduled_reservation` 分别记录两类 CWND fallback，`picks_reinject_fallback` 单独记录重注入 fallback。`both_sendable` 等四个矩阵字段表示完成 CWND 和重注入资格过滤后的可选状态。warmup 结束时会冻结 `warmup_elapsed`、`warmup_sent` 和 `warmup_delivered`，后续 primary 切换继续输出该快照，不会混入 BULK 阶段累计值。RAP 输出 min-RTT、原路径 reinject 和无可发送路径统计；BW 输出 available/standby/original 选择以及 score/available-bytes 累计值。
