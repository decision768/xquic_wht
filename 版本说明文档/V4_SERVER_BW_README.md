# v4-server-bw 说明

## 目标

v4 基于 v3-io-opt 创建，保留前面已经完成的客户端 RX 修复、有界队列、异步写文件、服务端 GET `pread` 诊断等改动。

本版本只聚焦一个问题：让服务端 GET 发送端可以可靠使用 `bw` 调度器，作为 `minrtt`、`rap`、`balanced` 之外的聚合测试方案。

## 主要改动

1. 修复 `src/transport/scheduler/xqc_scheduler_bandwidth.c`

   v3 中该文件有几处乱码注释破坏了真实代码，导致 `bw` 调度器不适合作为测试对象。v4 已将该文件替换为干净版本，恢复：

   - `XQC_BW_CAP_SHIFT`
   - `available_cwnd`
   - `available_score`
   - `xqc_bandwidth_scheduler_compute_scores(...)`
   - 基于 `cwnd / srtt` 的路径容量估算
   - 基于目标 inflight deficit 的路径选择

2. 显式配置服务端使用 `bw`

   文件：`mini/server_config.txt`

   新增：

   ```text
   mp_sched=bw
   ```

3. 保留服务端调度确认日志

   启动服务端后应看到：

   ```text
   [diag_server_sched] requested=bw resolved=bw
   ```

   如果没有看到这行，不应认为本轮测试已经使用了 `bw`。

## bw 调度器的基本含义

`bw` 不是直接读取网卡测速结果，而是根据 QUIC 内部状态估算每条 path 的发送承载能力：

```text
cwnd
inflight
srtt
path_schedule_bytes
available_bytes
```

它会优先选择当前更需要被填充、并且还有发送空间的 path。相比 `minrtt` 和 `rap`，它更适合验证“是否能把两条链路的带宽叠加起来”。

## 推荐测试方式

GET 下载时，主要发送端是服务端，因此优先固定客户端，切换服务端：

```text
客户端：cc=bbr, mp_sched=balanced
服务端：cc=bbr, mp_sched=bw
```

对照组建议：

```text
server mp_sched=minrtt
server mp_sched=rap
server mp_sched=balanced
server mp_sched=bw
```

关键指标：

```text
Agg 是否超过 max(5G1, 5G2)
Agg / (5G1 + 5G2) 是否接近或超过 80%
path0/path1 send_bytes 是否都增长
cwnd_blocked 是否下降
客户端 RX 队列是否仍然稳定
文件校验和是否正确
```

## 编译注意

v4 目录没有保留从 v3 复制来的旧 `build` 缓存。请在 Linux 侧重新生成构建目录，例如：

```bash
mkdir -p build
cd build
cmake ..
make -j
```

如果你的工程原本使用特定交叉编译工具链或 CMake 参数，请沿用原来的构建命令。
