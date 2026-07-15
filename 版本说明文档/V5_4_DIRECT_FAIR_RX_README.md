# V5.4 Direct Fair RX 优化说明

## 1. 版本定位

本版本的分支版本号为 `V5.4`，代码基线来自 `v5.5.3`。本次修改聚焦客户端双链路 GET 接收路径，目标是消除旧 RX 架构中的跨路径耦合，并补齐路径生命周期和诊断统计。

本版本不修改：

- adaptive、BBR 等拥塞控制或多路径调度算法；
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
-> 每条路径每轮最多读取 16 个 UDP 包
-> 每轮轮转起始路径并服务两条路径
-> 每个非空轮调用一次 xqc_engine_finish_recv()
-> 最多 2 轮或达到 200 us 回调预算后返回 Libevent
```

固定参数：

```text
per_path_packets = 16
max_rounds_per_callback = 2
callback_budget_us = 200
```

Linux 主路径使用 `recvmmsg()`；其他平台使用最多 16 次非阻塞 `recvfrom()`。批量缓冲区在路径准备阶段从堆上分配并复用，不在回调栈上放置大数组。

启动时会输出：

```text
[resolved-rx] mode=direct_fair_recvmmsg per_path=16 rounds=2 budget_us=200
```

非 Linux 平台的 mode 为 `direct_fair_recvfrom`。

## 4. 公平性和 engine 交付

任一路径的 `EV_READ` 事件都会进入同一个仲裁器。仲裁器每轮从 `rx_next_path_index` 指定的路径开始，依次服务两条路径，并在下一轮交换起始路径。

因此持续满载的快路径不能无限占用 engine；慢路径只要 socket 可读，就会在当前轮或下一轮获得处理机会。未读空的 level-triggered socket 会再次触发 Libevent，不需要应用层主动重新通知。

每轮只要处理过至少一个包，就调用一次：

```c
xqc_engine_finish_recv(engine);
```

这样 ACK、定时器、HTTP/3 回调和拥塞控制反馈不会等待一个无界队列全部排空。

需要注意：`200 us` 是在公平轮边界检查的协作式预算，不是可抢占的硬实时上限。一次完整的双路径公平轮或一次 `xqc_engine_finish_recv()` 本身可能使回调时间超过 200 us。

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
- `batch_max` 不超过 16；
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

`app_submit_mbps` 只表示服务端把响应体提交给 XQUIC 的速度。网络实际交付仍以调度器 ACK 统计的 `delivered_mbps` 为准。

## 9. 主要修改文件

| 文件 | 主要内容 |
|---|---|
| `mini/mini_client.c` | Direct Fair RX、`recvmmsg()`/`recvfrom()` 批量读取、公平仲裁、统一释放、错误处理和 RX 诊断。 |
| `mini/mini_client.h` | 批量缓冲、仲裁状态、路径诊断、全局直方图和延迟字段。 |
| `mini/mini_client_cb.c` | 写失败统一释放、最终诊断、H3 关闭状态和 POST 指标说明。 |
| `mini/mini_client_main.c` | 最终诊断/engine 销毁顺序及失败分支指针初始化。 |
| `mini/CMakeLists.txt` | Linux `mini_client` 增加 `_GNU_SOURCE`，启用 `recvmmsg()` 声明。 |
| `mini/mini_server.h` | H3 连接级 GET 提交统计和监听对象关联。 |
| `mini/mini_server_cb.c` | 每连接统计上下文、墙钟 GET 提交窗口及指标语义。 |
| `mini/mini_server_main.c` | 初始化失败分支的安全清理。 |

## 10. 最终静态 Review 结论

最终 review 已确认：

- 旧 RX 线程、共享 FIFO、通知 pipe 和相关字段没有残留调用；
- 路径事件在 `path_id/is_active` 初始化后才加入 Libevent；
- `EV_READ` 和 `EV_WRITE` 独立判断；
- 每个非空公平轮只调用一次 `xqc_engine_finish_recv()`；
- 仲裁期间的路径释放采用延迟释放，不直接销毁正在执行的回调资源；
- engine 销毁后不再查询连接统计；
- 服务端 GET 提交统计为真正的 H3 连接级状态；
- 非文件 GET 不会污染文件 GET 的统计窗口；
- `git diff --check` 未发现补丁空白错误。

本机没有目标 Linux/ARM 编译和运行环境，因此本次只完成代码级静态 review，没有执行编译、ASan、网络传输或文件校验测试。

## 11. 目标环境验证建议

必须从干净构建目录重新配置，不能直接复用可能包含旧绝对路径的 `CMakeCache.txt`：

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
4. 快路径满载、慢路径间歇突发
5. 传输中移除并重建任一路径
6. recvmmsg 正常、EAGAIN、EINTR 和真实错误
7. 下载完成、超时和连接关闭
8. 每种模式至少 10 次交替测试
```

重点验收：

- 文件哈希一致，无崩溃、UAF、double-free 或 fd 重复关闭；
- 双链路两条路径都有有效接收，不能出现可读路径长期零服务；
- RX 回调和 `finish_recv` 间隔 p99 在 CPU 有余量时以 250 us 为目标；
- 相比旧版本，主机侧可观测 RX 延迟下降，双链路 GET 吞吐提升或相同吞吐下 CPU 降低；
- 单链路吞吐回退不超过预定范围；
- 服务端 `app_submit_mbps` 不再跨连接或重复累计；
- 实际网络收益以 `delivered_mbps` 和客户端 GET 完成速率判断。

如果 RX 回调和 `finish_recv` 延迟已经明显改善，但双链路 GET 吞吐仍没有提升，应停止扩大本补丁范围，另行分析 adaptive 带宽估算、ACK 交付率和路径调度，不在本版本继续混改调度器。

## 12. 回退方式

本版本直接替换旧共享队列 RX 实现，没有保留运行时或编译时开关。现场回退应使用修改前的旧版本二进制和对应静态库，不建议只替换 `mini_client` 而保留不匹配的库文件。
