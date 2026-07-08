# XQUIC 高级能力清单与聚合传输优化建议

本文列出当前 XQUIC 库中已经存在、但当前 mini/GX 工作流尚未充分接入的高级能力，并判断哪些更可能提升网络聚合传输效率。

## 1. 核心结论

当前项目已经启用了多路径、调度器和重注入的一部分能力，但仍有若干库层能力没有接入 mini 配置或当前构建：

| 能力 | 库层是否存在 | 当前 mini/GX 是否已使用 | 优先级 |
| --- | --- | --- | --- |
| scheduler A/B | 存在 | 已部分使用 | 最高 |
| `pacing_on` | 存在 | 未开启 | 高 |
| BBR2 | 源码存在 | 当前构建关闭，mini 未映射 | 高 |
| Copa/Reno/Unlimited | 源码存在 | 当前构建关闭，mini 未映射 | 中 |
| FEC | 框架和 scheme 存在 | 当前构建关闭，mini 未接入 | 中到高，取决于丢包 |
| Datagram/datagram redundancy | API 存在 | 当前文件传输未使用 | 低到中 |
| H3 bytestream | 实现存在 | 当前 mini 未使用 | 低 |
| backup_fec/interop scheduler | 库层存在 | 当前配置未直接暴露或未编译 | 中 |

最实际的优化顺序是：

```text
1. 不改源码做 scheduler A/B
2. 接入 pacing_on=1
3. 启用并接入 BBR2
4. 在确认随机丢包明显后评估 FEC
5. 暂缓 datagram/bytestream 作为文件传输主线
```

## 2. 当前已经使用的能力

### 多路径

当前 mini client 和 mini server 都设置了多路径相关连接参数：

| 位置 | 证据 | 说明 |
| --- | --- | --- |
| `mini/mini_client.c:512` | `settings->enable_multipath = args->quic_cfg.multipath` | 客户端开启多路径协商 |
| `mini/mini_server.c:487` | `.enable_multipath = args->quic_cfg.multipath` | 服务端开启多路径协商 |
| `src/transport/xqc_multipath.c:374-380` | `xqc_conn_enable_multipath` | 双端 transport parameter 都支持时启用多路径 |

### 调度器

当前 mini 会把配置中的 `mp_sched` 映射到 `scheduler_callback`：

| 位置 | 说明 |
| --- | --- |
| `mini/mini_client.c:443-463` | 客户端支持 `minrtt/backup/balanced/rap/act/bw` |
| `mini/mini_server.c:450-464` | 服务端支持 `minrtt/backup/act/bw` |
| `mini/mini_client.c:505` | 写入 `settings->scheduler_callback` |
| `mini/mini_server.c:488` | 写入 `.scheduler_callback = sched` |
| `src/transport/xqc_conn.c:999-1010` | 连接初始化时创建 scheduler |
| `src/transport/xqc_conn.c:2004` | 发包时通过 scheduler 选择路径 |

### 重注入

当前 mini 已经接入 deadline reinjection：

| 位置 | 证据 | 说明 |
| --- | --- | --- |
| `mini/mini_client.c:506` | `settings->reinj_ctl_callback = xqc_deadline_reinj_ctl_cb` | 客户端设置 deadline 重注入控制 |
| `mini/mini_client.c:519` | `settings->mp_enable_reinjection = 1` | 客户端开启重注入 |
| `mini/mini_server.c:494` | `.mp_enable_reinjection = 1` | 服务端开启重注入 |
| `src/transport/reinjection_control/xqc_reinj_deadline.c:149` | `xqc_deadline_reinj_ctl_cb` | deadline 重注入函数表 |
| `src/transport/xqc_reinjection.c` | 重注入实现 | 未 ACK 数据换路重发 |

## 3. 高级能力清单

### 3.1 pacing_on

`pacing_on` 用于控制发送节奏，避免一次性突发塞满队列。它对多路径聚合尤其有意义，因为双 5G 或多网卡场景下，突发发送可能造成队列抖动、短时丢包和 RTT 膨胀。

| 项目 | 说明 |
| --- | --- |
| 设置入口 | `include/xquic/xquic.h:1293` 的 `xqc_conn_settings_t.pacing_on` |
| 默认值 | `src/transport/xqc_conn.c:40` 默认 `.pacing_on = 0` |
| 初始化 | `src/transport/xqc_send_ctl.c:175`、`src/transport/xqc_send_ctl.c:242` 调用 `xqc_pacing_init` |
| 实现 | `src/transport/xqc_pacing.c` |
| 当前 mini 状态 | `mini/mini_client.c` 和 `mini/mini_server.c` 未设置 `pacing_on` |

改造入口：

1. 在 client/server 的连接设置中显式设置 `settings->pacing_on = 1`。
2. 更稳妥的做法是先增加配置键 `pacing_on=0/1`，默认保持 0。
3. 通过日志观察 pacing block、RTT、吞吐和完成时间。

风险：pacing 过严可能降低峰值吞吐，所以需要和 `balanced/bw/act` 做组合 A/B。

### 3.2 BBR2 / Copa / Reno / Unlimited

这些拥塞控制算法源码存在，但当前构建关闭：

| 算法 | 构建开关 | 当前状态 | 价值 |
| --- | --- | --- | --- |
| BBR2 | `XQC_ENABLE_BBR2` | `build/CMakeCache.txt:216` 为 `OFF` | 高丢包或 bufferbloat 场景可能优于 BBR |
| Copa | `XQC_ENABLE_COPA` | `build/CMakeCache.txt:219` 为 `OFF` | 低时延优先场景可评估 |
| Reno | `XQC_ENABLE_RENO` | `build/CMakeCache.txt:234` 为 `OFF` | 基线对比 |
| Unlimited | `XQC_ENABLE_UNLIMITED` | `build/CMakeCache.txt:243` 为 `OFF` | 排除拥塞控制瓶颈的实验工具 |

当前 `include/xquic/xqc_configure.h:5-8` 也显示这些宏未定义。mini 侧 `xqc_mini_cli_get_cc_cb` 和 `xqc_mini_svr_get_cc_cb` 中 BBR2 分支被注释，配置解析也只映射 `bbr/cubic`。

改造入口：

1. 构建时开启对应 CMake 开关，例如 `-DXQC_ENABLE_BBR2=1`。
2. 在 `mini/common.h` 增加或确认对应 `CC_TYPE`。
3. 在 `mini/mini_client.c:xqc_mini_cli_load_config_file` 中解析 `cc=bbr2`。
4. 在 `xqc_mini_cli_get_cc_cb` 和 `xqc_mini_svr_get_cc_cb` 中映射到 `xqc_bbr2_cb`。
5. 重新构建并确认 `include/xquic/xqc_configure.h` 和 `build/CMakeCache.txt`。

推荐优先评估 BBR2。Copa/Reno/Unlimited 作为对照或特定场景再展开。

### 3.3 FEC

FEC 用冗余编码换取丢包恢复能力，适用于随机丢包明显、重传 RTT 成本高的场景。它不能解决路由不通、接口不可达或单纯带宽不足问题。

| 项目 | 说明 |
| --- | --- |
| 总开关 | `CMakeLists.txt:40` 的 `XQC_ENABLE_FEC` |
| scheme 开关 | `XQC_ENABLE_XOR`、`XQC_ENABLE_RSC`、`XQC_ENABLE_PKM` |
| 当前构建状态 | `build/CMakeCache.txt:225/231/237/246` 均为 `OFF` |
| 主要源码 | `src/transport/xqc_fec.c`、`src/transport/xqc_fec_scheme.c`、`src/transport/fec_schemes/` |
| 参数入口 | `include/xquic/xquic.h:1467-1491` |
| transport parameter | `src/transport/xqc_transport_params.c` |

可用 scheme：

| scheme | 文件 | 说明 |
| --- | --- | --- |
| XOR | `src/transport/fec_schemes/xqc_xor.c` | 简单异或冗余，计算成本低，恢复能力有限 |
| Reed-Solomon | `src/transport/fec_schemes/xqc_reed_solomon.c` | 恢复能力强，CPU 成本更高 |
| Packet Mask | `src/transport/fec_schemes/xqc_packet_mask.c` | 面向 packet mask 的恢复策略 |

改造入口：

1. 先用日志确认聚合传输中存在随机丢包，且重传等待明显拖慢完成时间。
2. 构建开启 `XQC_ENABLE_FEC` 和至少一个 scheme。
3. 在 client/server 都设置 `enable_encode_fec`、`enable_decode_fec` 和 `fec_params`。
4. 选择较低码率开始测试，避免 CPU 和带宽冗余成本掩盖收益。
5. 观察 FEC 恢复包数量、重传数量、完成时间和 CPU。

风险：FEC 是高成本改造，应排在 scheduler、pacing、BBR2 之后。

### 3.4 Datagram 和 datagram redundancy

QUIC Datagram 适合低延迟、不可靠消息，不保证像 stream 那样完整可靠。当前 mini 文件上传/下载走 H3 request/stream，不适合直接用 datagram 替代。

| 项目 | 说明 |
| --- | --- |
| API | `include/xquic/xquic.h` 中 `xqc_datagram_send`、`xqc_datagram_send_multiple` 等 |
| 实现 | `src/transport/xqc_datagram.c` |
| transport parameter | `max_datagram_frame_size` |
| 当前 mini 状态 | `mini/mini_client.c:517` 中 `max_datagram_frame_size` 被注释，未启用 datagram |
| 重注入策略 | `src/transport/reinjection_control/xqc_reinj_dgram.c` |

适用场景：实时状态、低延迟消息、可容忍丢包的数据。当前报告中建议列为“库层能力，非文件传输主线”。

### 3.5 H3 bytestream

H3 bytestream 扩展存在于 `src/http3/xqc_h3_ext_bytestream.c`，当前 `xquic-static` build 记录也包含该对象。但当前 mini 主要使用 H3 request/stream 传文件，没有直接使用 bytestream API。

适用场景：需要自定义 H3 扩展通道时再研究。对当前聚合测速目标，收益优先级低于 scheduler、pacing、BBR2 和 FEC。

### 3.6 backup_fec 和 interop scheduler

`backup_fec` 默认编译进当前静态库，但 mini 配置没有暴露。`interop` 受 `XQC_ENABLE_MP_INTEROP` 控制，当前关闭。

| 调度器 | 状态 | 建议 |
| --- | --- | --- |
| `backup_fec` | 源码和函数表存在，当前 mini 未映射 | 若后续启用 FEC，可一起评估 |
| `interop` | 受 `XQC_ENABLE_MP_INTEROP` 控制，当前关闭 | 仅在互通测试或 MPQUIC interop 场景研究 |

## 4. 优先实验方案

### 阶段 1：scheduler A/B，不改源码

目标：先判断现有调度器是否已经能改善聚合吞吐。

建议组合：

| 业务方向 | 发送端 | 优先配置 |
| --- | --- | --- |
| POST 上传 | client | `mp_sched=balanced`、`bw`、`act`、`minrtt` |
| GET 下载 | server | `mp_sched=bw`、`act`、`minrtt`、`backup` |

记录指标：

```text
完成时间
吞吐
双路径是否同时 active
每条路径 RTT
每条路径 loss/retrans
失败率
日志中的 scheduler 选择记录
```

### 阶段 2：pacing_on

目标：降低突发发送造成的队列抖动，观察吞吐是否更稳定。

建议：

1. 先以配置开关接入，默认关闭。
2. 与 `balanced/bw/act` 组合测试。
3. 对比 RTT 抖动、完成时间和丢包。

### 阶段 3：BBR2

目标：评估 BBR2 在蜂窝网络、弱网、bufferbloat 场景下是否优于 BBR。

建议：

1. 构建开启 `-DXQC_ENABLE_BBR2=1`。
2. mini 增加 `cc=bbr2` 配置映射。
3. 与 `cc=bbr`、`cc=cubic` 做 A/B。
4. 固定 scheduler 后再换拥塞算法，避免变量过多。

### 阶段 4：FEC 可行性评估

目标：只在确认随机丢包明显时评估 FEC。

建议：

1. 先确认不是路由、接口绑定、NAT 或服务端 bind 问题。
2. 选择 XOR 或 Packet Mask 做低成本试验。
3. 再考虑 Reed-Solomon。
4. 记录冗余开销、CPU、恢复包数量和最终完成时间。

## 5. GX 工作流注意点

`GX/ssh_gx.py` 的职责是远端切配置并执行 `./mini_client`。它本身不直接开启 pacing、FEC、datagram 或 BBR2。

因此，GX 实验要分清三层：

| 层级 | 控制内容 |
| --- | --- |
| GX Python | 选择远端配置文件、执行 mini_client、解析输出 |
| 远端配置 | `client_config*.txt` 中的 `cc`、`mp_sched`、`interfaces` 等 |
| mini/XQUIC 二进制 | 决定是否真的支持该算法或高级能力 |

不能只改 GX 脚本里的配置文件名就认为启用了库层高级能力。凡是涉及 BBR2、FEC、pacing 配置解析，都需要确认二进制源码和构建结果。

## 6. 验证命令

本地只读核查：

```powershell
cd C:\Users\DELL\Desktop\xquic_4.1

rg -n "XQC_ENABLE_BBR2|XQC_ENABLE_COPA|XQC_ENABLE_FEC|XQC_ENABLE_XOR|XQC_ENABLE_RSC|XQC_ENABLE_PKM" CMakeLists.txt include\xquic\xqc_configure.h build\CMakeCache.txt
rg -n "pacing_on|max_datagram_frame_size|enable_multipath|mp_enable_reinjection|enable_encode_fec|enable_decode_fec|scheduler_callback|reinj_ctl_callback" include src mini
rg -n "mp_sched|cc=|interfaces|mini_client|all streams sent|all download streams finished" mini GX
rg -n "xqc_datagram_send|xqc_h3_ext_bytestream|xqc_fec|xqc_pacing_init|xqc_bbr2_cb|xqc_copa_cb" include src
```

GX 远端核查：

```sh
cd /oemdata/gx/xquic_file
grep -nE 'mp_sched|cc=|interfaces|pacing|fec|datagram' client_config*.txt server_config.txt
./mini_client
```

如果测试时间过长，建议用户在目标设备上手动执行远端命令，再把配置文件、`mini_client` 输出、summary 和 XQUIC 日志回传分析。
