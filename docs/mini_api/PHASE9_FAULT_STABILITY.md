# Mini API 阶段 9：故障与稳定性验收

## 1. 必测清单

GET、POST 分别执行：

- 传输中断开 5G1，剩余 5G2 完成传输；
- 传输中断开 5G2，剩余 5G1 完成传输；
- 路径断开后恢复，snapshot 最终重新显示 2 条活动路径；
- server 异常退出，client 在有限时间内返回明确错误；
- client 异常退出，server 能清理 accepted connection 并继续 accept；
- 传输超时，API 返回 `TIMEOUT` close reason；
- 同一连接连续完成至少 20 个任务；
- 完整 init/new/connect/transfer/close/cleanup 至少 20 轮；
- 0 字节、1 字节、4 MiB 边界和大于 1 GiB 文件；
- 失败后重新建连并成功传输。

断开单路径并预期成功的用例必须检查最终 SHA-256；异常退出和超时用例必须检查明确错误文本，
不能把“进程没崩溃”当作通过。

## 2. 故障注入运行器

示例：POST 中断开 5G1，10 秒后恢复，预期 5G2 继续传输：

```bash
python3 scripts/mini_api/run_fault_case.py \
  --case-id post_disconnect_5g1 \
  --fault-type disconnect-5g1 \
  --expected-outcome success \
  --fault-after-seconds 5 \
  --recover-after-seconds 10 \
  --fault-command 'sudo ip link set wwan0 down' \
  --recovery-command 'sudo ip link set wwan0 up' \
  --expected-file /data/mini-api-test/payload-over-1g.bin \
  --actual-file /data/server-results/post-over-1g.bin \
  --output-dir /data/mini-api-fault-results \
  --timeout-seconds 1800 \
  -- ./build/mini/netins_udp_mini_api_client \
       --post --host 10.12.54.143 --port 8443 \
       --interface wwan0 --interface wwan1 \
       --local /data/mini-api-test/payload-over-1g.bin \
       --remote /post-over-1g.bin
```

运行器只执行显式传入的控制命令，不会自行猜测网卡名。只允许在隔离测试设备上使用。
一旦执行过 fault，`recovery-command` 会放在 `finally` 路径运行，即使 client 提前退出也会尝试
恢复网卡。若 client 尚未运行到注入时刻、注入/恢复命令失败、传输超时或哈希不一致，结果为
FAIL。

异常退出示例可以在命令中使用 `{client_pid}`：

```bash
--fault-type client-exit \
--expected-outcome error \
--fault-command 'kill -9 {client_pid}' \
--reconnect-command './run-small-reconnect-case.sh'
```

server 异常退出时，`fault-command` 应通过 SSH 或目标机服务管理器终止该测试 server。
`--reconnect-command` 可指定一条完整的重连/小文件传输命令；返回非 0 会使故障用例失败。
client 异常退出的 close reason 应从对应 server 日志核对；该类用例以 kill 成功、server 继续
accept 且 reconnect command 成功为自动门槛。其他预期失败用例必须提供
`--expected-error-pattern`。

## 3. 连续任务与重复生命周期

API client 新增公开示例参数：

```text
--repeat N
```

它在同一 handle、同一 H3 connection 上顺序调用 `netins_udp_transfer()` N 次，可直接覆盖同一
连接连续任务：

```bash
./build/mini/netins_udp_mini_api_client \
  --get --repeat 20 \
  --host 10.12.54.143 --port 8443 \
  --interface wwan0 --interface wwan1 \
  --remote /payload-200m.bin --local /data/repeat.bin
```

重复创建/销毁使用进程级运行器；命令里的 `{iteration}` 和 `{actual_file}` 会逐轮替换：

```bash
python3 scripts/mini_api/run_stability.py \
  --case-id lifecycle_get_20 \
  --iterations 20 \
  --expected-file /data/mini-api-test/payload-200m.bin \
  --actual-file-template '/data/lifecycle/get-{iteration}.bin' \
  --output-dir /data/mini-api-stability-results \
  --timeout-seconds 1800 \
  -- ./build/mini/netins_udp_mini_api_client \
       --get --host 10.12.54.143 --port 8443 \
       --interface wwan0 --interface wwan1 \
       --remote /payload-200m.bin --local '{actual_file}'
```

每一轮必须返回 0 且 SHA-256 一致；首次失败即停止并保留此前所有日志。

## 4. 验收判定

通过必须同时满足：

```text
单路径断开后另一条路径完成任务
恢复后可再次观察到两条活动路径
成功类用例 SHA-256 一致
失败类用例在超时内返回明确错误/close reason
没有死锁或无限等待
故障后能够重新建立连接
20 个同连接任务全部成功
20 轮完整生命周期全部成功
空文件、边界文件和 >1 GiB 文件完整
```

## 5. 当前状态

故障运行器、恢复保护、连续任务入口和生命周期运行器已完成本机语法/模拟验证。真实 5G
断链、恢复、远端进程退出和长时间压力测试需要目标 Linux/SIM8810 环境，当前状态为：

```text
TARGET_ENV_PENDING
```
