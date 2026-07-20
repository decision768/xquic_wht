以下内容不属于当前V5.5已启用能力：

- worker侧并行QUIC 1-RTT解密和认证；
- 每路径独立只读RX key context；
- key generation引用计数和并发解密上下文切换；
- worker侧ACK workspace和worker发布snapshot；
- ACK timer通过控制eventfd请求worker发布snapshot；
- GX环境TSAN、ASAN、UBSAN并发验证；
- 完整A/B/C/D实机矩阵和单路径聚合效率验收。

启动日志中的以下字段明确表示当前仍为engine串行解密：

```text
parallel_auth=0
commit_owner=engine_thread
```

在解决当前仲裁和engine轮次问题之前，不建议优先启用worker解密。

## 9. 后续修复优先级

### P0：将仲裁器改为work-conserving

保持每路径第一遍最多32包以保证公平，然后把未使用的总预算借给仍有积压的路径，确保有足够数据时每轮实际达到64包。

验收重点：

- Path 1 `ring_max_depth`显著下降；
- 平均每轮包数接近64；
- `finish_recv`次数下降；
- Path 1 ACK generation delay恢复到低毫秒级；
- 不增加Path 0服务等待p95/p99。

### P1：降低完整应用回调频率

保留ACK每轮优先提交，但评估是否需要每个小批次都执行完整STREAM/HTTP3 read/write回调。ACK反馈频率和应用交付频率应分别控制，避免为了快速ACK而高频运行所有应用逻辑。

### P2：控制ACK过度拆分

检查ACK-first是否减少了原有ACK合并机会。保证ACK及时性的同时，减少同一路径连续小ACK、packet-out分配和socket调用。

### P3：修正诊断时间点

分别记录：

```text
UDP接收时间
认证完成时间
ACK flag实际设置时间
ACK构建时间
ACK socket发送时间
```

避免用`pkt_recv_time`代替ACK due时刻，并增加每路径p50、p95和p99。

### P4：相同环境重新A/B

至少执行：

```text
V5.4.2 direct RX
V5.5 parallel ingress
V5.5 work-conserving修复版
```

每种模式分别测试单Path 0、单Path 1和双路径，至少重复10次，同时记录：

- 客户端完成吞吐和SHA-256；
- 两路径100 ms交付曲线；
- ring深度和ACK generation delay；
- 服务端BBR delivery rate；
- engine调用次数和CPU；
- `mp_diag=0/1`的性能差异。

只有在相同无线环境下完成交叉测试，才能分离代码回退与链路波动。

## 10. 运行和回退开关

V5.5默认客户端配置：

```text
rx_arch=parallel_ingress
mp_diag=1
mp_diag_interval_ms=250
```

需要回退到direct RX时，可将`rx_arch`设置为非`parallel_ingress`值，例如：

```text
rx_arch=direct_fair
```

ACK-first、snapshot和deferred frame apply为编译期选项，变更后必须清理构建目录并同时重编XQUIC静态库和mini程序，不能混用新旧对象文件。

## 11. 主要修改文件

| 文件 | V5.5主要修改 |
|---|---|
| `mini/mini_rx_ring.c/.h` | 新增SPSC ring |
| `mini/mini_client.c/.h` | RX worker、eventfd、ring仲裁、生命周期和RX诊断 |
| `mini/mini_client_cb.c` | 回调和最终诊断衔接 |
| `src/transport/xqc_engine.c/.h` | ACK-first、ACK立即发送、deferred work和兼容RX API |
| `src/transport/xqc_packet.c/.h` | 1-RTT prepare、扫描、排队和串行回退 |
| `src/transport/xqc_frame.c/.h` | 纯帧语法扫描与延迟应用支持 |
| `src/transport/xqc_packet_out.c` | ACK snapshot编码和ACK构建时间链 |
| `src/transport/xqc_recv_record.c/.h` | ACK snapshot range构建 |
| `src/transport/xqc_send_ctl.c/.h` | ACK、BBR、socket发送和路径时间链诊断 |
| `src/transport/xqc_conn.c/.h` | connection generation和deferred work状态 |
| `src/transport/xqc_multipath.c/.h` | path generation和ACK snapshot状态 |
| `src/transport/xqc_timer.c` | ACK timer诊断衔接 |
| `CMakeLists.txt`、`cmake/CMakeLists.txt`、`CMakeOptions.txt` | 新增V5.5编译开关 |
| `mini/CMakeLists.txt` | 编译`mini_rx_ring.c` |

## 12. 当前版本结论

V5.5已经验证了以下设计方向可以正常运行：

- 两个socket能够由独立worker并行接收；
- worker不并发修改XQUIC连接共享状态；
- ACK能够在STREAM/HTTP3回调前构建并立即尝试发送；
- 常见1-RTT帧可以在ACK后再应用；
- snapshot、work pool、ring和路径生命周期未在本次100 MiB测试中出现正确性错误。

但V5.5尚不能作为性能默认版本。当前固定32包/路径的非work-conserving仲裁，在75/25不对称流量下造成Path 1 ring接近满载、ACK年龄升至约84 ms、engine运行次数增加约2.8倍，并伴随双链路吞吐下降。

因此V5.5当前定位应为：