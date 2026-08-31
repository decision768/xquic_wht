# Stream Demo 数据流编译测试说明

本文说明如何复用已有客户端和服务端编译环境，通过文件覆盖构建当前交付版本的文件 Demo 与
Stream Demo，并完成基本运行验证。

当前源码可以生成以下四个可执行文件：

```text
netins_udp_mini_api_client    文件 Demo 客户端
netins_udp_mini_api_server    文件 Demo 服务端
netins_udp_stream_client      数据流 Demo 客户端
netins_udp_stream_server      数据流 Demo 服务端
```

当前交付基线：

| 项目 | 值 |
|---|---|
| API Release | `6.5` |
| 内部 Stream 协议 | v4 |
| Stream Demo | 4 lane |
| 数据方向 | `--post`（Client→Server）或 `--get`（Server→Client） |

## 1. 环境复用

原编译环境中的以下内容可以继续复用：

- 编译器和交叉编译工具链；
- SIM8810 SDK 和 sysroot；
- 与目标架构一致的 Tongsuo/BabaSSL；
- 与目标架构一致的 Libevent；
- 与目标架构一致的 zlog；
- 原有端口、路由、证书和目标板运行环境。

原环境中的源码版本可能早于当前版本。编译当前交付产物时，下面这些文件和目录应与当前交付
源码保持一致：

```text
CMakeLists.txt
VERSION
cmake/
demo/
include/
mini/
scripts/
src/
tests/
```

其中 `tests/` 用于正式压测，不参与四个 Demo 的 C 编译。已有第三方依赖的版本、架构和路径
满足要求时，可以继续使用原依赖目录。

不能只替换两个 Stream Demo 源文件。数据流功能还依赖公共头文件、Mini backend、H3
Bytestream、分流与重排协议、导出符号以及 CMake 目标；只替换 Demo 文件可能导致 API 缺失、
链接失败或两端协议不一致。

## 2. 覆盖当前源码

建议先备份原源码目录，再通过现有文件传输方式，把当前交付源码按原相对路径覆盖到客户端和
服务端编译机。可以新建一个源码目录，也可以在完成备份后覆盖原源码目录。

覆盖完成后，在两台编译机的源码根目录分别执行：

```bash
cat VERSION
grep 'NETINS_UDP_API_RELEASE_VERSION' include/xquic/netins_udp.h
grep 'MINI_STREAM_PROTOCOL_VERSION' mini/mini_stream_protocol.h
test -f demo/netins_udp_mini_api_client.c
test -f demo/netins_udp_mini_api_server.c
test -f demo/netins_udp_stream_client.c
test -f demo/netins_udp_stream_server.c
grep 'add_executable(netins_udp_mini_api_client' mini/CMakeLists.txt
grep 'add_executable(netins_udp_stream_client' mini/CMakeLists.txt
grep 'add_executable(netins_udp_stream_server' mini/CMakeLists.txt
```

当前版本的关键输出应包含：

```text
6.5
#define NETINS_UDP_API_RELEASE_VERSION "6.5"
#define MINI_STREAM_PROTOCOL_VERSION 4U
```

如果版本、协议或构建目标不匹配，说明源码没有完整覆盖，应先补齐文件再配置 CMake。

## 3. 建议使用新的构建目录

建议针对当前源码创建新的客户端和服务端构建目录，例如：

```text
build-client-aarch64-delivery
build-server-linux-delivery
```

新的构建目录可以避免原缓存中的源码路径、编译器、目标架构和依赖路径影响当前构建。客户端
与服务端目标架构不同，必须使用各自独立的构建目录和同架构依赖。

当前工程使用根目录中的同一份 `CMakeLists.txt`，通过不同配置参数分别生成 AARCH64 客户端
产物和 Linux x86_64 服务端产物。

## 4. AARCH64 客户端构建

### 4.1 SIM8810 交叉编译

在客户端编译机进入当前源码根目录，根据实际环境调整下面的路径：

```bash
cd /实际路径/当前源码目录
DELIVERY_SRC="$PWD"

ARM_SDK=/home/admin123/SIM8810_SDK_OCPU_LE3.0_1.1.0_251207
ARM_TC=/opt/crosstools/toolchain/gcc/usr/bin/aarch64-oe-linux
ARM_TLS="$DELIVERY_SRC/../third_party/Tongsuo-8.3.0"
ARM_EVENT=/home/admin123/libevent_aarch64/libevent-2.1.12-stable
ARM_ZLOG=/home/admin123/zlog-1.2.18
CLIENT_BUILD="$DELIVERY_SRC/build-client-aarch64-delivery"

export SIM8810_SDK_ROOT="$ARM_SDK"
export SIM8810_TOOLCHAIN_DIR="$ARM_TC"
```

检查依赖：

```bash
test -d "$ARM_SDK/sysroots_pvm"
test -x "$ARM_TC/aarch64-oe-linux-gcc"
test -f "$ARM_TLS/include/openssl/ssl.h"
test -f "$ARM_TLS/libssl.a"
test -f "$ARM_TLS/libcrypto.a"
test -f "$ARM_EVENT/include/event2/event.h"
test -f "$ARM_EVENT/.libs/libevent.a"
test -f "$ARM_EVENT/.libs/libevent_pthreads.a"
test -f "$ARM_ZLOG/src/zlog.h"
test -f "$ARM_ZLOG/build_aarch64/lib/libzlog.a"
```

配置：

```bash
cmake -S "$DELIVERY_SRC" -B "$CLIENT_BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$DELIVERY_SRC/cmake/toolchains/sim8810-aarch64.cmake" \
  -DSIM8810_SDK_ROOT="$ARM_SDK" \
  -DSIM8810_TOOLCHAIN_DIR="$ARM_TC" \
  -DCMAKE_BUILD_TYPE=Release \
  -DXQC_ENABLE_MINI_API=ON \
  -DXQC_ENABLE_TESTING=OFF \
  -DSSL_TYPE=babassl \
  -DSSL_INC_PATH="$ARM_TLS/include" \
  -DSSL_LIB_PATH="$ARM_TLS/libssl.a;$ARM_TLS/libcrypto.a" \
  -DLIBEVENT_INCLUDE_DIR="$ARM_EVENT/include" \
  -DLIBEVENT_LIBRARY="$ARM_EVENT/.libs/libevent.a" \
  -DLIBEVENT_PTHREADS_LIBRARY="$ARM_EVENT/.libs/libevent_pthreads.a" \
  -DZLOG_INCLUDE_DIR="$ARM_ZLOG/src" \
  -DZLOG_STATIC_LIB="$ARM_ZLOG/build_aarch64/lib/libzlog.a"
```

一次生成文件客户端 Demo 和数据流客户端 Demo：

```bash
cmake --build "$CLIENT_BUILD" \
  --target netins_udp_mini_api_client netins_udp_stream_client \
  --parallel "$(nproc)"
```

也可以分别构建：

```bash
cmake --build "$CLIENT_BUILD" \
  --target netins_udp_mini_api_client \
  --parallel "$(nproc)"

cmake --build "$CLIENT_BUILD" \
  --target netins_udp_stream_client \
  --parallel "$(nproc)"
```

客户端产物：

```text
build-client-aarch64-delivery/mini/netins_udp_mini_api_client
build-client-aarch64-delivery/mini/netins_udp_stream_client
build-client-aarch64-delivery/libxquic-static.a
```

验证架构：

```bash
file "$CLIENT_BUILD/mini/netins_udp_mini_api_client"
file "$CLIENT_BUILD/mini/netins_udp_stream_client"
"$ARM_TC/aarch64-oe-linux-readelf" -h \
  "$CLIENT_BUILD/mini/netins_udp_stream_client" | grep Machine
```

结果应包含 `ARM aarch64` 或 `AArch64`。交叉编译产物应上传到 SIM8810/AARCH64 目标机运行。

### 4.2 在 AARCH64 Linux 主机原生编译

如果客户端原本就在 AARCH64 Linux 主机上原生编译，可继续使用同架构依赖，并配置新的构建
目录。原生编译不传 SIM8810 工具链参数：

```bash
CLIENT_BUILD="$DELIVERY_SRC/build-client-aarch64-native-delivery"

cmake -S "$DELIVERY_SRC" -B "$CLIENT_BUILD" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DXQC_ENABLE_MINI_API=ON \
  -DXQC_ENABLE_TESTING=OFF \
  -DSSL_TYPE=babassl \
  -DSSL_INC_PATH="$ARM_TLS/include" \
  -DSSL_LIB_PATH="$ARM_TLS/libssl.a;$ARM_TLS/libcrypto.a" \
  -DLIBEVENT_INCLUDE_DIR="$ARM_EVENT/include" \
  -DLIBEVENT_LIBRARY="$ARM_EVENT/.libs/libevent.a" \
  -DLIBEVENT_PTHREADS_LIBRARY="$ARM_EVENT/.libs/libevent_pthreads.a" \
  -DZLOG_INCLUDE_DIR="$ARM_ZLOG/src" \
  -DZLOG_STATIC_LIB="$ARM_ZLOG/build_aarch64/lib/libzlog.a"

cmake --build "$CLIENT_BUILD" \
  --target netins_udp_mini_api_client netins_udp_stream_client \
  --parallel "$(nproc)"
```

## 5. Linux x86_64 服务端构建

在服务端进入当前源码根目录，根据实际环境设置依赖路径：

```bash
cd /实际路径/当前源码目录
DELIVERY_SRC="$PWD"

LINUX_TLS=/usr/local/babassl
LINUX_EVENT=/data/quic/libevent-2.1.12-stable
LINUX_ZLOG=/实际路径/zlog-1.2.18
SERVER_BUILD="$DELIVERY_SRC/build-server-linux-delivery"
```

先确认 BabaSSL 静态库位置：

```bash
find "$LINUX_TLS" -maxdepth 2 \
  \( -name libssl.a -o -name libcrypto.a \) -print
```

如果静态库位于 BabaSSL 根目录：

```bash
LINUX_SSL_LIB="$LINUX_TLS/libssl.a"
LINUX_CRYPTO_LIB="$LINUX_TLS/libcrypto.a"
```

如果静态库位于 `lib/` 子目录：

```bash
LINUX_SSL_LIB="$LINUX_TLS/lib/libssl.a"
LINUX_CRYPTO_LIB="$LINUX_TLS/lib/libcrypto.a"
```

检查依赖：

```bash
test "$(uname -m)" = x86_64
test -x /usr/bin/gcc
test -x /usr/bin/g++
test -f "$LINUX_TLS/include/openssl/ssl.h"
test -f "$LINUX_SSL_LIB"
test -f "$LINUX_CRYPTO_LIB"
test -f "$LINUX_EVENT/include/event2/event.h"
test -f "$LINUX_EVENT/.libs/libevent.a"
test -f "$LINUX_EVENT/.libs/libevent_pthreads.a"
test -f "$LINUX_ZLOG/src/zlog.h"
test -f "$LINUX_ZLOG/build/lib/libzlog.a"
```

配置：

```bash
cmake -S "$DELIVERY_SRC" -B "$SERVER_BUILD" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DXQC_ENABLE_MINI_API=ON \
  -DXQC_ENABLE_TESTING=OFF \
  -DSSL_TYPE=babassl \
  -DSSL_INC_PATH="$LINUX_TLS/include" \
  -DSSL_LIB_PATH="$LINUX_SSL_LIB;$LINUX_CRYPTO_LIB" \
  -DLIBEVENT_INCLUDE_DIR="$LINUX_EVENT/include" \
  -DLIBEVENT_LIBRARY="$LINUX_EVENT/.libs/libevent.a" \
  -DLIBEVENT_PTHREADS_LIBRARY="$LINUX_EVENT/.libs/libevent_pthreads.a" \
  -DZLOG_INCLUDE_DIR="$LINUX_ZLOG/src" \
  -DZLOG_STATIC_LIB="$LINUX_ZLOG/build/lib/libzlog.a"
```

一次生成文件服务端 Demo 和数据流服务端 Demo：

```bash
cmake --build "$SERVER_BUILD" \
  --target netins_udp_mini_api_server netins_udp_stream_server \
  --parallel "$(nproc)"
```

也可以分别构建：

```bash
cmake --build "$SERVER_BUILD" \
  --target netins_udp_mini_api_server \
  --parallel "$(nproc)"

cmake --build "$SERVER_BUILD" \
  --target netins_udp_stream_server \
  --parallel "$(nproc)"
```

服务端产物：

```text
build-server-linux-delivery/mini/netins_udp_mini_api_server
build-server-linux-delivery/mini/netins_udp_stream_server
build-server-linux-delivery/libxquic-static.a
```

验证架构和运行时依赖：

```bash
file "$SERVER_BUILD/mini/netins_udp_mini_api_server"
file "$SERVER_BUILD/mini/netins_udp_stream_server"
readelf -h "$SERVER_BUILD/mini/netins_udp_stream_server" | grep Machine
ldd "$SERVER_BUILD/mini/netins_udp_mini_api_server"
ldd "$SERVER_BUILD/mini/netins_udp_stream_server"
```

`Machine` 应为 `Advanced Micro Devices X86-64`，`ldd` 不应出现 `not found`。

## 6. Stream Demo 基本测试

同一对 Stream Demo 可执行文件同时支持 POST 和 GET。每次测试前重新启动服务端，Client 与
Server 的 `BYTES`、`SEED` 和方向必须一致。

### 6.1 POST：客户端发送，服务端接收

云端服务端：

```bash
./netins_udp_stream_server 0.0.0.0 18460 67108864 7 \
  600000 --post
```

AARCH64 客户端：

```bash
./netins_udp_stream_client SERVER_IP 18460 67108864 7 \
  0 600000 600000 \
  --post \
  --interface rmnet_data0 \
  --interface usb0 \
  --scheduler balanced
```

### 6.2 GET：服务端发送，客户端接收

云端服务端：

```bash
./netins_udp_stream_server 0.0.0.0 18460 67108864 7 \
  600000 --get \
  --write-timeout-ms 600000
```

AARCH64 客户端：

```bash
./netins_udp_stream_client SERVER_IP 18460 67108864 7 \
  0 600000 600000 \
  --get \
  --interface rmnet_data0 \
  --interface usb0
```

测试数据由发送端 Demo 在内存中生成，不需要准备输入文件。GET 使用 `adaptive`；POST 未传
`--scheduler` 时默认使用 `balanced`。成功时两端都应输出：

```text
[stream_api_result] ... success=1 ...
```

## 7. 文件 Demo 与全量构建

完成第 4、5 节的双目标构建后，文件 Demo 与 Stream Demo 已同时生成。文件 Demo 仍使用原有
文件传输流程、命令行参数和压测脚本。

如果需要构建当前架构下的全部默认目标，可执行：

```bash
cmake --build "$CLIENT_BUILD" --parallel "$(nproc)"
cmake --build "$SERVER_BUILD" --parallel "$(nproc)"
```

源码检查：

```bash
cd "$DELIVERY_SRC"
python3 scripts/mini_api/check_stream_isolation.py --root .
python3 scripts/mini_api/check_abi.py \
  --header include/xquic/netins_udp.h \
  --version-script scripts/xquic.lds
```

预期输出：

```text
STREAM ISOLATION CHECK: PASS (55 invariants)
ABI CHECK: PASS (28 public functions)
```

## 8. 正式 GET+POST 压测

控制主机安装 Python 3 和 `paramiko`，主板与云端沿用文件压测环境中的
`monitor_board_resource.sh` 和 `monitor_server_memory.sh`。运行：

```bash
python tests/stream_api_benchmark.py --iterations 10
```

脚本每轮传输 100 MiB，依次执行 GET 和 POST 的 `5G1 -> Agg -> 5G2`。GET 使用
`adaptive`，POST 默认 `balanced`。输出包含每次速率日志、GET/POST 聚合率汇总、资源汇总和
资源采样 CSV。

执行下面的命令可以查看 SSH 地址、运行目录和可执行文件名等参数：

```bash
python tests/stream_api_benchmark.py --help
```

原文件综合压测脚本仍可继续用于文件 Demo，不需要因增加 Stream Demo 而修改。

## 9. 常见问题

### CMake 中没有四个 Demo 目标

检查当前源码是否覆盖完整，并确认配置时包含：

```text
-DXQC_ENABLE_MINI_API=ON
```

然后检查：

```bash
grep 'add_executable(netins_udp_mini_api_client' mini/CMakeLists.txt
grep 'add_executable(netins_udp_mini_api_server' mini/CMakeLists.txt
grep 'add_executable(netins_udp_stream_client' mini/CMakeLists.txt
grep 'add_executable(netins_udp_stream_server' mini/CMakeLists.txt
```

### 编译或链接找不到 `netins_udp_stream_*` API

通常是源码目录没有完整同步，或构建目录仍引用另一套头文件、静态库和 CMake 配置。确认
`include/`、`mini/`、`src/`、`demo/`、`cmake/`、`scripts/` 与当前交付源码一致，并使用为
当前源码新建的构建目录重新配置。

### `file format not recognized` 或 `Relocations in generic ELF`

客户端依赖必须全部为 AARCH64，服务端依赖必须全部为 x86_64。调整依赖路径后，使用新的构建
目录重新配置。

### `relocation ... recompile with -fPIC`

Tongsuo/BabaSSL、Libevent 或 zlog 中至少一个静态库没有使用 `-fPIC` 编译，需要重新编译
对应依赖。

### 两端能够启动，但连接后报告协议错误

确认 Client 和 Server 均使用当前交付源码重新构建，并检查两端的
`MINI_STREAM_PROTOCOL_VERSION` 都是 `4U`。开发中间版本或更早的 Stream 协议不能与当前版本
混用。

## 10. 最终检查清单

- [ ] Client 和 Server 的源码目录均已同步到当前交付版本；
- [ ] 两端 `NETINS_UDP_API_RELEASE_VERSION` 均为 `6.5`；
- [ ] 两端 `MINI_STREAM_PROTOCOL_VERSION` 均为 `4U`；
- [ ] 客户端生成文件 Demo 和 Stream Demo 两个 AARCH64 可执行文件；
- [ ] 服务端生成文件 Demo 和 Stream Demo 两个 x86_64 可执行文件；
- [ ] 两端 `ldd` 均没有 `not found`；
- [ ] Stream POST 和 GET 各完成一次，两端均输出 `success=1`；
- [ ] 文件 Demo 仍可按原命令完成文件传输。
