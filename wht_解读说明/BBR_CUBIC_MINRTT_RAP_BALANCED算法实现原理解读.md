# BBR / CUBIC / minrtt / rap / balanced 算法实现原理解读

本文面向当前工程 `C:\Users\DELL\Desktop\xquic_4.1`，解释 5 个算法在 XQUIC 中的具体实现逻辑：

- 拥塞控制算法：`BBR`、`CUBIC`
- 多路径调度算法：`minrtt`、`rap`、`balanced`

这 5 个算法处在不同层次：

| 类型 | 算法 | 决策对象 | 直接影响 |
| --- | --- | --- | --- |
| 拥塞控制 | BBR | 每条路径最多能发多少、以什么速率发 | `cwnd`、`pacing_rate`、带宽估计 |
| 拥塞控制 | CUBIC | 每条路径拥塞窗口如何增长/下降 | `cwnd`、慢启动、丢包降窗 |
| 多路径调度 | minrtt | 当前包发到哪条路径 | 优先低 RTT 路径 |
| 多路径调度 | rap | 当前包发到哪条路径，尤其重注入时避开原路径 | 低 RTT + 重注入换路 |
| 多路径调度 | balanced | 当前包发到哪条路径 | 综合剩余窗口、RTT、路径利用率做平衡 |

在代码关系上，拥塞控制先决定每条 path 的发送能力，调度器再从多个可发送 path 中选择一个 path：

```text
ACK / LOSS
  -> src/transport/xqc_send_ctl.c
  -> BBR 或 CUBIC 更新 path 的 cwnd / pacing / bandwidth
  -> src/transport/xqc_conn.c 发包时调用 scheduler_callback->xqc_scheduler_get_path
  -> minrtt / rap / balanced 在可发送路径里选 path
```

---

## 1. BBR 拥塞控制

### 1.1 算法目标

BBR 的目标不是等丢包后才认为拥塞，而是主动估算网络的两个核心量：

| 量 | 含义 | BBR 用途 |
| --- | --- | --- |
| Bottleneck Bandwidth | 瓶颈带宽，即当前路径可持续发送速率 | 决定 pacing rate |
| Min RTT | 最小往返时延，近似传播时延 | 和带宽一起估算 BDP |

BBR 认为一条路径合适的在途数据量大约是：

```text
BDP = bottleneck_bandwidth * min_rtt
```

代码中体现为：

- `src/congestion_control/xqc_bbr.h:42-50`：保存 `min_rtt` 和 `bandwidth`
- `src/congestion_control/xqc_bbr.c:386-388`：`xqc_bbr_bdp` 用 `min_rtt * bw` 计算 BDP
- `src/congestion_control/xqc_bbr.c:418-427`：`xqc_bbr_target_cwnd` 用 `gain * BDP` 得到目标窗口

### 1.2 核心状态和参数

BBR 的状态在 `xqc_bbr_t` 中，定义于 `src/congestion_control/xqc_bbr.h:36-159`。

关键字段：

| 字段 | 作用 |
| --- | --- |
| `mode` | 当前 BBR 状态：STARTUP、DRAIN、PROBE_BW、PROBE_RTT |
| `min_rtt` | 当前路径观测到的最小 RTT |
| `bandwidth` | 带宽窗口滤波器，保存近期最大带宽 |
| `congestion_window` | 当前拥塞窗口 |
| `pacing_rate` | 当前发送速率 |
| `pacing_gain` | pacing 增益，乘到带宽上 |
| `cwnd_gain` | cwnd 增益，乘到 BDP 上 |
| `full_bandwidth_reached` | 是否认为 STARTUP 已经探到带宽上限 |
| `cycle_idx` | PROBE_BW 周期中的 gain 位置 |

关键常量在 `src/congestion_control/xqc_bbr.c:21-61`：

| 常量 | 当前值/含义 |
| --- | --- |
| `XQC_BBR_INITIAL_WINDOW` | 初始窗口，32 MSS |
| `xqc_bbr_high_gain` | STARTUP 高增益，约 2.885 |
| `xqc_bbr_drain_gain` | DRAIN 低增益，约 1 / 2.885 |
| `xqc_bbr_cwnd_gain` | PROBE_BW cwnd gain，2.5 |
| `xqc_bbr_pacing_gain[]` | PROBE_BW 周期增益：`1.25, 0.75, 1, 1, ...` |
| `xqc_bbr_fullbw_thresh` | 判断带宽是否还在明显增长 |
| `xqc_bbr_fullbw_cnt` | 连续 3 个 round 带宽增长不足则认为 full bandwidth reached |

### 1.3 四个状态的具体含义

BBR 的状态定义在 `src/congestion_control/xqc_bbr.h:21-29`。

#### STARTUP

目标：快速探测链路带宽上限。

实现：

- `xqc_bbr_enter_startup` 设置 `mode = BBR_STARTUP`
- `pacing_gain = xqc_bbr_high_gain`
- `cwnd_gain = xqc_bbr2_startup_cwnd_gain`

代码位置：`src/congestion_control/xqc_bbr.c:105-110`。

直观理解：刚开始不确定链路容量，所以用大于 1 的 pacing gain 加速发送。如果 ACK 反馈显示带宽还在增长，就继续加速。

#### DRAIN

目标：STARTUP 可能已经把队列打满，DRAIN 用较低发送速率把队列排空。

实现：

- `xqc_bbr_enter_drain` 设置 `mode = BBR_DRAIN`
- `pacing_gain = xqc_bbr_drain_gain`
- `cwnd_gain` 仍保持较高，避免窗口过小

代码位置：`src/congestion_control/xqc_bbr.c:572-576`。

状态切换条件：

- `xqc_bbr_check_full_bw_reached` 判断带宽是否已经不再明显增长
- `xqc_bbr_check_drain` 在 STARTUP 满带宽后进入 DRAIN
- 当 `bytes_inflight <= target_cwnd(1.0, max_bw)` 时进入 PROBE_BW

代码位置：`src/congestion_control/xqc_bbr.c:540-568`、`src/congestion_control/xqc_bbr.c:592-606`。

#### PROBE_BW

目标：在稳定发送时周期性探测是否还有更多带宽。

实现：

- `xqc_bbr_enter_probe_bw` 设置 `mode = BBR_PROBE_BW`
- `cwnd_gain = xqc_bbr_cwnd_gain`
- `cycle_idx` 随机初始化
- `pacing_gain` 从 `xqc_bbr_pacing_gain[]` 中选取

代码位置：`src/congestion_control/xqc_bbr.c:580-586`。

周期逻辑：

```text
1.25：稍微加速，试探更多带宽
0.75：降低速率，排掉前一阶段可能制造的队列
1.0：保持稳定发送
```

代码位置：

- `src/congestion_control/xqc_bbr.c:428-459`：判断是否进入下一个 cycle phase
- `src/congestion_control/xqc_bbr.c:463-470`：更新 `cycle_idx` 和 `pacing_gain`

#### PROBE_RTT

目标：定期压低在途数据，重新测量真实最小 RTT。

实现：

- `xqc_bbr_enter_probe_rtt` 设置 `mode = BBR_PROBE_RTT`
- `pacing_gain = 1`
- `cwnd_gain = 1`
- `xqc_bbr_probe_rtt_cwnd` 会把窗口压到较小水平

代码位置：`src/congestion_control/xqc_bbr.c:611-618`、`src/congestion_control/xqc_bbr.c:662-673`。

切换逻辑：

- `xqc_bbr_update_min_rtt` 检查 min RTT 是否过期
- 过期后进入 PROBE_RTT
- 达到最小持续时间后退出，回到 PROBE_BW 或 STARTUP

代码位置：`src/congestion_control/xqc_bbr.c:675-751`。

### 1.4 ACK 到来时 BBR 怎么更新

ACK 主入口是 `xqc_bbr_on_ack`，代码位置 `src/congestion_control/xqc_bbr.c:1043-1075`。

一次 ACK 反馈会按这个顺序更新：

```text
xqc_bbr_on_ack
  -> xqc_bbr_update_bandwidth
  -> xqc_bbr_update_cycle_phase
  -> xqc_bbr_check_full_bw_reached
  -> xqc_bbr_check_drain
  -> xqc_bbr_update_min_rtt
  -> xqc_bbr_update_recovery_mode
  -> xqc_bbr_set_pacing_rate
  -> xqc_bbr_set_cwnd
```

每一步含义：

| 步骤 | 作用 |
| --- | --- |
| 更新带宽 | 用 ACK 样本计算当前 delivery rate，更新最大带宽滤波器 |
| 更新 PROBE_BW 周期 | 如果当前阶段持续够久或 inflight 到达目标，进入下一个 gain phase |
| 检查满带宽 | 如果连续多个 round 带宽没有明显增长，认为 STARTUP 完成 |
| 检查 DRAIN | 如果 DRAIN 阶段队列排到目标以下，进入 PROBE_BW |
| 更新 min RTT | 新 RTT 样本更小时刷新 min RTT，过期时进入 PROBE_RTT |
| 更新恢复状态 | 根据 loss/round 判断是否处于 recovery |
| 设置 pacing rate | `pacing_rate = bandwidth * pacing_gain` |
| 设置 cwnd | `cwnd` 向 `cwnd_gain * BDP` 靠拢，同时考虑 ACK 聚合和恢复期 |

### 1.5 丢包时 BBR 怎么处理

丢包入口是 `xqc_bbr_on_lost`，代码位置 `src/congestion_control/xqc_bbr.c:978-999`。

BBR 不像 CUBIC 那样简单按固定比例立刻砍窗口。它主要做两件事：

1. 如果 STARTUP 发生丢包，可能影响 STARTUP 的 pacing gain。
2. 保存当前 `prior_cwnd`，进入 recovery 相关逻辑，后续在 `xqc_bbr_modulate_cwnd_for_recovery` 中调节窗口。

真正的 recovery 窗口调节在 `src/congestion_control/xqc_bbr.c:794-852`。代码会根据 loss、acked、bytes_in_flight 等信息调整 `congestion_window`，但仍保留 BBR 的模型驱动思想。

### 1.6 BBR 在当前项目中的接入方式

BBR 函数表定义在 `src/congestion_control/xqc_bbr.c:1223-1235`：

| 回调 | 对应函数 |
| --- | --- |
| 初始化 | `xqc_bbr_init` |
| ACK 多包回调 | `xqc_bbr_on_ack` |
| 丢包回调 | `xqc_bbr_on_lost` |
| 获取窗口 | `xqc_bbr_get_cwnd` |
| 获取 pacing rate | `xqc_bbr_get_pacing_rate` |
| 获取带宽估计 | `xqc_bbr_get_bandwidth` |
| idle restart | `xqc_bbr_restart_from_idle` |

mini 配置中 `cc=bbr` 会映射到 `xqc_bbr_cb`：

- 客户端：`mini/mini_client.c:469-485`
- 服务端：`mini/mini_server.c:431-447`

### 1.7 BBR 的优缺点

优点：

- 不完全依赖丢包判断拥塞，适合无线链路、蜂窝网络这种非拥塞丢包较多的环境。
- 有 pacing rate，理论上发送更平滑。
- 能主动探测带宽。

局限：

- 多路径场景下，每条路径的 ACK、RTT、app-limited 状态会互相影响调度结果。
- 当前代码里 `minrtt` 调度器注释提到：对 BBR/BBRv2，单纯用 cwnd 判断可发送性并不完全准确，见 `src/transport/scheduler/xqc_scheduler_minrtt.c:90-94`。
- 如果路径带宽/RTT 波动很大，BBR 的带宽估计和 PROBE_BW 周期可能需要结合日志具体分析。

---

## 2. CUBIC 拥塞控制

### 2.1 算法目标

CUBIC 是典型 loss-based 拥塞控制算法。它把丢包视为拥塞信号，通过三次函数控制窗口增长。

核心思想：

```text
没有丢包：cwnd 按 CUBIC 曲线增长
发生丢包：cwnd 按 beta 乘性下降
接近历史最大窗口 Wmax：增长变慢
超过 Wmax 后：增长逐渐加快，继续探测更高带宽
```

代码依据：

- `src/congestion_control/xqc_cubic.c:4`：注明基于 RFC 8312
- `src/congestion_control/xqc_cubic.c:38-103`：CUBIC 窗口更新核心
- `src/congestion_control/xqc_cubic.c:150-177`：丢包降窗逻辑
- `src/congestion_control/xqc_cubic.c:182-211`：ACK 增窗逻辑

### 2.2 核心状态和参数

状态结构在 `src/congestion_control/xqc_cubic.h:12-26`：

| 字段 | 作用 |
| --- | --- |
| `cwnd` | 当前拥塞窗口，单位字节 |
| `tcp_cwnd` | Reno 友好窗口，用于保证和 Reno 的公平性 |
| `last_max_cwnd` | 上一次丢包前的最大窗口，即 Wmax |
| `ssthresh` | 慢启动阈值 |
| `bic_K` | CUBIC 曲线的时间偏移点 |
| `bic_origin_point` | CUBIC 曲线的原点窗口 |
| `epoch_start` | 当前 CUBIC epoch 起始时间 |
| `congestion_recovery_start_time` | 拥塞恢复开始时间 |

关键常量在 `src/congestion_control/xqc_cubic.c:11-23`：

| 常量 | 含义 |
| --- | --- |
| `XQC_CUBIC_BETA` | 降窗系数，约 0.7 |
| `XQC_CUBIC_C` | CUBIC 曲线参数，约 0.4 |
| `XQC_CUBIC_INIT_WIN` | 初始窗口，32 MSS |
| `XQC_CUBIC_MIN_WIN` | 最小窗口，4 MSS |
| `XQC_CUBIC_FAST_CONVERGENCE` | 快速收敛开关 |

### 2.3 ACK 到来时 CUBIC 怎么增长窗口

ACK 入口是 `xqc_cubic_on_ack`，代码位置 `src/congestion_control/xqc_cubic.c:182-211`。

流程：

```text
xqc_cubic_on_ack
  -> 如果 ACK 的包属于当前 recovery，直接返回，不增长窗口
  -> 如果 cwnd < ssthresh，处于慢启动：cwnd += acked_bytes
  -> 否则进入拥塞避免：
       1. Reno 友好窗口 tcp_cwnd 按线性方式增长
       2. 调用 xqc_cubic_update 计算 CUBIC 目标窗口
       3. cwnd 更新为 max(tcp_cwnd, cubic_target)
```

慢启动：

- 条件：`cwnd < ssthresh`
- 行为：每收到 ACK 按 ACK 字节数增加 `cwnd`
- 代码：`src/congestion_control/xqc_cubic.c:199-203`

拥塞避免：

- `tcp_cwnd_cnt` 累积 ACK 字节，达到 `tcp_cwnd` 后给 `tcp_cwnd` 加一个 MSS
- 调用 `xqc_cubic_update`
- 代码：`src/congestion_control/xqc_cubic.c:205-211`

### 2.4 CUBIC 曲线如何计算

核心函数是 `xqc_cubic_update`，代码位置 `src/congestion_control/xqc_cubic.c:38-103`。

关键计算过程：

1. 如果是当前 epoch 第一次更新，设置 `epoch_start = now`。
2. 判断当前 `cwnd` 是否已经超过 `last_max_cwnd`。
3. 如果超过，说明已经越过旧峰值，把当前 `cwnd` 作为新的 origin。
4. 如果没超过，用 `last_max_cwnd` 作为 origin，并计算 `K`。
5. 用当前时间与 `K` 的距离计算 CUBIC 三次函数偏移。
6. 得到 `bic_target`。
7. 限制最大增长速度，不超过每 RTT 1.5 倍。
8. 和 Reno 友好窗口 `tcp_cwnd` 取最大值。

可以用这个简化公式理解：

```text
W_cubic(t) = C * (t - K)^3 + Wmax
```

代码中的变量对应：

| 数学概念 | 代码变量 |
| --- | --- |
| Wmax | `last_max_cwnd` 或 `bic_origin_point` |
| K | `bic_K` |
| 当前时间偏移 | `t`、`offs` |
| C 参数 | `XQC_CUBIC_C` |
| 目标窗口 | `bic_target` |

### 2.5 丢包时 CUBIC 怎么降窗

丢包入口是 `xqc_cubic_on_lost`，代码位置 `src/congestion_control/xqc_cubic.c:150-177`。

流程：

```text
xqc_cubic_on_lost
  -> 如果这个 lost packet 已经属于 recovery 之前，不重复处理
  -> 记录 congestion_recovery_start_time
  -> 清空 epoch_start，让下一轮重新计算 CUBIC 曲线
  -> 更新 last_max_cwnd
  -> cwnd = cwnd * beta
  -> cwnd 不低于 min_cwnd
  -> tcp_cwnd = cwnd
  -> ssthresh = cwnd
```

关键点：

- `XQC_CUBIC_BETA = 718 / 1024`，约等于 0.7。
- 如果快速收敛开启，且当前窗口小于上次最大窗口，则 `last_max_cwnd` 会被进一步降低，让多个连接更公平地让出带宽。
- 降窗后 `ssthresh = cwnd`，后续 ACK 会从这个阈值开始重新增长。

### 2.6 CUBIC 在当前项目中的接入方式

CUBIC 函数表定义在 `src/congestion_control/xqc_cubic.c:251-260`：

| 回调 | 对应函数 |
| --- | --- |
| 初始化 | `xqc_cubic_init` |
| ACK 回调 | `xqc_cubic_on_ack` |
| 丢包回调 | `xqc_cubic_on_lost` |
| 获取窗口 | `xqc_cubic_get_cwnd` |
| 重置窗口 | `xqc_cubic_reset_cwnd` |
| 是否慢启动 | `xqc_cubic_in_slow_start` |
| 是否 recovery | `xqc_cubic_in_recovery` |

mini 配置中 `cc=cubic` 会映射到 `xqc_cubic_cb`：

- 客户端：`mini/mini_client.c:469-485`
- 服务端：`mini/mini_server.c:431-447`

### 2.7 CUBIC 的优缺点

优点：

- 实现直观、稳定，是常见公网拥塞控制算法。
- 丢包后明确降窗，容易从日志中解释行为。
- 与传统 TCP CUBIC 思路一致，报告时容易讲清楚。

局限：

- 对无线链路中非拥塞丢包比较敏感，容易误降窗。
- 不直接估计瓶颈带宽，链路快速变化时可能不如 BBR 灵活。
- 多路径聚合时，每条路径的 CUBIC 窗口独立变化，调度器如果只偏向低 RTT，可能导致某些路径窗口利用不足。

---

## 3. minrtt 多路径调度

### 3.1 算法目标

`minrtt` 的目标很直接：在所有可发送路径中，选择 SRTT 最小的路径。

它是一个低时延优先调度器，不是吞吐聚合优先调度器。

代码文件：

- `src/transport/scheduler/xqc_scheduler_minrtt.c`
- 函数表：`src/transport/scheduler/xqc_scheduler_minrtt.c:145-149`
- 选路入口：`xqc_minrtt_scheduler_get_path`，`src/transport/scheduler/xqc_scheduler_minrtt.c:24-143`

### 3.2 具体执行流程

`minrtt` 的选路过程：

```text
遍历 conn->conn_paths_list 中所有 path
  -> 读取 path_srtt、path_bw、loss_rate、pto_count 等日志信息
  -> 计算 path_class
  -> 跳过 inactive path
  -> 跳过 frozen path
  -> 如果是 reinject，跳过原 path
  -> 检查该 path 是否可发送
  -> 在每个 path_class 内保留 srtt 最小的 path
遍历 path_class
  -> 返回优先级最高 class 中的 best_path
```

对应代码：

| 逻辑 | 代码位置 |
| --- | --- |
| 遍历所有 path | `src/transport/scheduler/xqc_scheduler_minrtt.c:58-122` |
| 获取 SRTT | `src/transport/scheduler/xqc_scheduler_minrtt.c:61` |
| 获取 path class | `src/transport/scheduler/xqc_scheduler_minrtt.c:72` |
| 跳过 inactive/frozen/重注入原 path | `src/transport/scheduler/xqc_scheduler_minrtt.c:74-80` |
| 检查是否可发送 | `src/transport/scheduler/xqc_scheduler_minrtt.c:90-99` |
| 选择 class 内 SRTT 最小 path | `src/transport/scheduler/xqc_scheduler_minrtt.c:105-109` |
| 按 class 优先级返回 | `src/transport/scheduler/xqc_scheduler_minrtt.c:125-138` |

### 3.3 path_class 的作用

`minrtt` 不是简单从所有路径里找最低 RTT，而是先按 `path_class` 分类，再在每个 class 内找最小 RTT。

相关代码：

- `src/transport/scheduler/xqc_scheduler_minrtt.c:47-52`：初始化每个 class 的 `best_path`
- `src/transport/scheduler/xqc_scheduler_minrtt.c:72`：调用 `xqc_path_get_perf_class(path)`
- `src/transport/scheduler/xqc_scheduler_minrtt.c:125-138`：按 class 顺序返回第一个可用 best path

这意味着：

```text
高优先级 class 的路径，即使 RTT 稍高，也可能优先于低优先级 class。
同一 class 内才比较 SRTT。
```

### 3.4 可发送性检查

`minrtt` 调用 `xqc_scheduler_check_path_can_send` 判断路径是否能发：

- 代码位置：`src/transport/scheduler/xqc_scheduler_minrtt.c:95`
- 函数声明：`src/transport/scheduler/xqc_scheduler_common.h:7`
- 实现：`src/transport/scheduler/xqc_scheduler_common.c`

这个检查会考虑 path 状态、拥塞窗口、packet 大小等。如果所有路径都被 cwnd 阻塞，`cc_blocked` 可能被置为 true。

代码注释指出一个重要限制：`src/transport/scheduler/xqc_scheduler_minrtt.c:90-94` 提到，对 BBR/BBRv2，单纯用 cwnd 判断“一 RTT 内还能发多少”并不完全准确。

### 3.5 minrtt 的行为特征

优点：

- 简单、稳定、容易解释。
- 对低时延业务友好。
- 路径质量差异明显时，能快速偏向更好的路径。

局限：

- 容易长期偏向 RTT 最低路径，导致其他路径利用不足。
- 不直接考虑剩余 cwnd、inflight、路径带宽等吞吐因素。
- 对聚合传输来说，它更像低时延基线，不一定能获得最大吞吐。

适合报告中的一句话总结：

```text
minrtt 是低时延优先调度器：先过滤不可用路径，再在高优先级路径类别内选择 SRTT 最小的路径。
```

---

## 4. rap 多路径调度

### 4.1 算法目标

`rap` 当前实现可以理解为“低 RTT + 重注入避开原路径”的调度器。

与 `minrtt` 相比，`rap` 的实现更简单：

- 不使用 path_class 数组
- 在所有 active、非 frozen、可发送路径中找最小 SRTT
- 如果当前是 reinjection，默认避开发生原始发送的 path

代码文件：

- `src/transport/scheduler/xqc_scheduler_rap.c`
- 选路入口：`xqc_rap_scheduler_get_path`，`src/transport/scheduler/xqc_scheduler_rap.c:23-106`
- 函数表：`src/transport/scheduler/xqc_scheduler_rap.c:108-112`

### 4.2 具体执行流程

```text
遍历 conn->conn_paths_list 中所有 path
  -> 跳过非 ACTIVE path
  -> 跳过 FROZEN path
  -> 检查 path 是否可发送
  -> 如果是 reinject 且 path 是原 path，暂存 original_path 并跳过
  -> 读取 path_srtt
  -> 如果 path_srtt 更小，则更新 best_path
遍历结束
  -> 如果找到了 best_path，返回它
  -> 如果没找到，但 original_path 可用且不强制 diff path，则回退原 path
  -> 否则返回 NULL
```

对应代码：

| 逻辑 | 代码位置 |
| --- | --- |
| 遍历 path | `src/transport/scheduler/xqc_scheduler_rap.c:44-84` |
| 跳过非 active path | `src/transport/scheduler/xqc_scheduler_rap.c:47-49` |
| 跳过 frozen path | `src/transport/scheduler/xqc_scheduler_rap.c:51-54` |
| 检查可发送性 | `src/transport/scheduler/xqc_scheduler_rap.c:63-65` |
| 重注入时跳过原 path | `src/transport/scheduler/xqc_scheduler_rap.c:71-74` |
| 选最小 SRTT | `src/transport/scheduler/xqc_scheduler_rap.c:76-83` |
| fallback 到原 path | `src/transport/scheduler/xqc_scheduler_rap.c:86-98` |

### 4.3 reinject 逻辑

`rap` 对重注入的处理是它和普通 min RTT 选路的关键区别。

当 `reinject == true` 且当前候选 path 等于 `packet_out->po_path_id` 时：

```text
original_path = path
continue
```

也就是说，它先不考虑原路径，优先找另一条可发送且 RTT 更低的路径。

如果最后找不到其他 path：

- 若 packet 没有 `XQC_POF_REINJECT_DIFF_PATH` 标记，则允许回退原 path
- 若 packet 强制要求换 path，则返回 NULL

代码位置：`src/transport/scheduler/xqc_scheduler_rap.c:71-98`。

### 4.4 rap 的行为特征

优点：

- 实现简单，路径选择可解释。
- 重注入时倾向换路，有助于避开原路径上的拥塞或丢包。
- 比 `minrtt` 更直接，不受 path_class 优先级影响。

局限：

- 核心选择依据仍然是 SRTT，不直接考虑剩余窗口和吞吐。
- 如果 RTT 最小路径带宽小，可能聚合能力不足。
- 对普通新包，它基本就是全局 min RTT 策略。

适合报告中的一句话总结：

```text
rap 是重注入友好的低 RTT 调度器：普通发送选最小 SRTT，重注入时尽量避开原发送路径。
```

---

## 5. balanced 多路径调度

### 5.1 算法目标

`balanced` 的目标是比 `minrtt` 更适合聚合传输：它不只看 RTT，还看每条路径当前还剩多少发送空间。

它的核心评分可以理解为：

```text
score = 可用发送空间 / 有效 RTT * 利用率修正因子
```

代码文件：

- `src/transport/scheduler/xqc_scheduler_balanced.c`
- 选路入口：`xqc_balanced_scheduler_get_path`，`src/transport/scheduler/xqc_scheduler_balanced.c:164-281`
- 函数表：`src/transport/scheduler/xqc_scheduler_balanced.c:283-287`

### 5.2 核心状态

`balanced` 有一个很小的状态结构：

```text
has_last_path
last_path_id
pick_count
```

代码位置：`src/transport/scheduler/xqc_scheduler_balanced.c:7-11`。

作用：

- `last_path_id` 用来让下一次选择时从上次 path 的后一个候选开始扫描
- 避免多个 path 分数相同或接近时总是偏向数组中第一个 path
- `pick_count` 用于日志统计

### 5.3 可用发送空间 avail_bytes

`balanced` 先计算每条路径还能发多少：

```text
bytes_on_path = path_schedule_bytes + bytes_in_flight
avail_bytes = cwnd - bytes_on_path
```

代码位置：`src/transport/scheduler/xqc_scheduler_balanced.c:39-51`。

含义：

| 变量 | 含义 |
| --- | --- |
| `path_schedule_bytes` | 已经被调度到该路径但可能尚未真正发出的数据 |
| `ctl_bytes_in_flight` | 已发出但未 ACK 的在途数据 |
| `cwnd` | 拥塞控制给这条路径允许的窗口 |
| `avail_bytes` | 这条路径理论上还能继续承载的新数据 |

如果 `cwnd <= bytes_on_path`，该路径评分里的可用空间就是 0。

### 5.4 有效 RTT effective_srtt

`balanced` 不直接使用原始 SRTT，而是计算 `effective_srtt`：

代码位置：`src/transport/scheduler/xqc_scheduler_balanced.c:52-69`。

逻辑：

```text
srtt = xqc_send_ctl_get_srtt(ctl)
如果 srtt == 0，则用 initial_rtt
如果 min_rtt 有效，且 srtt > min_rtt * 4，则把 srtt 截断为 min_rtt * 4
最终返回 srtt，最小为 1
```

这个截断很重要。它避免某条路径因为瞬时 SRTT 异常偏大而被长期完全排除。

### 5.5 score 计算

评分函数是 `xqc_balanced_scheduler_compute_score`，代码位置 `src/transport/scheduler/xqc_scheduler_balanced.c:71-97`。

核心计算：

```text
utilization = bytes_on_path / cwnd
capacity = avail_bytes / effective_srtt
score = capacity * (2 - utilization)
```

代码里为了避免浮点，用 shift 和 scale 实现：

| 代码变量 | 含义 |
| --- | --- |
| `XQC_BALANCED_SCORE_SHIFT = 10` | 放大倍数，提升整数计算精度 |
| `XQC_BALANCED_UTIL_SCALE = 1024` | utilization 的 1.0 表示 |
| `XQC_BALANCED_UTIL_BOOST_CEIL = 1536` | utilization 上限，避免修正过大 |
| `capacity` | `avail_bytes / effective_srtt` 的放大版 |
| `score` | 最终路径分数 |

直观解释：

- `avail_bytes` 越大，路径越空，score 越高。
- `effective_srtt` 越小，路径反馈越快，score 越高。
- `utilization` 越低，说明路径还没被充分使用，修正因子越大。
- 如果路径已经很满，score 会降低。

所以 balanced 不会像 minrtt 那样只盯低 RTT 路径，它会主动给“还有窗口、还没用满”的路径机会。

### 5.6 候选路径分组

`balanced` 会把候选路径分成两组：

| 组 | path 状态 | 优先级 |
| --- | --- | --- |
| `available_paths` | `XQC_APP_PATH_STATUS_AVAILABLE` | 优先 |
| `standby_paths` | `XQC_APP_PATH_STATUS_STANDBY` | available 没有可用路径时才用 |

代码位置：

- 遍历 path：`src/transport/scheduler/xqc_scheduler_balanced.c:194-247`
- 放入 available：`src/transport/scheduler/xqc_scheduler_balanced.c:227-233`
- 放入 standby：`src/transport/scheduler/xqc_scheduler_balanced.c:235-241`

这意味着 balanced 不是无条件使用所有路径。它尊重应用层路径状态：

- `FROZEN` 直接跳过
- `AVAILABLE` 优先参与调度
- `STANDBY` 作为后备

### 5.7 选中 best path

真正选 path 的函数是 `xqc_balanced_scheduler_pick_path`，代码位置 `src/transport/scheduler/xqc_scheduler_balanced.c:99-162`。

选择规则：

```text
如果没有候选，返回 NULL
如果上次选过 path，则从上次 path 的下一个候选开始扫描
优先选择 score 最大的路径
如果 score 相同，选 SRTT 更小的路径
记录 last_path_id
返回 best path
```

主入口中先选 available，再选 standby：

```text
best_path = pick(available_paths)
如果 best_path == NULL:
    best_path = pick(standby_paths)
```

代码位置：`src/transport/scheduler/xqc_scheduler_balanced.c:252-260`。

### 5.8 重注入处理

和 `rap` 类似，balanced 也会在重注入时避开原路径：

- 如果 `reinject && packet_out->po_path_id == path->path_id`，把它暂存为 `original_path` 并跳过
- 如果最后没有其他路径，且 packet 没有强制 `XQC_POF_REINJECT_DIFF_PATH`，允许回退原路径

代码位置：

- 跳过原路径：`src/transport/scheduler/xqc_scheduler_balanced.c:218-221`
- 回退原路径：`src/transport/scheduler/xqc_scheduler_balanced.c:262-266`

### 5.9 balanced 的行为特征

优点：

- 同时考虑 RTT 和路径剩余窗口，比 minrtt 更适合聚合吞吐。
- 能避免单一路径被长期压满。
- 相同分数下用 SRTT 做 tie-breaker，仍保持低时延倾向。
- 有 last_path 轮转，减少固定顺序偏置。

局限：

- score 依赖 cwnd 和 inflight，若拥塞控制估计不准，调度也会受影响。
- 它不是严格按带宽比例分流，而是按“剩余空间 / RTT / 利用率”做启发式选择。
- 如果某条路径 RTT 略高但带宽很大，是否充分使用要看 score 结果和日志。

适合报告中的一句话总结：

```text
balanced 是容量/时延平衡调度器：优先 available 路径，用剩余 cwnd、inflight 和有效 SRTT 计算 score，选择最有发送余量且反馈较快的路径。
```

---

## 6. 五个算法对比

| 算法 | 层次 | 核心指标 | 决策方式 | 更适合 |
| --- | --- | --- | --- | --- |
| BBR | 拥塞控制 | 带宽、min RTT、BDP | 模型估计 + pacing/cwnd gain | 蜂窝、高带宽、非拥塞丢包场景 |
| CUBIC | 拥塞控制 | 丢包、cwnd、Wmax | 三次函数增窗 + beta 降窗 | 传统公网、丢包代表拥塞的场景 |
| minrtt | 调度器 | SRTT、path_class | 同一 class 中选最低 RTT | 低时延优先、基线对比 |
| rap | 调度器 | SRTT、是否重注入 | 选最低 RTT，重注入避开原 path | 重注入换路、低 RTT 优先 |
| balanced | 调度器 | avail_bytes、SRTT、utilization | score 最大者优先 | 多路径聚合吞吐、路径负载平衡 |

关键区别：

```text
BBR / CUBIC 决定每条路径能发多少。
minrtt / rap / balanced 决定当前包走哪条路径。
```

如果用户做聚合传输报告，建议这样组织：

1. 先讲拥塞控制：单条路径的发送能力如何形成。
2. 再讲多路径调度：多条路径之间如何选路。
3. 最后讲组合效果：BBR/CUBIC 影响每条 path 的 cwnd，balanced/minrtt/rap 再基于 path 状态做选择。

---

## 7. 日志和验证建议

### 7.1 代码定位命令

```powershell
cd C:\Users\DELL\Desktop\xquic_4.1

rg -n "xqc_bbr_on_ack|xqc_bbr_set_cwnd|xqc_bbr_set_pacing_rate|xqc_bbr_check_drain|xqc_bbr_update_min_rtt|xqc_bbr_cb" src\congestion_control\xqc_bbr.c
rg -n "xqc_cubic_update|xqc_cubic_on_ack|xqc_cubic_on_lost|xqc_cubic_cb" src\congestion_control\xqc_cubic.c
rg -n "xqc_minrtt_scheduler_get_path|xqc_rap_scheduler_get_path|xqc_balanced_scheduler_get_path|compute_score|available_bytes" src\transport\scheduler
```

### 7.2 运行日志建议关注字段

如果打开 XQUIC debug 日志，建议关注：

| 算法 | 日志关键词 |
| --- | --- |
| BBR | `BBR`、`bandwidth`、`pacing_rate`、`min_rtt`、`full_bw_reached`、`PROBE_RTT` |
| CUBIC | `cubic`、`cwnd`、`ssthresh`、`loss`、`recovery` |
| minrtt | `minrtt scheduler`、`path_srtt`、`path_bw`、`loss_rate`、`best_path` |
| rap | `path srtt`、`best path`、`original path`、`reinjected` |
| balanced | `balanced scheduler`、`score`、`avail`、`best path`、`pick_cnt` |

### 7.3 报告中的实验建议

如果要证明这些算法差异，建议固定其他变量，只改变一个变量：

1. 固定 `cc=bbr`，分别测 `mp_sched=minrtt/rap/balanced`。
2. 固定 `mp_sched=balanced`，分别测 `cc=bbr/cubic`。
3. 对每组记录完成时间、吞吐、双路径活跃情况、每条路径 RTT、丢包和重传。

预期现象：

| 组合 | 可能表现 |
| --- | --- |
| `cc=bbr + mp_sched=minrtt` | 低 RTT 路径使用多，聚合吞吐未必最高 |
| `cc=bbr + mp_sched=rap` | 重注入时更倾向换路，普通发送仍偏低 RTT |
| `cc=bbr + mp_sched=balanced` | 更可能同时利用多条路径 |
| `cc=cubic + mp_sched=balanced` | 丢包时窗口下降更明显，吞吐波动可能更大 |

---

## 8. 一句话总结

BBR 是“估计带宽和最小 RTT 后按模型发送”，CUBIC 是“按三次函数增窗、丢包后乘性降窗”。minrtt 和 rap 都偏低 RTT，rap 额外考虑重注入换路；balanced 则把剩余窗口、inflight、RTT 和路径利用率合成 score，更适合解释为面向聚合传输的平衡型调度器。

---

## 9. 脱离代码的算法概念总结

本节不再按代码函数展开，而是从报告和理解角度解释这些算法：它们依赖哪些参数、这些参数怎么计算出来、算法根据这些参数如何做决策。

### 9.1 先理解几类基础参数

这些算法反复使用几个基础网络参数。先理解这些参数，再看算法会更清楚。

| 参数 | 中文含义 | 怎么计算/获得 | 主要用途 |
| --- | --- | --- | --- |
| RTT | 往返时延 | 一个包发出到收到 ACK 的时间差 | 衡量路径响应速度 |
| SRTT | 平滑 RTT | 对多次 RTT 样本做平滑平均，降低偶然抖动影响 | 调度器判断哪条路径更快 |
| min RTT | 最小 RTT | 一段时间内观测到的最小 RTT，近似传播时延 | BBR 估算 BDP，balanced 限制异常 SRTT |
| bandwidth | 带宽估计 | 根据 ACK 返回速率估算单位时间交付了多少字节 | BBR 判断瓶颈带宽 |
| cwnd | 拥塞窗口 | 拥塞控制算法动态维护的允许在途数据上限 | 控制每条路径最多能同时发多少 |
| inflight | 在途数据量 | 已发送但尚未被 ACK 确认的数据量 | 判断路径是否已经被占满 |
| loss | 丢包信号 | 发送出去的包超时或被判定丢失 | CUBIC 降窗，BBR 进入恢复逻辑 |
| pacing rate | 发送速率 | 拥塞控制根据带宽估计和增益算出的发包速度 | 控制发包节奏，避免突发 |
| avail bytes | 可用发送空间 | `cwnd - 已占用窗口` | balanced 判断哪条路径还有余量 |
| utilization | 路径利用率 | `已占用窗口 / cwnd` | balanced 判断路径是否已经过载 |

这里最关键的区别是：

```text
RTT/SRTT/min RTT 主要描述路径时延。
bandwidth 描述路径交付能力。
cwnd/inflight/avail bytes 描述路径当前还能不能继续装数据。
loss 描述网络是否可能已经拥塞或质量变差。
```

### 9.2 BBR：基于带宽和时延模型的拥塞控制

BBR 的核心思想是：不要只等丢包才判断拥塞，而是主动估计“这条路径实际能承载多少数据”。

它主要调用/依赖这些参数：

| 参数 | BBR 怎么用 |
| --- | --- |
| bandwidth | 估计瓶颈带宽，即路径长期能稳定交付的最大速率 |
| min RTT | 估计路径传播时延，尽量排除排队时延 |
| BDP | `bandwidth * min RTT`，估计链路中刚好填满管道所需的数据量 |
| pacing gain | 发送速率增益，用于探测带宽或排空队列 |
| cwnd gain | 窗口增益，用于决定 cwnd 目标值 |
| ACK 样本 | 用于更新带宽估计、判断一轮传输是否结束 |
| loss | 用于恢复期调节，不是唯一拥塞依据 |

参数如何计算：

1. 每次收到 ACK，都知道这段时间内确认了多少字节。
2. 用 `确认字节数 / 时间间隔` 得到一次 delivery rate 样本。
3. 从近期样本中取较高值，作为 bandwidth 估计。
4. 从 RTT 样本中维护一个 min RTT，近似认为它是不排队时的基础时延。
5. 用 `bandwidth * min RTT` 得到 BDP。
6. 用 `BDP * cwnd_gain` 得到目标 cwnd。
7. 用 `bandwidth * pacing_gain` 得到 pacing rate。

BBR 的决策方式：

```text
如果还不知道链路容量：
    用较高 pacing gain 快速探测带宽。
如果发现带宽不再明显增长：
    降低发送速率，排空队列。
进入稳定阶段后：
    周期性稍微加速探测，再稍微减速排队，然后保持稳定。
如果 min RTT 长时间没刷新：
    暂时压低窗口，重新测一次基础 RTT。
```

直观比喻：

```text
BBR 像是在估计水管的直径和长度。
bandwidth 是水管每秒能流多少水。
min RTT 是水在水管里走一圈的基础时间。
BDP 是刚好填满水管需要多少水。
BBR 尽量让在途数据接近 BDP，而不是一直把队列塞爆。
```

BBR 的报告要点：

- 它是模型驱动，不是纯丢包驱动。
- 它最重要的两个参数是 bandwidth 和 min RTT。
- 它通过 pacing gain 主动探测，通过 cwnd gain 控制在途数据。
- 在蜂窝、无线、多路径场景中，非拥塞丢包较多，BBR 往往比纯丢包算法更有解释价值。

### 9.3 CUBIC：基于丢包反馈和三次函数的拥塞控制

CUBIC 的核心思想是：网络没丢包时逐步增大发送窗口；一旦丢包，就认为可能拥塞，立即降低窗口。这里最容易混淆的是“慢启动、拥塞避免、CUBIC 曲线增长”三者的关系。它们不会重叠冲突，因为当前项目代码里实际是一个互斥分支：

```text
慢启动：一个阶段
拥塞避免：另一个阶段
CUBIC 曲线增长：拥塞避免阶段里使用的增长公式
Reno tcp_cwnd：拥塞避免阶段里的保底/公平性参考线
```

当前项目的判断逻辑是：

```c
if (cubic->cwnd < cubic->ssthresh) {
    /* slow start */
    cubic->tcp_cwnd += acked_bytes;
    cubic->cwnd += acked_bytes;
} else {
    /* congestion avoidance */
    cubic->tcp_cwnd_cnt += acked_bytes;
    if (cubic->tcp_cwnd_cnt >= cubic->tcp_cwnd) {
        cubic->tcp_cwnd += XQC_CUBIC_MSS;
        cubic->tcp_cwnd_cnt = 0;
    }
    xqc_cubic_update(cong_ctl, acked_bytes, now);
}
```

也就是说：

```text
慢启动 和 拥塞避免 是两个互斥阶段。
CUBIC 曲线增长 是拥塞避免阶段里的具体增长公式。
```

它主要调用/依赖这些参数：

| 参数 | CUBIC 怎么用 |
| --- | --- |
| `cwnd` | 当前拥塞窗口，是 CUBIC 直接调节的核心对象 |
| `Wmax / last_max_cwnd` | 上次丢包前达到的最大窗口 |
| `beta` | 丢包后的乘性降窗比例，当前约 0.7 |
| `C` | 三次函数增长曲线参数，当前约 0.4 |
| `ssthresh` | 慢启动阈值，决定慢启动和拥塞避免分界 |
| `tcp_cwnd` | Reno 友好窗口，拥塞避免阶段作为保底参考 |
| `ACK 字节数` | 没丢包时用于增加 `cwnd` 或推进 `tcp_cwnd` |
| `loss` | 触发降窗和重新开始一轮 CUBIC 增长 |
| `min RTT` | 参与 CUBIC 曲线时间修正 |

初始化时，当前项目会设置：

```text
cwnd = init_cwnd
tcp_cwnd = init_cwnd
last_max_cwnd = init_cwnd
ssthresh = XQC_CUBIC_MAX_SSTHRESH
```

因为 `ssthresh` 初始非常大，所以一开始一般满足：

```text
cwnd < ssthresh
```

因此先进入慢启动。慢启动阶段非常简单：每收到 ACK，就按确认的数据量增加窗口。

```text
cwnd += acked_bytes
tcp_cwnd += acked_bytes
```

这不是 CUBIC 曲线。慢启动的目标是快速把窗口拉起来，尽快探到链路容量。

当发生丢包时，CUBIC 认为网络可能拥塞，于是进入降窗逻辑：

```text
last_max_cwnd = 当前 cwnd
cwnd = cwnd * beta
ssthresh = cwnd
epoch_start = 0
```

当前项目里 `beta = 718 / 1024`，约等于 0.7。假设丢包前窗口是 100，那么丢包后大致变成：

```text
last_max_cwnd = 100
cwnd = 70
ssthresh = 70
```

之后再收到 ACK 时，判断：

```text
cwnd < ssthresh ?
70 < 70  不成立
```

所以不会回到慢启动，而是进入拥塞避免。拥塞避免阶段才会调用 `xqc_cubic_update()`，也就是 CUBIC 曲线增长。

CUBIC 曲线的理论公式是：

```text
W_cubic(t) = C * (t - K)^3 + Wmax
```

含义是：

| 数学概念 | 当前项目中的代码变量 |
| --- | --- |
| `Wmax` | `last_max_cwnd` 或 `bic_origin_point` |
| `t` | 距离上次降窗过去的时间 |
| `K` | 从降窗后的窗口重新增长到 `Wmax` 大概需要的时间点 |
| `C` | `XQC_CUBIC_C` |
| 目标窗口 | `bic_target` |

这条曲线的设计目的很关键：

```text
刚丢包后：窗口从 0.7 * Wmax 开始恢复。
接近 Wmax 时：增长变慢，小心探测。
超过 Wmax 后：继续加速增长，探索是否有更多带宽。
```

直观图：

```text
cwnd
 ^
 |                         超过旧峰值后，继续探测新带宽
 |                              /
 |                            /
 |                         _/
 |                    ___/
 |               ____/
 |          ____/
 |____ ____/  接近 Wmax 时变慢
 |
 +--------------------------------> time
        降窗后      K点附近
```

当前项目在 CUBIC 里还维护了一个 `tcp_cwnd`。它不是另一个阶段，而是 Reno 友好窗口，用来保证 CUBIC 在某些区域不要比传统 Reno 更差。拥塞避免阶段里，代码会先线性推进 `tcp_cwnd`，然后取：

```text
最终目标窗口 = max(CUBIC 曲线算出来的窗口, Reno 友好窗口 tcp_cwnd)
```

所以 `tcp_cwnd` 是 CUBIC 的辅助参考，不是和 CUBIC 曲线冲突的另一套主算法。

完整状态流可以按这个顺序理解：

```text
1. 初始化
   cwnd = init_cwnd
   ssthresh = 很大
   last_max_cwnd = init_cwnd

2. 慢启动
   条件：cwnd < ssthresh
   行为：ACK 来了就 cwnd += acked_bytes
   目的：快速探测带宽

3. 发生丢包
   last_max_cwnd = 当前 cwnd
   cwnd = cwnd * 0.7
   ssthresh = cwnd
   epoch_start = 0
   目的：承认拥塞，降低发送压力

4. 拥塞避免
   条件：cwnd >= ssthresh
   行为：
     维护 Reno 友好窗口 tcp_cwnd
     调用 xqc_cubic_update 计算 CUBIC 曲线目标窗口
     cwnd = max(CUBIC目标, Reno友好窗口)

5. 下一次丢包
   重新记录 Wmax
   再降窗
   再进入下一轮 CUBIC epoch
```

CUBIC 的决策方式：

```text
如果还在慢启动：
    快速增大 cwnd，不使用 CUBIC 曲线。
如果已经丢过包并进入拥塞避免：
    用 CUBIC 三次函数围绕 Wmax 增长。
如果接近上次丢包前的最大窗口：
    增长变慢，谨慎试探。
如果超过历史最大窗口：
    增长逐渐加快，继续探测更高容量。
如果发生丢包：
    认为网络可能拥塞，把窗口乘以 beta 降下来，并设置新的 ssthresh。
```

直观比喻：

```text
CUBIC 像是在试探一个天花板。
慢启动阶段是快速冲上去找天花板。
丢包说明撞到了天花板，于是窗口退回到约 70%。
之后进入拥塞避免，不再猛冲，而是围绕上次撞到的位置 Wmax 小心恢复。
接近 Wmax 时放慢，超过 Wmax 后继续探测新的天花板。
```

CUBIC 的报告要点：

- 它是 loss-based 算法，丢包是核心反馈。
- 慢启动和拥塞避免互斥，不会同时执行。
- CUBIC 曲线增长只发生在拥塞避免阶段。
- `tcp_cwnd` 是 Reno 友好保底参考，不是第三个并行增长阶段。
- 它会记住上次丢包前的最大窗口，在那个位置附近更谨慎。
- 在无线链路中，如果丢包不一定代表拥塞，CUBIC 可能过早降窗。

### 9.4 minrtt：最低平滑 RTT 优先的路径调度

minrtt 是多路径调度算法，不决定每条路径能发多少，而是决定当前这个包应该走哪条路径。

它主要调用/依赖这些参数：

| 参数 | minrtt 怎么用 |
| --- | --- |
| path_state | 判断路径是否 active |
| app_path_status | 判断路径是否 frozen、available、standby |
| SRTT | 核心选择指标，越小越优先 |
| path_class | 路径性能类别，先按类别优先级筛选 |
| cwnd / inflight | 判断该路径当前是否还能发送 |
| reinject 标记 | 重注入时避免继续走原路径 |

参数如何计算：

1. 每条路径都有独立的 RTT 样本。
2. RTT 样本被平滑成 SRTT。
3. 路径还会根据状态、质量、应用设置得到 path_class。
4. 拥塞控制维护每条路径的 cwnd。
5. 已发送未确认的数据形成 inflight。
6. 如果 `inflight` 已经接近或超过 `cwnd`，该路径暂时不可发送。

minrtt 的决策方式：

```text
遍历所有路径。
过滤掉 inactive / frozen / 当前不能发送的路径。
如果是重注入，跳过原路径。
按 path_class 分组。
在每个 path_class 内选择 SRTT 最小的路径。
最后按 path_class 优先级返回最合适的路径。
```

直观理解：

```text
minrtt 认为低延迟路径更好。
如果两条路径都能发，它更倾向把包交给 SRTT 更小的路径。
```

minrtt 的报告要点：

- 它是低时延优先，不是吞吐聚合优先。
- 它依赖 SRTT，但不是完全只看 SRTT，还会先过滤路径状态和发送能力。
- 它容易偏向最快路径，可能导致高带宽但 RTT 稍高的路径利用不足。

### 9.5 rap：低 RTT + 重注入换路的路径调度

rap 也是路径调度算法。它和 minrtt 类似，都偏向低 RTT；区别是 rap 对重注入场景更直接。

它主要调用/依赖这些参数：

| 参数 | rap 怎么用 |
| --- | --- |
| path_state | 只考虑 active 路径 |
| app_path_status | 跳过 frozen 路径 |
| SRTT | 选择最低 SRTT 路径 |
| cwnd / inflight | 判断路径是否能发送 |
| reinject 标记 | 重注入时尽量避开原路径 |
| original path | 如果没有其他路径可用，可能回退原路径 |

参数如何计算：

1. SRTT 由路径 RTT 样本平滑得到。
2. 可发送性由该路径的 cwnd、inflight、包大小和路径状态共同决定。
3. original path 来自这个包上一次发送时使用的路径 ID。
4. reinject 表示这个包是重传/重注入包，而不是普通新包。

rap 的决策方式：

```text
遍历所有路径。
过滤掉 inactive / frozen / 当前不能发送的路径。
如果是重注入，并且候选路径就是原路径：
    暂时跳过原路径。
在剩余路径中选择 SRTT 最小的路径。
如果没有其他路径可用：
    在允许的情况下回退原路径。
如果强制要求换路但没有其他路径：
    不选择路径。
```

直观理解：

```text
rap 的普通发送逻辑接近“全局最低 RTT”。
但如果一个包需要重注入，它会优先尝试换到另一条路径。
这样做的原因是：原路径上已经出现未确认或丢失迹象，换路可能更快恢复。
```

rap 的报告要点：

- 它仍然是低 RTT 优先算法。
- 它的特色是重注入时避开原路径。
- 它不显式计算带宽比例，也不主动平衡多路径负载。
- 如果报告中讲聚合吞吐，rap 更适合作为“低 RTT + 故障换路”的对比算法。

### 9.6 balanced：容量和时延综合评分的路径调度

balanced 是更偏聚合传输的调度算法。它不只问“哪条路径 RTT 最低”，还问“哪条路径现在还有多少发送空间”。

它主要调用/依赖这些参数：

| 参数 | balanced 怎么用 |
| --- | --- |
| cwnd | 每条路径的拥塞窗口上限 |
| inflight | 每条路径已经发出但未 ACK 的数据 |
| scheduled bytes | 已经调度到该路径、但还在发送队列中的数据 |
| avail bytes | `cwnd - inflight - scheduled bytes`，表示路径还剩多少空间 |
| SRTT | 衡量路径反馈速度 |
| min RTT | 用于限制异常偏大的 SRTT，得到 effective SRTT |
| utilization | 路径利用率，表示窗口已经被占用了多少 |
| app_path_status | 区分 available 和 standby |
| reinject 标记 | 重注入时尽量避开原路径 |

参数如何计算：

1. 拥塞控制算法为每条路径维护 `cwnd`。
2. 发出去但未 ACK 的数据形成 `inflight`。
3. 已经放入路径发送缓冲但未真正发完的数据形成 scheduled bytes。
4. `avail bytes = cwnd - inflight - scheduled bytes`。
5. 每条路径 RTT 样本被平滑成 SRTT。
6. 如果 SRTT 异常高，balanced 用 min RTT 的若干倍做上限，得到 effective SRTT。
7. `utilization = 已占用窗口 / cwnd`。
8. 最后计算路径 score。

balanced 的评分可以概括为：

```text
score 越大，路径越值得选。
score 大致正比于 avail bytes。
score 大致反比于 effective SRTT。
路径利用率越低，越容易被加分。
```

也可以用更口语化的方式理解：

```text
balanced 喜欢选择“还有窗口、没被塞满、反馈又比较快”的路径。
```

balanced 的决策方式：

```text
遍历所有路径。
过滤掉 inactive / frozen / 当前不能发送的路径。
如果是重注入，先避开原路径。
计算每条路径的 avail bytes、effective SRTT、utilization。
根据这些值计算 score。
先在 available 路径中选 score 最大的。
如果 available 没有可选路径，再考虑 standby 路径。
如果 score 相同，选择 SRTT 更低的路径。
如果没有其他路径且允许回退，重注入包可回退原路径。
```

直观理解：

```text
minrtt/rap 更像“谁快走谁”。
balanced 更像“谁快、谁空、谁还能装更多数据，就走谁”。
```

balanced 的报告要点：

- 它同时考虑时延和容量。
- 它用 cwnd/inflight/scheduled bytes 判断路径剩余空间。
- 它用 SRTT/effective SRTT 避免选择反馈太慢的路径。
- 它比 minrtt 更适合解释多路径聚合传输。
- 它不是严格带宽比例分流，而是启发式评分选择。

### 9.7 五个算法放在一起怎么理解

可以把这些算法分成两层理解。

第一层是拥塞控制，回答“每条路径能发多少”：

| 算法 | 一句话原理 | 关键参数 |
| --- | --- | --- |
| BBR | 根据带宽和 min RTT 建模，估计 BDP，再控制 pacing rate 和 cwnd | bandwidth、min RTT、BDP、pacing gain、cwnd gain |
| CUBIC | 没丢包按三次函数增大 cwnd，丢包后按 beta 乘性降低 cwnd | cwnd、Wmax、beta、C、ssthresh、loss |

第二层是路径调度，回答“当前包走哪条路径”：

| 算法 | 一句话原理 | 关键参数 |
| --- | --- | --- |
| minrtt | 在可发送路径中选 SRTT 最小者，且尊重 path_class 优先级 | SRTT、path_class、path_state、cwnd/inflight |
| rap | 普通发送选低 SRTT，重注入时尽量避开原路径 | SRTT、reinject、original path、path_state、cwnd/inflight |
| balanced | 用剩余窗口、SRTT 和利用率计算 score，选择综合最优路径 | cwnd、inflight、scheduled bytes、avail bytes、SRTT、utilization |

最终组合效果可以这样描述：

```text
BBR/CUBIC 先为每条路径形成 cwnd、pacing、带宽或丢包状态。
minrtt/rap/balanced 再根据这些路径状态选择当前包走哪条路径。
如果拥塞控制给某条路径的 cwnd 很小，调度器即使想选它，也可能因为不可发送而跳过。
如果调度器长期不选某条路径，那条路径的带宽估计和窗口利用也可能受到影响。
```

针对报告中的准确概念，可以这样总结：

```text
BBR 是模型派：看带宽和最小时延，估算链路容量。
CUBIC 是丢包派：窗口逐步增长，丢包后按比例收缩。
minrtt 是低时延派：优先走平滑 RTT 最小的路径。
rap 是换路派：低时延优先，重注入时尽量换路径。
balanced 是平衡派：综合路径剩余容量、时延和利用率，尽量让多路径都发挥作用。
```
