# 拥塞控制算法与多路径调度算法报告

本文说明当前项目配置文件中的 `cc` 和 `mp_sched` 是什么、如何解析、当前可用哪些算法、源码位置在哪里，以及报告中应如何解释各算法原理。

## 1. 配置入口和调用链

当前 mini 示例层实际暴露两个算法配置字段：

| 字段 | 当前配置 | 含义 |
| --- | --- | --- |
| `cc` | `mini/client_config.txt:9` 为 `cc=bbr` | 拥塞控制算法，决定每条路径的发送窗口、pacing rate、带宽估计或丢包降窗逻辑 |
| `mp_sched` | `mini/client_config.txt:10` 为 `mp_sched=balanced` | 多路径调度算法，决定发送数据时选择哪条路径 |

客户端配置链路：

```text
mini/client_config.txt
  -> mini/mini_client.c:xqc_mini_cli_load_config_file
  -> xqc_mini_cli_get_cc_cb
  -> xqc_mini_cli_get_sched_cb
  -> xqc_conn_settings_t.cong_ctrl_callback / scheduler_callback
  -> src/transport/xqc_send_ctl.c / src/transport/xqc_conn.c
```

服务端配置链路：

```text
mini/server_config.txt
  -> mini/mini_server.c:xqc_mini_svr_load_config_file
  -> xqc_mini_svr_get_cc_cb
  -> xqc_mini_svr_get_sched_cb
  -> xqc_conn_settings_t.cong_ctrl_callback / scheduler_callback
  -> src/transport/xqc_send_ctl.c / src/transport/xqc_conn.c
```

关键证据：

| 位置 | 结论 |
| --- | --- |
| `mini/mini_client.c:341` | 客户端默认 `cc` 是 `CC_TYPE_BBR` |
| `mini/client_config.txt:9` | 当前客户端配置 `cc=bbr` |
| `mini/client_config.txt:10` | 当前客户端配置 `mp_sched=balanced` |
| `mini/mini_client.c:1563-1565` | 客户端配置解析目前只把 `bbr`、`cubic` 映射为可用拥塞算法 |
| `mini/mini_client.c:443-463` | 客户端 `mp_sched` 支持 `minrtt/backup/balanced/rap/act/bw` |
| `mini/mini_server.c:431-441` | 服务端拥塞算法映射为 BBR/CUBIC，BBR2 代码被注释 |
| `mini/mini_server.c:450-464` | 服务端调度器实际映射 `minrtt/backup/act/bw` |
| `include/xquic/xquic.h:954` | 拥塞控制函数表 `xqc_cong_ctrl_callback_t` |
| `include/xquic/xquic.h:1024` | 多路径调度函数表 `xqc_scheduler_callback_t` |

注意：`mini/common.h:27-30` 中定义了 `CC_TYPE_BBR`、`CC_TYPE_CUBIC`、`CC_TYPE_RENO`、`CC_TYPE_COPA`，但当前 mini 配置解析和回调选择实际只启用了 BBR 与 CUBIC。

## 2. 拥塞控制算法

### 2.1 当前 mini 可直接配置的算法

| 配置值 | 函数表 | 实现文件 | 当前编译状态 | mini 配置状态 |
| --- | --- | --- | --- | --- |
| `bbr` | `xqc_bbr_cb` | `src/congestion_control/xqc_bbr.c` | 默认编译 | 已映射 |
| `cubic` | `xqc_cubic_cb` | `src/congestion_control/xqc_cubic.c` | 默认编译 | 已映射 |

调用链：

```text
mini/mini_client.c:xqc_mini_cli_get_cc_cb
  -> settings->cong_ctrl_callback
  -> src/transport/xqc_send_ctl.c:xqc_send_ctl_create
  -> ACK 时调用 xqc_cong_ctl_on_ack / xqc_cong_ctl_on_ack_multiple_pkts
  -> 丢包时调用 xqc_cong_ctl_on_lost
  -> 发包前读取 xqc_cong_ctl_get_cwnd / xqc_cong_ctl_get_pacing_rate
```

### 2.2 库中存在但当前构建关闭的算法

`CMakeLists.txt:32-35` 定义了这些可选拥塞控制开关，当前 `build/CMakeCache.txt:216-246` 中均为 `OFF`，`include/xquic/xqc_configure.h:5-8` 也显示未定义：

| 算法 | CMake 开关 | 函数表 | 实现文件 | 当前状态 |
| --- | --- | --- | --- | --- |
| Reno | `XQC_ENABLE_RENO` | `xqc_reno_cb` | `src/congestion_control/xqc_new_reno.c` | 源码存在，未编译进当前 build |
| BBR2 | `XQC_ENABLE_BBR2` | `xqc_bbr2_cb` | `src/congestion_control/xqc_bbr2.c` | 源码存在，未编译进当前 build |
| Copa | `XQC_ENABLE_COPA` | `xqc_copa_cb` | `src/congestion_control/xqc_copa.c` | 源码存在，未编译进当前 build |
| Unlimited | `XQC_ENABLE_UNLIMITED` | `xqc_unlimited_cc_cb` | `src/congestion_control/xqc_unlimited_cc.c` | 源码存在，未编译进当前 build |

### 2.3 拥塞算法原理和代码证据

#### BBR

BBR 的核心不是直接等丢包后降窗，而是估计瓶颈带宽和最小 RTT，计算 BDP，再用 pacing rate 和 cwnd 控制发送节奏。

报告中应解释：

| 要点 | 说明 | 代码证据 |
| --- | --- | --- |
| 带宽模型 | 根据 ACK 采样估计 bottleneck bandwidth | `src/congestion_control/xqc_bbr.c`、`src/congestion_control/xqc_sample.c` |
| RTT 模型 | 跟踪 min RTT，避免长期排队导致估计偏高 | `xqc_bbr` 内 min RTT 相关逻辑 |
| 状态机 | STARTUP 探测带宽，DRAIN 排队，PROBE_BW 周期探测，PROBE_RTT 刷新 min RTT | `xqc_bbr_update_cycle_phase`、`xqc_bbr_check_drain` |
| 发送节奏 | `pacing_gain` 和 `cwnd_gain` 控制速率和窗口 | `xqc_bbr_set_pacing_rate`、`xqc_bbr_set_cwnd` |

适用场景：高带宽、长 RTT、蜂窝网络波动场景。风险是估计模型受 ACK 聚合、多路径路径差异影响，需要结合调度器观察。

#### CUBIC

CUBIC 是典型 loss-based 算法。它用三次函数增长拥塞窗口，在丢包时乘性降低窗口。

报告中应解释：

| 要点 | 说明 | 代码证据 |
| --- | --- | --- |
| 三次函数增长 | 离上次丢包点越远增长越快，接近历史窗口时更平滑 | `src/congestion_control/xqc_cubic.c:xqc_cubic_update` |
| 慢启动和拥塞避免 | 初期快速增长，超过阈值后进入拥塞避免 | `xqc_cubic_on_ack` |
| 丢包降窗 | 丢包后按 beta 降低 cwnd，并记录恢复时间 | `xqc_cubic_on_lost` |

适用场景：传统公网链路、丢包作为拥塞信号较可信的场景。风险是蜂窝网络中非拥塞丢包会导致窗口过度下降。

#### Reno

Reno 是更基础的 loss-based 算法。

报告中应解释：

| 要点 | 说明 | 代码证据 |
| --- | --- | --- |
| 慢启动 | 每个 ACK 增加窗口，快速占满链路 | `src/congestion_control/xqc_new_reno.c:xqc_reno_on_ack` |
| 拥塞避免 | 线性增加窗口 | `xqc_reno_on_ack` |
| 丢包处理 | 丢包后乘性降窗并设置 `ssthresh` | `xqc_reno_on_lost` |

适用场景：作为基线算法对比。当前没有编译进 build，也没有在 mini 配置中启用。

#### BBR2

BBR2 是 BBR 的后续版本，重点增强对丢包、inflight 上下界和公平性的控制。

报告中应解释：

| 要点 | 说明 | 代码证据 |
| --- | --- | --- |
| inflight 上下界 | 使用 `inflight_hi/inflight_lo` 限制过激探测 | `src/congestion_control/xqc_bbr2.c` |
| loss-aware | 根据丢包信号调整模型，不只依赖带宽和 RTT | `xqc_bbr2_update_congestion_signals` |
| 状态机 | 类似 BBR，但 PROBE_BW 更细分 | `xqc_bbr2_update_model`、`xqc_bbr2_cb` |

适用场景：高丢包、bufferbloat、蜂窝网络排队明显的场景。当前需要 `-DXQC_ENABLE_BBR2=1` 并补 mini 配置映射后才能测试。

#### Copa

Copa 是 delay-based 算法，目标是在吞吐和排队时延之间取平衡。

报告中应解释：

| 要点 | 说明 | 代码证据 |
| --- | --- | --- |
| 排队时延 | 使用当前 RTT 与 min RTT 的差估计排队 | `src/congestion_control/xqc_copa.c:xqc_copa_on_ack` |
| 目标速率 | 根据 delay 和 `delta` 计算目标速率 | `xqc_copa_on_ack` |
| 方向和速度 | 通过方向和 velocity 调整 cwnd 增减 | `xqc_copa_on_ack`、`xqc_copa_on_lost` |

适用场景：重视低时延的业务。当前未编译进 build，也未在 mini 配置中启用。

#### Unlimited

Unlimited 基本不限制 cwnd，适合实验上限或排除拥塞控制影响，不适合真实公网默认使用。

报告中应解释：

| 要点 | 说明 | 代码证据 |
| --- | --- | --- |
| 不限制窗口 | `get_cwnd` 返回极大值 | `src/congestion_control/xqc_unlimited_cc.c` |
| 实验用途 | 用来判断瓶颈是否来自拥塞控制窗口 | `xqc_unlimited_cc_cb` |

风险很高：可能造成突发、排队、丢包和不公平。

## 3. 多路径调度算法

### 3.1 当前可用和暴露状态

| 配置值 | 函数表 | 实现文件 | 默认编译 | 客户端映射 | 服务端映射 |
| --- | --- | --- | --- | --- | --- |
| `minrtt` | `xqc_minrtt_scheduler_cb` | `src/transport/scheduler/xqc_scheduler_minrtt.c` | 是 | 是 | 是 |
| `backup` | `xqc_backup_scheduler_cb` | `src/transport/scheduler/xqc_scheduler_backup.c` | 是 | 是 | 是 |
| `balanced` | `xqc_balanced_scheduler_cb` | `src/transport/scheduler/xqc_scheduler_balanced.c` | 是 | 是 | 否 |
| `rap` | `xqc_rap_scheduler_cb` | `src/transport/scheduler/xqc_scheduler_rap.c` | 是 | 是 | 否 |
| `act` | `xqc_act_scheduler_cb` | `src/transport/scheduler/xqc_scheduler_act.c` | 是 | 是 | 是 |
| `bw` | `xqc_bandwidth_scheduler_cb` | `src/transport/scheduler/xqc_scheduler_bandwidth.c` | 是 | 是 | 是 |
| 未暴露 | `xqc_backup_fec_scheduler_cb` | `src/transport/scheduler/xqc_scheduler_backup_fec.c` | 是 | 否 | 否 |
| 未暴露 | `xqc_interop_scheduler_cb` | `src/transport/scheduler/xqc_scheduler_interop.c` | 受 `XQC_ENABLE_MP_INTEROP` 控制 | 否 | 否 |

调度器调用链：

```text
mini/mini_client.c 或 mini/mini_server.c
  -> settings->scheduler_callback
  -> src/transport/xqc_conn.c 初始化 scheduler
  -> 发包时调用 scheduler_callback->xqc_scheduler_get_path
  -> 选出 xqc_path_ctx_t
  -> 数据进入该路径的 send buffer
```

关键调用点：

| 位置 | 含义 |
| --- | --- |
| `src/transport/xqc_conn.c:999-1010` | 初始化连接调度器 |
| `src/transport/xqc_conn.c:2004` | 发包调度时调用 `xqc_scheduler_get_path` |
| `src/transport/xqc_conn.c:2039` | 数据入选中路径的发送队列 |
| `src/transport/xqc_conn.c:2710-2723` | 调度器接收 round start/finish 事件 |

### 3.2 调度算法原理

#### minrtt

`minrtt` 优先选择当前可发送路径中 SRTT 最小的路径。它简单直接，适合低时延优先，但不一定能把多条路径的带宽都用满。

代码证据：`src/transport/scheduler/xqc_scheduler_minrtt.c:xqc_minrtt_scheduler_get_path`。

#### backup

`backup` 把路径分为 available 和 standby。正常优先用 available 路径，standby 更像备份链路，只有在无更高优先级可用路径或探测场景才使用。

代码证据：`src/transport/scheduler/xqc_scheduler_backup.c:xqc_backup_scheduler_get_path`、`xqc_backup_scheduler_handle_conn_event`。

#### rap

`rap` 主要按 RTT 选择路径，重注入时会避免原路径，适合快速把丢失或未确认的数据换路发送。

代码证据：`src/transport/scheduler/xqc_scheduler_rap.c:xqc_rap_scheduler_get_path`。

#### balanced

`balanced` 根据路径可用窗口和有效 SRTT 计算 score，尽量兼顾容量和时延。相比 `minrtt`，它更可能在双路可用时使用多条路径。

代码证据：`src/transport/scheduler/xqc_scheduler_balanced.c:xqc_balanced_scheduler_compute_score`、`xqc_balanced_scheduler_get_path`。

#### act

`act` 会估计每条路径 capacity，再通过权重和 deficit 机制做路径选择。它比单纯 min RTT 更适合多路径聚合场景。

代码证据：`src/transport/scheduler/xqc_scheduler_act.c:xqc_act_scheduler_sample_capacity`、`xqc_act_scheduler_apply_weights`。

#### bw

`bw` 按 cwnd、inflight、SRTT 估算每条路径容量和聚合预算，偏向吞吐聚合。它是当前 GET 服务端默认调度器中较值得重点评估的选项。

代码证据：`src/transport/scheduler/xqc_scheduler_bandwidth.c:xqc_bandwidth_scheduler_compute_scores`、`xqc_bandwidth_scheduler_get_path`。

#### backup_fec 和 interop

`backup_fec` 已默认编译进当前静态库，但 mini 配置没有映射入口。`interop` 受 `XQC_ENABLE_MP_INTEROP` 控制，当前构建关闭。报告中应标为“库层存在，但当前项目未直接调用”。

## 4. 当前报告建议结论

1. 当前可直接通过配置验证的是 `cc=bbr/cubic` 和客户端 `mp_sched=minrtt/backup/balanced/rap/act/bw`。
2. 如果测试 POST 上传，发送端是客户端，应优先改 `mini/client_config.txt` 的 `mp_sched`。
3. 如果测试 GET 下载，发送端是服务端，应优先确认远端 `server_config.txt` 和服务端 `xqc_mini_svr_get_sched_cb` 支持的调度器。
4. `balanced`、`act`、`bw` 更值得作为聚合吞吐对比对象；`minrtt` 更适合作为低时延优先基线。
5. BBR2、Copa、Reno、Unlimited 不能只改配置启用，需要先确认 CMake 开关、编译结果和 mini 映射。

## 5. 验证命令

```powershell
cd C:\Users\DELL\Desktop\xquic_4.1

rg -n "cc=|mp_sched=|CC_TYPE|xqc_mini_cli_get_cc_cb|xqc_mini_cli_get_sched_cb" mini
rg -n "xqc_mini_svr_get_cc_cb|xqc_mini_svr_get_sched_cb|mp_sched" mini\mini_server.c mini\server_config.txt
rg -n "XQC_ENABLE_(BBR2|COPA|RENO|UNLIMITED|MP_INTEROP)" CMakeLists.txt xqc_configure.h.in include\xquic\xqc_configure.h build\CMakeCache.txt
rg -n "const xqc_cong_ctrl_callback_t xqc_.*_cb" src\congestion_control
rg -n "const xqc_scheduler_callback_t xqc_.*_scheduler_cb" src\transport\scheduler
rg -n "xqc_scheduler_get_path|xqc_send_ctl_create|xqc_cong_ctl_on_ack|xqc_cong_ctl_on_lost" src\transport
```
