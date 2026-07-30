# Mini API 阶段 7：公开 API 示例程序

阶段 7 新增两个独立示例：

- `netins_udp_mini_api_client`
- `netins_udp_mini_api_server`

两个示例源文件只包含 `<xquic/netins_udp.h>` 和 C 标准库头文件，不包含 mini 私有头文件，
不访问 `xqc_*`、HTTP/3 request、event loop 或 stream count。它们同时是第三方接入的最小
可编译参考。

## 1. 编译

启用 `XQC_ENABLE_MINI_API` 后，两个程序随库一起构建：

```bash
cmake -S . -B build -DXQC_ENABLE_MINI_API=ON
cmake --build build --target \
  netins_udp_mini_api_client netins_udp_mini_api_server
```

交叉编译时仍需按项目编译指南提供 SDK、TLS、Libevent 和 zlog 路径。

## 2. 启动 API server

```bash
./netins_udp_mini_api_server \
  --bind 0.0.0.0 \
  --port 8443 \
  --cert /secure/server.crt \
  --key /secure/server.key \
  --root /srv/mini-api \
  --max-connections 2
```

server 依次 `listen -> accept -> serve`。每个 accepted handle 完成后单独关闭，listener 可继续
接受下一条连接。`--accept-timeout-ms 0` 和 `--serve-timeout-ms 0` 均表示无限等待。

## 3. GET

单路径：

```bash
./netins_udp_mini_api_client \
  --get \
  --host 10.12.54.143 \
  --port 8443 \
  --server-name mini.example \
  --interface wwan0 \
  --remote /payload.bin \
  --local ./download.bin
```

双路径：

```bash
./netins_udp_mini_api_client \
  --get \
  --host 10.12.54.143 \
  --port 8443 \
  --server-name mini.example \
  --interface wwan0 \
  --interface wwan1 \
  --scheduler bandwidth \
  --remote /payload.bin \
  --local ./download.bin
```

GET 使用 API 固定的 8 request 并发。

## 4. POST

```bash
./netins_udp_mini_api_client \
  --post \
  --host 10.12.54.143 \
  --port 8443 \
  --server-name mini.example \
  --interface wwan0 \
  --interface wwan1 \
  --remote /uploaded.bin \
  --local ./payload.bin
```

POST 使用 API 固定的 4 request 并发。

成功后 client 打印任务字节数、分片完成数、耗时、吞吐、multipath 状态和每条可见路径的统计。
诊断输出通过 `NETINS_UDP_OPT_LOGGER` 进入 stderr；业务结果输出到 stdout。

## 5. 验证状态

本机已完成：

- 两个示例在 C11、`-Wall -Wextra -Werror` 下独立编译；
- include 依赖检查确认仅使用公开 API；
- CMake 目标生成检查。

真实 server/client 握手和文件传输仍为 `TARGET_ENV_PENDING`，纳入阶段 8 的四种交叉组合矩阵。
