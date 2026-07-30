# Mini API 阶段0：冻结基线

## 1. 冻结结论

| 项目 | 冻结值 |
|---|---|
| 工作区 | `D:\5g\xquic-1.8.3-v5.3.3-path-delivery-diag` |
| 原始分支 | `v5.4.2_final` |
| 原始提交 | `0a6a941b6bd7cd0d6909f9b037cd2cf7802605c3` |
| 提交标题 | `test sdk_agg` |
| 相对远端 | 比 `origin/v5.4.2_final` 领先1个提交 |
| 开发分支 | `codex/mini-api-v5.4.2` |
| 冻结时工作区 | 干净 |

开发分支从上述精确提交创建。后续 Mini API 工作不得直接修改
`v5.4.2_final`。

`0a6a941b` 相对父提交只修改以下文件：

- `mini/mini_server.h`
- `mini/mini_server_cb.c`
- `mini/mini_server_cb.h`

改动内容是服务器接收聚合、发送拆分的内部耗时统计，不改变 XQUIC
公共 ABI。这些统计属于 Mini API 必须保留的基线。

## 2. 配置基线

### 2.1 客户端

配置文件：`mini/client_config.txt`

```text
server_addr=10.12.54.143
server_port=8443
method=POST
stream_cnt=1
cc=bbr
mp_sched=balanced
mp_diag=1
mp_diag_interval_ms=250
interfaces=eth0
kernel_sndbuf=16777216
kernel_revbuf=16777216
user_send_buf_size=8192
user_recv_buf_size=65536
```

SHA-256：

```text
fb02b1366136aae3b247749674de36a4a0f1e009fe03c3b69a333e092eebffd7
```

### 2.2 服务端

配置文件：`mini/server_config.txt`

```text
listen_addr=0.0.0.0
listen_port=8443
data_dir=.
default_send_file=hhh.bin
cc=cubic
mp_sched=rap
kernel_sndbuf=16777216
kernel_revbuf=16777216
user_send_buf_size=32768
user_recv_buf_size=32768
upload_path=./upload/ccc.bin
use_zlog=0
```

SHA-256：

```text
d79968b73f25c0a713d68f358253a20ef280828b06d9181898a15ca1f7b2d184
```

配置文件名是相对当前工作目录解析的。部署测试时必须把
`client_config.txt`、`server_config.txt` 与对应可执行文件放在同一运行目录，
或在包含配置文件的目录启动程序。

## 3. `stream_cnt` 冻结结论

当前源码默认值和当前配置文件值均为1：

- `mini/mini_client.c:887`：`args->req_stream_cnt = 1`
- `mini/client_config.txt:6`：`stream_cnt=1`
- `mini/mini_client.c:2337-2344`：配置文件可以覆盖，并限制到最多16
- `mini/mini_client.c:3389-3403`：命令行解析代码也可以覆盖

当前 `mini_client_main.c` 没有调用命令行解析函数，因此当前可执行程序实际以
配置文件为准。

POST 的总 request 数由文件大小除以4 MiB计算；`stream_cnt` 控制同时活跃的
request 数。GET 先发1个 request 获取 `x-total-length`，随后仍以
`stream_cnt` 作为最大并发数。

因此当前仓库的真实默认行为是：

```text
POST最大并发request数 = 1
GET最大并发request数  = 1
```

仓库中的历史 `日志/V5.4.1/client 1.txt` 出现过
`launch requests, max concurrency=8`，说明历史 GET 性能测试曾使用8并发，
但该日志属于更早的 RX=64 版本，且对应的外部配置没有保存在当前仓库，不能
作为 `0a6a941b` 的可复现基线。当前仓库尚无 POST=4 的可核验证据。

Mini API 固定 POST=4、GET=8 是待实现的 API 策略，不是现有 mini 默认值。
为了保证回归可比较：

- 原 mini 在第一轮抽取中继续保持 `stream_cnt` 可配置；
- 第三方公共 API 不暴露 `stream_cnt`；
- Mini API 的4/8默认值在传输任务层落地；
- 性能对比时原 mini 和 API 必须显式记录各自的实际并发值。

## 4. 不可改变的关键常量与调用链

| 项目 | 值 | 代码位置 |
|---|---:|---|
| 逻辑分片 | 4 MiB | `mini/mini_client.c:11` |
| 客户端 RX 批量缓冲 | 256包/路径 | `mini/mini_client.c:16` |
| 单路径 RX 上限 | 40包/轮 | `mini/mini_client.c:17` |
| 双路径 RX 上限 | 256包/路径/轮 | `mini/mini_client.c:18` |
| 最大路径数 | 2 | `mini/mini_client.h:50` |
| 最大同时 request 数 | 16 | `mini/mini_client.h:51` |
| 服务端 socket 每事件读取上限 | 32包 | `mini/mini_server.h:25` |
| XQUIC 内部处理批次 | 100包 | `src/transport/xqc_conn.h:37` |

客户端公平直接 RX 调用链：

```text
xqc_mini_cli_socket_event_callback()
  -> xqc_mini_cli_socket_read_handler()
  -> xqc_mini_cli_run_rx_arbiter()
  -> xqc_mini_cli_recv_path_batch()
  -> xqc_mini_cli_process_rx_packet()
  -> xqc_engine_finish_recv()
```

动态预算的实现位于 `mini/mini_client.c:2128-2229`。其中：

- 活动接收路径少于2条时，每路径上限40；
- 活动接收路径达到2条时，每路径上限256；
- 每轮轮换首个处理路径；
- 本轮收到包后只在公平轮次末显式调用一次 `xqc_engine_finish_recv()`；
- `[diag_rx_budget]` 在模式或预算变化时输出。

这些函数必须直接进入共享 backend，不能在 API adapter 中复制一份相似实现。

## 5. 关键源文件校验值

以下 SHA-256 用于确认性能复测所用源码仍是冻结基线：

```text
9b1a8090c7afdf6381d938a2d14e3a4bb3f729a84a52c6a903f58934dcdfba18  mini/mini_client.c
5425576b0b4608ca86a778cf5abf65b576a6316266568cc190271f3db9f42578  mini/mini_client.h
e0d07e830b642b3a0bec185c0621d9785929402e35bd7480de3c649623eae5e5  mini/mini_client_cb.c
254ddc9a563cf45841afcac14055aeb92ec006839e99f049e212e82524c02ead  mini/mini_client_main.c
c7f7ba5c5a174a560d32fcad0cd2e8a4d77e148b8f6d0d215cff808c39ede5c8  mini/mini_server.c
b11cce2cfe9fa5481bf090c311d17b2d2967b83dca0f9ae2e38982d61dc18edf  mini/mini_server.h
75a9bd77b46b38a3ad6340c3582e84ad65f64f63e8c1f9d785e48e0a5c2d488a  mini/mini_server_cb.c
d139b60fc6a1ebd6efe00ce0efe65700c75f46c5d2400d8a93d6296466ff102c  mini/mini_server_cb.h
e35ab6aeb53a43cefa8492b8b63ae98d0f6f8d482349b31d968b39bcdc631785  mini/mini_server_main.c
703d83d4b6861d585e9ccd7ef0265c03dd70131e1b883fa4aa49f9493434fbb8  mini/CMakeLists.txt
```

## 6. 固定测试文件

使用 `scripts/prepare_mini_api_baseline_files.py` 生成确定性内容、精确长度的文件：

```bash
python3 scripts/prepare_mini_api_baseline_files.py \
  --output-dir build/mini-api-baseline
```

固定文件：

| 文件 | 精确字节数 | SHA-256 |
|---|---:|---|
| `mini-api-200MiB.bin` | 209715200 | `db0e6cbbe829827e344133442cb0a34e30cc815880f6f6ffae06721ea703d4fd` |
| `mini-api-1GiB.bin` | 1073741824 | `e18e3f358b46eae9266ac36a5ff6347f6bf09711dff389597f237d5fe83111d8` |

现有 `mini/get1GBfile.py` 不作为冻结生成器：它按文本块循环写入，最后一次写入
可能越过目标值，不能保证文件恰好为1 GiB。

## 7. 基线构建命令

目标 Linux 环境应从全新目录配置，避免旧 `CMakeCache.txt` 中的绝对路径：

```bash
cmake -S . -B build-baseline \
  -DXQC_ENABLE_TESTING=ON \
  -DSSL_TYPE=babassl \
  -DSSL_PATH=/path/to/babassl
cmake --build build-baseline -j
```

当前 CMake 还有两组目标机硬编码依赖：

```text
LIBEVENT_ROOT=/home/admin123/libevent_aarch64/libevent-2.1.12-stable
ZLOG_INCLUDE_DIR=/home/admin123/zlog-1.2.18/src
ZLOG_STATIC_LIB=/home/admin123/zlog-1.2.18/build_aarch64/lib/libzlog.a
```

因此原样基线构建必须在具备这些路径的 SIM8810/AArch64 环境执行。后续 CMake
阶段需要将其改为可配置的 cache 变量，但阶段0不改动它们。

## 8. 基线运行与取证

准备运行目录：

```bash
mkdir -p build-baseline/mini/upload build-baseline/mini/download
cp mini/client_config.txt build-baseline/mini/
cp mini/server_config.txt build-baseline/mini/
cp /secure/path/server.crt /secure/path/server.key build-baseline/mini/
```

服务端：

```bash
cd build-baseline/mini
./mini_server 2>&1 | tee baseline-server.log
```

客户端：

```bash
cd build-baseline/mini
./mini_client 2>&1 | tee baseline-client.log
```

每次测试记录：

```bash
git rev-parse HEAD
sha256sum client_config.txt server_config.txt
sha256sum mini-api-200MiB.bin mini-api-1GiB.bin
grep -E '\[diag_rx_budget\]|\[resolved-rx\]|launch requests|aggregate throughput' \
  baseline-client.log baseline-server.log
```

预期预算日志：

```text
[diag_rx_budget] active_paths=1 per_path_limit=40 round_limit=40 mode=single
[diag_rx_budget] active_paths=2 per_path_limit=256 round_limit=512 mode=dual
```

GET、POST、5G1、5G2、Agg 的实际网卡名、路由切换命令和故障注入命令依赖
现场环境，必须随目标机测试记录一并归档，不能假设当前的 `eth0` 就是最终接口。

## 9. 本机验证状态

本次已完成：

- Git 基线、远端差异和干净状态核验；
- 配置、关键常量、源码哈希和调用链冻结；
- 当前 `stream_cnt` 行为确认；
- 固定测试文件生成规则与预期哈希；
- `git diff --check` 静态检查。

本机没有 CMake、C/C++ 编译器或已安装的 WSL Linux 发行版；仓库也没有运行所需
的服务端证书、私钥及目标双网卡环境。因此以下验收必须在目标 Linux 环境补跑：

- 原 mini 全量编译；
- GET/POST 端到端传输；
- 单路径40与双路径256日志；
- 200 MiB、1 GiB 文件 SHA-256；
- 5G1、5G2、Agg 吞吐基线。

在这些目标机记录归档前，阶段0的“代码与配置冻结”已经完成，但“运行基线”
状态应标记为 `TARGET_ENV_PENDING`，不能用历史 V5.4.1 日志冒充本提交的结果。
