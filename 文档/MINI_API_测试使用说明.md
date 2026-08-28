# Mini API 测试使用说明

## 1. 测试用途

本套脚本用于在控制电脑上通过 SSH 自动控制 AARCH64 主板和 Linux 云服务器，完成 Mini API 的以下测试：

- `rmnet_data0` 单路径 GET/POST；
- `usb0` 单路径 GET/POST；
- `rmnet_data0 + usb0` 双路径聚合 GET/POST；
- 客户端传输速率、时间、字节数和分片完整性检查；
- 主板客户端进程 CPU、RSS、HWM 和虚拟内存采集；
- 云服务器整机及 Mini API 服务端进程 CPU、内存采集；
- 单路径与聚合测试结果汇总。

测试脚本位于：

```text
tests/mini_api_benchmark/
├── mini_api_benchmark.py
├── monitor_board_resource.sh
└── monitor_server_memory.sh
```

三个脚本必须配套使用。Python 脚本运行在控制电脑；两个 shell 脚本分别部署到主板和云服务器。

## 2. 默认测试拓扑

```text
控制电脑
  ├── SSH → AARCH64 主板：192.168.11.1
  │          ├── rmnet_data0
  │          └── usb0
  │
  └── SSH → Linux 云服务器：124.70.219.194
             └── UDP 18446
```

控制电脑必须能够同时 SSH 连接主板和云服务器。主板的两张网卡必须已经配置 IPv4 地址，并且都能够独立路由到云服务器。

## 3. 文件准备

### 3.1 控制电脑

控制电脑保留源码仓库中的以下文件：

```text
tests/mini_api_benchmark/mini_api_benchmark.py
tests/mini_api_benchmark/monitor_board_resource.sh
tests/mini_api_benchmark/monitor_server_memory.sh
```

控制电脑需要安装 Python 3 和 Paramiko：

```powershell
python -m pip install paramiko
```

建议使用 SSH 密钥连接主板和服务器。不要把 SSH 密码或私钥写入脚本并提交到仓库。

### 3.2 AARCH64 主板

主板默认需要以下文件：

```text
/oemdata/gx/xquic_file/netins_udp_mini_api_client
/oemdata/gx/xquic_file/download/kkk.txt
/oemdata/monitor_board_resource.sh
```

其中：

- `netins_udp_mini_api_client` 是 AARCH64 客户端 Demo；
- `download/kkk.txt` 是 POST 上传源文件，文件必须非空；
- `monitor_board_resource.sh` 负责采集客户端进程资源。

上传并授权：

```sh
chmod +x /oemdata/gx/xquic_file/netins_udp_mini_api_client
chmod +x /oemdata/monitor_board_resource.sh
```

主板需要提供 `bash`、`awk`、`sleep`、`date`、`find`、`getconf`、`ip`、`df` 和 `wc` 命令，并允许读取 `/proc`。

如果使用 `--verify-sha256`，主板还必须提供 `sha256sum`。

### 3.3 Linux 云服务器

当前自动压测脚本仍显式使用外部证书覆盖，因此默认测试环境需要以下文件：

```text
/root/test_xquic_server/netins_udp_mini_api_server
/root/test_xquic_server/server.crt
/root/test_xquic_server/server.key
/root/test_xquic_server/upload/1
/root/test_xquic_server/monitor_server_memory.sh
```

准备目录并授权：

```sh
cd /root/test_xquic_server
mkdir -p upload
chmod +x netins_udp_mini_api_server
chmod +x monitor_server_memory.sh
test -s upload/1
test -r server.crt
test -r server.key
```

`upload/1` 是默认 GET 源文件，必须非空。POST 结果默认写入 `upload` 目录，因此该父目录必须提前存在。

服务端 Demo 本身已经内置证书。手工启动或正式部署时可以同时省略 `server.crt`、`server.key`
以及命令中的 `--cert`、`--key`；保留这两个参数则继续使用指定文件覆盖内置证书。使用内置
证书或 `NETINS_UDP_OPT_TLS_MEMORY` 时，服务器还必须挂载并允许访问 `/proc/self/fd`；当前
自动压测使用外部文件覆盖，不依赖该路径。

服务器需要提供 `bash`、`awk`、`sleep`、`date`、`find`、`getconf`、`ss`、`nohup`、`grep`、`tr` 和 `wc` 命令，并允许读取 `/proc`。

如果使用 `--verify-sha256`，服务器还必须提供 `sha256sum`。

## 4. 部署资源监控脚本

可以从控制电脑上传监控脚本。以下命令中的仓库路径和 SSH 地址应按实际环境调整。

上传到主板：

```powershell
scp tests/mini_api_benchmark/monitor_board_resource.sh root@192.168.11.1:/oemdata/monitor_board_resource.sh
ssh root@192.168.11.1 "chmod +x /oemdata/monitor_board_resource.sh"
```

上传到云服务器：

```powershell
scp tests/mini_api_benchmark/monitor_server_memory.sh root@124.70.219.194:/root/test_xquic_server/monitor_server_memory.sh
ssh root@124.70.219.194 "chmod +x /root/test_xquic_server/monitor_server_memory.sh"
```

## 5. 测试前检查

### 5.1 主板检查

```sh
test -x /oemdata/gx/xquic_file/netins_udp_mini_api_client
test -x /oemdata/monitor_board_resource.sh
test -s /oemdata/gx/xquic_file/download/kkk.txt
ip -o -4 addr show dev rmnet_data0
ip -o -4 addr show dev usb0
ip -4 route get 124.70.219.194 oif rmnet_data0
ip -4 route get 124.70.219.194 oif usb0
```

### 5.2 云服务器检查

```sh
cd /root/test_xquic_server
test -x netins_udp_mini_api_server
test -x monitor_server_memory.sh
test -r server.crt
test -r server.key
test -s upload/1
test -d upload
ss -lunp | grep ':18446 ' || true
```

最后一条命令如果显示已有进程监听 UDP 18446，应先确认并停止旧的测试服务端。压测脚本会拒绝覆盖一个已经被占用的端口。

## 6. 运行测试

在仓库根目录运行：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py
```

默认测试参数如下：

| 参数 | 默认值 |
|---|---|
| 主板 SSH 地址 | `192.168.11.1:22` |
| 云服务器 SSH 地址 | `124.70.219.194:22` |
| QUIC 地址和端口 | `124.70.219.194:18446` |
| 主板工作目录 | `/oemdata/gx/xquic_file` |
| 服务器工作目录 | `/root/test_xquic_server` |
| 主板客户端 | `./netins_udp_mini_api_client` |
| 云服务器服务端 | `./netins_udp_mini_api_server` |
| 主板资源监控脚本 | `/oemdata/monitor_board_resource.sh` |
| 服务器资源监控脚本 | `/root/test_xquic_server/monitor_server_memory.sh` |
| GET 远端文件 | `/upload/1` |
| POST 本地文件 | `/oemdata/gx/xquic_file/download/kkk.txt` |
| GET 调度算法 | 固定 `adaptive` |
| POST 客户端调度算法 | `adaptive`（可选 minrtt、rap 等） |
| 客户端拥塞控制 | `bbr` |
| 测试轮数 | 20 |
| 模式顺序 | `rmnet,agg,usb`（5G1、聚合、5G2） |
| 单项最大尝试次数 | 2次（首次加1次重试） |
| 客户端超时 | 600000 ms |
| 资源采样间隔 | 0.5 s |
| SHA-256 校验 | 默认关闭 |

服务端由 Python 脚本在 `/root/test_xquic_server` 中自动启动，服务端 Demo 固定使用
`adaptive`。Python 的 `--scheduler` 只控制 POST 客户端；GET 命令始终传入 `adaptive`。

每轮先完成全部 GET，再完成全部 POST；GET 和 POST 内部都按照 `--order` 指定的网络模式顺序执行。默认一轮的实际顺序为：

```text
GET 5G1 -> GET 聚合 -> GET 5G2 -> POST 5G1 -> POST 聚合 -> POST 5G2
```

其中 `rmnet` 表示 5G1（`rmnet_data0`），`agg` 表示双路径聚合（`rmnet_data0 + usb0`），`usb` 表示 5G2（`usb0`）。`--order` 只调整每个 GET/POST 分组内部的网络模式顺序，不改变“先 GET、后 POST”的方向顺序。

### 6.1 使用 SSH 密钥

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --board-ssh-key C:\keys\board_key `
  --server-ssh-key C:\keys\server_key
```

### 6.2 使用环境变量提供密码

```powershell
$env:MINI_BENCH_BOARD_PASSWORD = "主板密码"
$env:MINI_BENCH_SERVER_PASSWORD = "服务器密码"
python tests/mini_api_benchmark/mini_api_benchmark.py
```

测试结束后应清除当前终端中的密码变量：

```powershell
Remove-Item Env:MINI_BENCH_BOARD_PASSWORD
Remove-Item Env:MINI_BENCH_SERVER_PASSWORD
```

### 6.3 常用参数示例

执行10轮并启用文件 SHA-256 校验：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --iterations 10 `
  --verify-sha256
```

修改服务器地址和端口：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --server-ssh-host 124.70.219.194 `
  --quic-host 124.70.219.194 `
  --quic-port 18446
```

修改 GET 组和 POST 组内部的网络模式顺序，例如改为“5G2、聚合、5G1”：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --order usb,agg,rmnet
```

此时每轮顺序为 `GET 5G2 -> GET 聚合 -> GET 5G1 -> POST 5G2 -> POST 聚合 -> POST 5G1`。`--order` 不会把 POST 调到 GET 前面。

查看全部参数：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py --help
```

### 6.4 修改GET源文件

GET源文件位于云服务器，由以下两个参数共同决定：

```text
--server-dir
--remote-path
```

脚本启动服务端前会进入 `--server-dir`，并把当前目录作为服务端 `--root`。因此实际GET文件路径为：

```text
实际服务器路径 = server-dir + remote-path
```

默认参数：

```text
--server-dir /root/test_xquic_server
--remote-path /upload/1
```

对应的实际文件是：

```text
/root/test_xquic_server/upload/1
```

例如，准备使用服务器上的：

```text
/data/mini_test/files/get-500m.bin
```

可以执行：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --server-dir /data/mini_test `
  --remote-path /files/get-500m.bin
```

测试前检查：

```sh
test -s /data/mini_test/files/get-500m.bin
```

注意：

- `--remote-path` 必须以 `/` 开头；
- `--remote-path /files/get-500m.bin` 是相对于 `--server-dir` 的服务端路径，不是服务器文件系统的绝对路径；
- 不能使用 `..` 访问 `--server-dir` 之外的文件；
- 服务端会拒绝通过符号链接穿越根目录；
- 当前压测脚本固定从 `--server-dir/server.crt` 和 `--server-dir/server.key` 加载证书，因此修改 `--server-dir` 后也要把证书放到新目录；
- 如需让压测脚本也使用内置证书，应在脚本的服务端启动命令中同时删除 `--cert` 和 `--key` 两项，不能只删除其中一个；
- 新的 GET 源文件必须非空，脚本会在测试前读取并记录文件大小。

如果只想替换文件内容而不修改命令，也可以保持 `/upload/1` 不变，直接替换：

```text
/root/test_xquic_server/upload/1
```

### 6.5 修改POST源文件

POST源文件位于主板，由 `--post-local` 指定。建议始终使用主板上的绝对路径。

默认值：

```text
--post-local /oemdata/gx/xquic_file/download/kkk.txt
```

例如改为上传：

```text
/oemdata/test_data/post-1g.bin
```

运行：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --post-local /oemdata/test_data/post-1g.bin
```

测试前在主板检查：

```sh
test -s /oemdata/test_data/post-1g.bin
```

三个模式的POST目标文件由三个独立参数控制：

```text
--post-remote-rmnet
--post-remote-usb
--post-remote-agg
```

例如：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --post-local /oemdata/test_data/post-1g.bin `
  --post-remote-rmnet /post_result/rmnet-post-1g.bin `
  --post-remote-usb /post_result/usb-post-1g.bin `
  --post-remote-agg /post_result/agg-post-1g.bin
```

如果 `--server-dir` 为 `/data/mini_test`，上述三个目标的实际服务器路径分别为：

```text
/data/mini_test/post_result/rmnet-post-1g.bin
/data/mini_test/post_result/usb-post-1g.bin
/data/mini_test/post_result/agg-post-1g.bin
```

测试前必须在服务器创建父目录：

```sh
mkdir -p /data/mini_test/post_result
```

注意：

- 三个POST远端路径必须互不相同；
- POST远端路径不能与GET源文件路径相同；
- POST远端路径必须以 `/` 开头；
- 脚本会在每次POST前删除本次目标文件，因此不要把生产文件配置为POST测试目标；
- 默认只检查文件大小和完整分片，正式完整性验收建议增加 `--verify-sha256`。

### 6.6 修改 POST 调度算法和客户端拥塞控制

Python 参数 `--scheduler` 只用于 POST 客户端。GET 客户端和文件服务端均固定为
`adaptive`，不会随该参数变化。

默认配置：

```text
GET:  adaptive
POST: --scheduler adaptive
--cc bbr
```

使用 `minrtt`：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --scheduler minrtt `
  --cc bbr
```

使用 `rap`：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --scheduler rap `
  --cc bbr
```

使用 `adaptive`：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --scheduler adaptive `
  --cc bbr
```

拥塞控制可以通过 `--cc` 修改。例如：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --scheduler minrtt `
  --cc cubic
```

进行不同版本或不同网络环境的对比测试时，POST 必须保持 `--scheduler` 和 `--cc` 一致，
并在记录中写明实际参数。不要把 Python 的 `--scheduler minrtt` 理解为 GET 或服务端也使用
`minrtt`。

实际服务端 Demo 启动命令不再包含 scheduler 参数：

```sh
./netins_udp_mini_api_server \
  --bind 0.0.0.0 \
  --port 18446 \
  --cert "$PWD/server.crt" \
  --key "$PWD/server.key" \
  --root "$PWD" \
  --accept-timeout-ms 0 \
  --serve-timeout-ms 600000 \
  --max-connections 3
```

上例继续使用外部证书覆盖。使用内置证书时，同时删除 `--cert` 和 `--key` 两行即可。

服务端开始执行 `listen()` 时会输出当前交付版本，例如：

```text
[mini-api:1] Mini API server version=6.4
```

服务端 scheduler 已在 Demo 内部固定为 `adaptive`，不提供 `--scheduler` 或
`--server-scheduler` 参数。

### 6.7 API服务端不在默认位置

服务端位置由两个参数控制：

```text
--server-dir
--server-executable
```

两者用途不同：

- `--server-dir`：服务端启动时的工作目录，同时也是文件服务根目录；
- `--server-executable`：服务端可执行文件路径，可以是相对于 `--server-dir` 的路径，也可以是服务器绝对路径。

#### 情况一：可执行文件和测试文件都在同一个新目录

服务器文件：

```text
/data/api_server/netins_udp_mini_api_server
/data/api_server/server.crt
/data/api_server/server.key
/data/api_server/upload/1
```

运行：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --server-dir /data/api_server `
  --server-executable ./netins_udp_mini_api_server
```

#### 情况二：可执行文件与文件服务目录分开

例如：

```text
服务端程序：/opt/netins/bin/netins_udp_mini_api_server
证书和测试文件：/data/mini_test/
```

运行：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --server-dir /data/mini_test `
  --server-executable /opt/netins/bin/netins_udp_mini_api_server
```

此时仍然要求以下文件存在：

```text
/data/mini_test/server.crt
/data/mini_test/server.key
/data/mini_test/upload/1
```

并检查：

```sh
test -x /opt/netins/bin/netins_udp_mini_api_server
test -r /data/mini_test/server.crt
test -r /data/mini_test/server.key
test -s /data/mini_test/upload/1
```

不要只修改 `--server-executable` 后就认为GET和POST根目录也随可执行文件移动；文件根目录只由 `--server-dir` 决定。

### 6.8 服务器资源监控脚本不在默认位置

服务器资源监控脚本由 `--server-resource-monitor` 指定。该路径是云服务器上的路径，不是控制电脑上的路径。

例如脚本实际位于：

```text
/opt/netins/tests/monitor_server_memory.sh
```

先在服务器授权：

```sh
chmod +x /opt/netins/tests/monitor_server_memory.sh
```

然后运行：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --server-resource-monitor /opt/netins/tests/monitor_server_memory.sh
```

服务器资源CSV临时目录由 `--server-resource-dir` 控制。例如：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --server-resource-monitor /opt/netins/tests/monitor_server_memory.sh `
  --server-resource-dir /data/mini_resource
```

对应服务器检查：

```sh
test -x /opt/netins/tests/monitor_server_memory.sh
mkdir -p /data/mini_resource
test -w /data/mini_resource
```

不要把控制电脑中的 `tests/mini_api_benchmark/monitor_server_memory.sh` 路径直接传给该参数。必须先上传到云服务器，再填写上传后的服务器路径。

### 6.9 主板客户端或监控脚本不在默认位置

主板位置由以下参数控制：

```text
--board-dir
--client-executable
--board-resource-monitor
--board-resource-dir
```

例如：

```text
客户端：/opt/netins/bin/netins_udp_mini_api_client
主板工作目录：/oemdata/mini_work
监控脚本：/opt/netins/tests/monitor_board_resource.sh
资源临时目录：/oemdata/mini_resource
```

运行：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --board-dir /oemdata/mini_work `
  --client-executable /opt/netins/bin/netins_udp_mini_api_client `
  --board-resource-monitor /opt/netins/tests/monitor_board_resource.sh `
  --board-resource-dir /oemdata/mini_resource `
  --post-local /oemdata/test_data/post-1g.bin
```

主板检查：

```sh
test -d /oemdata/mini_work
test -w /oemdata/mini_work
test -x /opt/netins/bin/netins_udp_mini_api_client
test -x /opt/netins/tests/monitor_board_resource.sh
test -s /oemdata/test_data/post-1g.bin
mkdir -p /oemdata/mini_resource
test -w /oemdata/mini_resource
```

GET结果文件会临时写入 `--board-dir`，因此该目录必须可写并有足够空间。

### 6.10 修改SSH地址、用户名和端口

主板SSH参数：

```text
--board-ssh-host
--board-ssh-port
--board-ssh-user
--board-ssh-key
```

服务器SSH参数：

```text
--server-ssh-host
--server-ssh-port
--server-ssh-user
--server-ssh-key
```

`--server-ssh-host` 是控制电脑连接云服务器的SSH地址；`--quic-host` 是主板发起QUIC连接时访问的地址。两者通常相同，但在SSH经过跳转、端口映射或管理网与业务网分离时可能不同。

例如：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --board-ssh-host 192.168.8.1 `
  --board-ssh-port 22 `
  --board-ssh-user root `
  --server-ssh-host 10.0.0.20 `
  --server-ssh-port 2222 `
  --server-ssh-user tester `
  --quic-host 124.70.219.194 `
  --quic-port 18446
```

### 6.11 完整的自定义路径示例

假设测试环境如下：

```text
主板客户端：/opt/netins/bin/netins_udp_mini_api_client
主板监控脚本：/opt/netins/tests/monitor_board_resource.sh
主板POST源文件：/oemdata/test_data/post-1g.bin
服务器程序：/opt/netins/bin/netins_udp_mini_api_server
服务器工作根目录：/data/mini_test
服务器监控脚本：/opt/netins/tests/monitor_server_memory.sh
服务器GET文件：/data/mini_test/get_data/get-1g.bin
服务器POST目录：/data/mini_test/post_result
```

服务器先准备：

```sh
mkdir -p /data/mini_test/get_data /data/mini_test/post_result
test -x /opt/netins/bin/netins_udp_mini_api_server
test -x /opt/netins/tests/monitor_server_memory.sh
test -s /data/mini_test/get_data/get-1g.bin
test -r /data/mini_test/server.crt
test -r /data/mini_test/server.key
```

主板先准备：

```sh
mkdir -p /oemdata/mini_work /oemdata/mini_resource
test -x /opt/netins/bin/netins_udp_mini_api_client
test -x /opt/netins/tests/monitor_board_resource.sh
test -s /oemdata/test_data/post-1g.bin
```

控制电脑运行：

```powershell
python tests/mini_api_benchmark/mini_api_benchmark.py `
  --server-dir /data/mini_test `
  --server-executable /opt/netins/bin/netins_udp_mini_api_server `
  --server-resource-monitor /opt/netins/tests/monitor_server_memory.sh `
  --server-resource-dir /data/mini_resource `
  --remote-path /get_data/get-1g.bin `
  --post-remote-rmnet /post_result/rmnet-1g.bin `
  --post-remote-usb /post_result/usb-1g.bin `
  --post-remote-agg /post_result/agg-1g.bin `
  --board-dir /oemdata/mini_work `
  --client-executable /opt/netins/bin/netins_udp_mini_api_client `
  --board-resource-monitor /opt/netins/tests/monitor_board_resource.sh `
  --board-resource-dir /oemdata/mini_resource `
  --post-local /oemdata/test_data/post-1g.bin `
  --scheduler minrtt `
  --cc bbr `
  --iterations 20 `
  --verify-sha256
```

## 7. 输出文件

默认结果目录为：

```text
tests/mini_api_benchmark/log/
```

主要输出包括：

```text
log/
├── 5G1_summary_get_iterXX_*.log
├── 5G1_summary_post_iterXX_*.log
├── 5G2_summary_get_iterXX_*.log
├── 5G2_summary_post_iterXX_*.log
├── Agg_summary_get_iterXX_*.log
├── Agg_summary_post_iterXX_*.log
├── aggregation_summary_get_<session>.csv
├── aggregation_summary_post_<session>.csv
├── stress_summary_<session>.csv
├── server_memory_<session>.csv
├── raw_client/<session>/*.stdout.log
├── raw_client/<session>/*.stderr.log
└── resources/<session>/*_resource.csv
```

- 单项 `summary` 文件记录文件大小、传输时间、速率和开始/结束时间；
- `aggregation_summary` 汇总单路径及聚合结果；
- `stress_summary` 汇总成功状态、速率及主板/服务器资源数据；
- `raw_client` 保留每次客户端尝试的原始 stdout 和 stderr，出现异常时应优先检查这里；
- `resources` 保存每次客户端进程的原始资源采样；
- `server_memory` 保存云服务器整机和服务端进程的资源采样。

`log/` 已由测试目录内的 `.gitignore` 排除，不应把压测结果、下载文件或临时文件提交到源码仓库。

## 8. 成功判定与文件完整性

一次传输被脚本判定为成功，需要同时满足：

- 客户端没有超时且退出码为0；
- 客户端输出包含完整的传输汇总；
- 字节数、时间和速率为有效值；
- 已完成分片数等于计划分片数；
- GET 下载文件或 POST 上传源文件大小与预期一致；
- 输出中的速率与字节数、时间能够互相校验；
- 启用 `--verify-sha256` 时，文件 SHA-256 必须一致。

用于正式文件完整性验收时，建议启用：

```text
--verify-sha256
```

## 9. CPU和内存数据说明

- `client_process_cpu_pct` 是主板客户端进程的 top 风格 CPU 占用，`100%` 表示占满一个逻辑CPU核，多线程进程可能超过 `100%`；
- `server_process_cpu_pct` 是云服务器 API 服务端进程的 top 风格 CPU 占用；
- `system_cpu_pct` 是云服务器整机所有CPU核汇总后的利用率，范围为 `0%～100%`；
- `RSS` 表示进程当前实际驻留的物理内存；
- `HWM` 表示进程运行期间出现过的最高 RSS；
- `VmSize` 是虚拟地址空间大小，不应直接作为实际内存占用；
- 判断 SDK 或网关进程内存占用率时，建议使用 `进程 RSS / 系统 MemTotal`，并同时查看最大 RSS 和 HWM。

短文件传输时间可能小于资源采样间隔，导致资源CSV没有有效样本。需要评价CPU和内存时，应使用足够大的测试文件，或适当减小采样间隔。

## 10. 注意事项

1. 测试前确认主板和服务器使用同一批次兼容的客户端、服务端可执行文件。
2. 不要同时从两台控制电脑运行同一套压测。脚本会检测带有测试标记的旧进程，但仍不应依赖自动检测代替环境管理。
3. 不要在脚本运行期间手工启动或停止 Mini API 服务端、客户端及资源监控脚本。
4. UDP 18446 必须允许主板访问，云服务器安全组和防火墙也必须放行该端口。
5. 当前自动压测脚本使用外部证书覆盖时，证书和私钥必须可读；使用内置证书时不需要部署这两个文件。
6. GET 源文件和 POST 源文件必须非空，主板必须有足够空间保存 GET 下载文件。
7. POST 的父目录必须提前存在。脚本会清理本次测试对应的目标文件，不应把生产文件放在相同路径。
8. 远端路径以服务端 `--root` 为根目录。默认 `/upload/1` 对应 `/root/test_xquic_server/upload/1`。
9. 默认关闭 SHA-256 是为了减少压测准备时间，不代表正式文件完整性验收可以省略哈希校验。
10. 若某次测试失败，脚本最多自动再尝试一次；失败尝试的原始 stdout/stderr 会保留供分析。
11. 测试被强制中断后，下一次启动前应检查 UDP 端口、服务端进程和 `/tmp` 中的测试控制文件。
12. 不要把 `log/`、额外的外部证书私钥、SSH密码、大型测试文件以及GET/POST结果文件提交到 Git。

## 11. 常见问题

### 11.1 提示 Paramiko 未安装

```powershell
python -m pip install paramiko
```

安装后确认运行脚本的 Python 与安装 Paramiko 的 Python 是同一个解释器。

### 11.2 主板预检失败

重点检查：

- 客户端和监控脚本是否存在并具有执行权限；
- `rmnet_data0`、`usb0` 是否为 UP 状态；
- 两张网卡是否都有 IPv4 地址和到服务器的路由；
- POST 源文件是否存在且非空；
- 主板工作目录剩余空间是否充足；
- 是否存在另一套尚未结束的测试进程。

### 11.3 云服务器预检失败

重点检查：

- 服务端、监控脚本和 GET 源文件是否存在；若命令使用 `--cert`/`--key`，再检查对应证书和
  私钥；若使用内置或内存证书，再检查 `/proc/self/fd`；
- `upload` 目录是否存在；
- UDP端口是否被旧服务占用；
- `/tmp/gx_server_memory` 是否可写；
- 服务器是否提供 `ss`、`awk`、`getconf` 等命令。

### 11.4 测试成功但资源样本为空

通常是文件太小，传输时间短于默认 `0.5 s` 采样间隔。可换用更大的测试文件，或通过以下参数缩短采样间隔：

```text
--board-resource-interval-s 0.2
--server-resource-interval-s 0.2
```

采样间隔过短会增加监控脚本自身开销，不建议在没有对照测试的情况下无限缩短。

### 11.5 测试失败或速率为空

先查看本次会话的：

```text
log/raw_client/<session>/*.stdout.log
log/raw_client/<session>/*.stderr.log
```

再检查控制台给出的失败原因、客户端退出码、分片完成情况、文件大小、网络路由和服务端是否仍在运行。
