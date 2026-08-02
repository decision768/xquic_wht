# Mini API 阶段 8：兼容性与性能验收

## 1. 必测矩阵

目标机必须完成以下 240 个用例：

```text
4 个 client/server 组合
× 2 个方向（GET、POST）
× 3 种网络（5G1、5G2、Agg）
× 2 个文件（200 MiB、1 GiB）
× 5 轮
= 240
```

组合标识：

| 标识 | client | server |
|---|---|---|
| `mini-mini` | 原 mini_client | 原 mini_server |
| `api-api` | API demo client | API demo server |
| `api-mini` | API demo client | 原 mini_server |
| `mini-api` | 原 mini_client | API demo server |

每一轮都必须保存完整 client/server 日志，不只保存最终吞吐数字。

## 2. 测试文件

在 client 和 server 的测试目录执行：

```bash
python3 scripts/mini_api/generate_test_files.py /data/mini-api-test
```

脚本生成 0、1、4 MiB、200 MiB、1 GiB 和略大于 1 GiB 的确定性文件，并输出
`sha256-manifest.json`。阶段 8 使用 200 MiB 和 1 GiB；其余文件供阶段 5/9 边界与故障测试。

## 3. 单轮采集

先按组合启动相应 server，再用 `run_target_case.py` 包装 client 命令。示例：

```bash
python3 scripts/mini_api/run_target_case.py \
  --case-id api-api_GET_Agg_200m_r1 \
  --combo api-api \
  --client-kind api \
  --direction GET \
  --network Agg \
  --size-bytes 209715200 \
  --round 1 \
  --expected-paths 2 \
  --expected-file /data/mini-api-test/payload-200m.bin \
  --actual-file /data/mini-api-results/download-200m.bin \
  --output-dir /data/mini-api-results \
  --timeout-seconds 1800 \
  -- ./build/mini/netins_udp_mini_api_client \
       --get --host 10.12.54.143 --port 8443 \
       --interface wwan0 --interface wwan1 \
       --remote /payload-200m.bin \
       --local /data/mini-api-results/download-200m.bin
```

POST 或跨机器用例必须在 client 命令返回前把 server 端结果复制到 `--actual-file` 指定位置；
可将 `bash -lc 'client-command && scp ...'` 作为 `--` 后的命令。禁止只比较文件长度。

每个 JSON 结果同时要求：

- client 返回 0 且未超时；
- 能解析吞吐；
- SHA-256 一致；
- 5G1/5G2 显示 1 条路径，Agg 显示 2 条路径；
- 日志实际出现 `active_paths=1 per_path_limit=256` 或
  `active_paths=2 per_path_limit=256`；
- 如果目标机存在 GNU `/usr/bin/time`，记录最大 RSS、user CPU 和 system CPU。

原 mini 的方法、文件路径和网卡由 `mini/client_config.txt` 配置；每轮必须把实际配置副本保存在
结果目录。原 mini 继续使用 `get_stream_cnt=8`、`post_stream_cnt=4`，API 不暴露该配置。

## 4. 汇总与硬门槛

240 个 JSON 全部生成后执行：

```bash
python3 scripts/mini_api/summarize_matrix.py /data/mini-api-results
```

如 205 Mbps 是正式指标：

```bash
python3 scripts/mini_api/summarize_matrix.py \
  /data/mini-api-results --minimum-agg-mbps 205
```

汇总器将缺少任何一轮视为失败，并检查：

```text
每个含 API 的组合平均吞吐 / mini-mini 平均吞吐 >= 95%
Agg > max(5G1, 5G2)
Agg / (5G1 + 5G2) >= 80%
可选：Agg >= 205 Mbps
所有文件 SHA-256 一致
单/双路径 256/256 预算和预期路径数一致
```

CPU/RSS 没有跨平台通用的固定阈值，因此必须同时对照相同方向、网络和文件大小的
`mini-mini` 五轮数据；明显增长需记录原因并重新测试。

## 5. 当前状态

仓库侧已完成脚本语法、自测数据生成和日志解析测试。当前 Windows 工作区不具备
Linux x86_64/SIM8810、TLS 证书、5G1/5G2 及可控链路，240 轮真实结果状态为：

```text
TARGET_ENV_PENDING
```

没有生成伪造的性能数字或 PASS 报告。
