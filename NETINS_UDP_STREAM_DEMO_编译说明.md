# NetIns UDP 6.4 数据流 Demo 编译说明

本文用于把原 6.3 编译环境升级为 6.4 数据流 Demo 编译环境，目标产物为：

```text
netins_udp_stream_client
netins_udp_stream_server
```

当前基准信息：

| 项目 | 值 |
|---|---|
| 6.4 源码基线 | `codex/data-stream-api` 单向完善版交付提交 |
| API Release | `6.4` |
| 内部 Stream 协议 | v3 |
| Demo lane 数 | 固定 4 lane，不提供运行时修改参数 |

## 1. 6.3 环境哪些可以复用

可以继续复用两端原 6.3 环境中的：

- 编译器和交叉编译工具链；
- SIM8810 SDK 和 sysroot；
- 同架构的 Tongsuo/BabaSSL；
- 同架构的 Libevent；
- 同架构的 zlog；
- 原来的服务器端口、路由和运行环境。

必须更新的内容：

- 客户端和服务端都要使用完整的 6.4 源码；
- 两端都要重新生成 CMake 构建目录；
- 两端都要重新编译 Demo 及其依赖的 `libxquic-static.a`；
- 两端必须使用同一版 6.4 Stream 协议，不能混用旧数据流实验版本。

不能只把 `demo/netins_udp_stream_client.c` 和
`demo/netins_udp_stream_server.c` 复制到 6.3 源码中单独编译。数据流实现还涉及公共头文件、
Mini backend、H3 Bytestream、Stream 分流/重排协议、导出符号和 CMake 目标。只替换 Demo 文件会
出现缺少 API、链接失败或协议不一致。

## 2. 通过文件覆盖升级到 6.4

客户端和服务端编译机不需要、也不使用 Git。升级方式是在当前电脑准备好 6.4 源码文件，
然后通过现有远程文件传输方式分别上传到两台编译机，保持相对路径覆盖原 6.3 源码。

### 2.1 覆盖前备份

先在两台编译机上复制一份原 6.3 源码目录作为备份。不要覆盖原 6.3 的第三方依赖目录，
也不要删除原 6.3 的可执行文件；它们可以继续用于回退和对照。

建议目录关系：

```text
/实际路径/xquic-6.3-backup/       原 6.3 备份
/实际路径/xquic-stream-api-6.4/   接收 6.4 文件并用于新编译
```

如果编译脚本必须使用原来的固定源码路径，也可以先完成备份，再把 6.4 文件按相对路径覆盖到
原源码目录。6.3 到 6.4 没有必须删除的源码文件，因此允许直接覆盖同名文件并添加新文件。

### 2.2 推荐覆盖范围

最稳妥的方式是上传当前 6.4 源码树中的全部源代码和构建配置，覆盖编译机上的对应文件，
但不要上传下列本机内容：

```text
.git/
build*/
stage/
delivery/
.vscode/
*.o
*.a
*.so
```

不能只上传两个 Stream Demo 文件。至少必须同步以下 6.4 内容：

| 相对路径 | 操作 | 原因 |
|---|---|---|
| `CMakeLists.txt`、`mini/CMakeLists.txt` | 覆盖 | 增加 Stream 构建目标和 backend 对象 |
| `VERSION` | 覆盖 | Release 标识更新为 6.4 |
| `cmake/MiniApiInstall.cmake` | 覆盖 | 增加 Stream 产物安装规则 |
| `demo/` | 用 6.4 内容覆盖并补充新文件 | 包含 Client、Server 和固定 4-lane Demo 配置 |
| `include/xquic/` | 用 6.4 内容覆盖 | 包含公开 Stream API 和 H3 Bytestream 声明 |
| `mini/` | 用 6.4 内容覆盖并补充新文件 | 包含分流、重排、CREDIT、断链和恢复实现 |
| `src/http3/` | 用 6.4 内容覆盖 | 包含底层 Bytestream 接入修改 |
| `src/netins_udp/` | 用 6.4 内容覆盖 | 包含公开 Stream API 实现 |
| `scripts/xquic.lds` | 覆盖 | 导出新增公开 API 符号 |
| `scripts/mini_api/` | 用 6.4 内容覆盖并补充新文件 | 提供 ABI 和隔离检查 |
| `tests/` | 用 6.4 内容覆盖并补充新文件 | 提供协议、契约和验收测试 |

如果使用“完整 6.4 源码树覆盖”，上表会自然全部满足，不需要逐个挑选文件。

### 2.3 覆盖后确认

在客户端和服务端编译机上分别进入被覆盖后的源码根目录：

```bash
cd /实际路径/xquic-stream-api-6.4

cat VERSION
grep 'NETINS_UDP_API_RELEASE_VERSION' include/xquic/netins_udp.h
grep 'MINI_STREAM_PROTOCOL_VERSION' mini/mini_stream_protocol.h
test -f demo/netins_udp_stream_client.c
test -f demo/netins_udp_stream_server.c
grep 'add_executable(netins_udp_stream_client' mini/CMakeLists.txt
grep 'add_executable(netins_udp_stream_server' mini/CMakeLists.txt
```

关键输出必须包含：

```text
6.4
#define NETINS_UDP_API_RELEASE_VERSION "6.4"
#define MINI_STREAM_PROTOCOL_VERSION 3U
```

只要任意一项不匹配，就说明文件没有完整覆盖，此时不要开始编译。

## 3. 构建目录要求

不要使用原来的 6.3 `build` 目录，也不要把旧的 `CMakeCache.txt` 复制过来。建议使用全新的：

```text
build-stream-client-aarch64-6.4
build-stream-server-linux-6.4
```

原因是旧缓存保存了源码路径、编译器、目标架构、依赖库路径和旧目标列表。复用旧缓存容易把
6.3 头文件、静态库或 CMake 配置混入 6.4 二进制。

不要使用旧的 `CMakeLists_aarch64.txt` 或 `CMakeLists_linux.txt` 覆盖 6.4 根目录中的
`CMakeLists.txt`。当前版本通过同一份 CMake 工程和不同参数构建两种架构。

## 4. 编译 AARCH64 Stream Client

以下路径来自原 6.3 编译环境。如果实际路径不同，只修改变量值，不修改 6.4 源码。

### 4.1 SIM8810 交叉编译

```bash
cd /实际路径/xquic-stream-api-6.4
STREAM_SRC="$PWD"

STREAM_ARM_SDK=/home/admin123/SIM8810_SDK_OCPU_LE3.0_1.1.0_251207
STREAM_ARM_TC=/opt/crosstools/toolchain/gcc/usr/bin/aarch64-oe-linux
STREAM_ARM_TLS="$STREAM_SRC/../third_party/Tongsuo-8.3.0"
STREAM_ARM_EVENT=/home/admin123/libevent_aarch64/libevent-2.1.12-stable
STREAM_ARM_ZLOG=/home/admin123/zlog-1.2.18
STREAM_CLIENT_BUILD="$STREAM_SRC/build-stream-client-aarch64-6.4"

export SIM8810_SDK_ROOT="$STREAM_ARM_SDK"
export SIM8810_TOOLCHAIN_DIR="$STREAM_ARM_TC"

test -d "$STREAM_ARM_SDK/sysroots_pvm"
test -x "$STREAM_ARM_TC/aarch64-oe-linux-gcc"
test -f "$STREAM_ARM_TLS/include/openssl/ssl.h"
test -f "$STREAM_ARM_TLS/libssl.a"
test -f "$STREAM_ARM_TLS/libcrypto.a"
test -f "$STREAM_ARM_EVENT/include/event2/event.h"
test -f "$STREAM_ARM_EVENT/.libs/libevent.a"
test -f "$STREAM_ARM_EVENT/.libs/libevent_pthreads.a"
test -f "$STREAM_ARM_ZLOG/src/zlog.h"
test -f "$STREAM_ARM_ZLOG/build_aarch64/lib/libzlog.a"

cmake -S "$STREAM_SRC" -B "$STREAM_CLIENT_BUILD" \
  -DCMAKE_TOOLCHAIN_FILE="$STREAM_SRC/cmake/toolchains/sim8810-aarch64.cmake" \
  -DSIM8810_SDK_ROOT="$STREAM_ARM_SDK" \
  -DSIM8810_TOOLCHAIN_DIR="$STREAM_ARM_TC" \
  -DCMAKE_BUILD_TYPE=Release \
  -DXQC_ENABLE_MINI_API=ON \
  -DXQC_ENABLE_TESTING=OFF \
  -DSSL_TYPE=babassl \
  -DSSL_INC_PATH="$STREAM_ARM_TLS/include" \
  -DSSL_LIB_PATH="$STREAM_ARM_TLS/libssl.a;$STREAM_ARM_TLS/libcrypto.a" \
  -DLIBEVENT_INCLUDE_DIR="$STREAM_ARM_EVENT/include" \
  -DLIBEVENT_LIBRARY="$STREAM_ARM_EVENT/.libs/libevent.a" \
  -DLIBEVENT_PTHREADS_LIBRARY="$STREAM_ARM_EVENT/.libs/libevent_pthreads.a" \
  -DZLOG_INCLUDE_DIR="$STREAM_ARM_ZLOG/src" \
  -DZLOG_STATIC_LIB="$STREAM_ARM_ZLOG/build_aarch64/lib/libzlog.a"

cmake --build "$STREAM_CLIENT_BUILD" \
  --target netins_udp_stream_client \
  --parallel "$(nproc)"
```

客户端产物：

```text
build-stream-client-aarch64-6.4/mini/netins_udp_stream_client
build-stream-client-aarch64-6.4/libxquic-static.a
```

检查架构：

```bash
file "$STREAM_CLIENT_BUILD/mini/netins_udp_stream_client"
"$STREAM_ARM_TC/aarch64-oe-linux-readelf" -h \
  "$STREAM_CLIENT_BUILD/mini/netins_udp_stream_client" | grep Machine
```

结果必须包含 `ARM aarch64` 或 `AArch64`。该程序要上传到 AARCH64/SIM8810 目标机运行，
不能在 x86_64 交叉编译主机上直接运行。

### 4.2 在 AARCH64 Linux 主机上原生编译

如果原 6.3 客户端就是在 AARCH64 主机原生编译，保留同架构依赖路径，使用新的构建目录：

```bash
cd /实际路径/xquic-stream-api-6.4
STREAM_SRC="$PWD"

STREAM_ARM_TLS="$STREAM_SRC/../third_party/Tongsuo-8.3.0"
STREAM_ARM_EVENT=/home/admin123/libevent_aarch64/libevent-2.1.12-stable
STREAM_ARM_ZLOG=/home/admin123/zlog-1.2.18
STREAM_CLIENT_BUILD="$STREAM_SRC/build-stream-client-aarch64-native-6.4"

cmake -S "$STREAM_SRC" -B "$STREAM_CLIENT_BUILD" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DXQC_ENABLE_MINI_API=ON \
  -DXQC_ENABLE_TESTING=OFF \
  -DSSL_TYPE=babassl \
  -DSSL_INC_PATH="$STREAM_ARM_TLS/include" \
  -DSSL_LIB_PATH="$STREAM_ARM_TLS/libssl.a;$STREAM_ARM_TLS/libcrypto.a" \
  -DLIBEVENT_INCLUDE_DIR="$STREAM_ARM_EVENT/include" \
  -DLIBEVENT_LIBRARY="$STREAM_ARM_EVENT/.libs/libevent.a" \
  -DLIBEVENT_PTHREADS_LIBRARY="$STREAM_ARM_EVENT/.libs/libevent_pthreads.a" \
  -DZLOG_INCLUDE_DIR="$STREAM_ARM_ZLOG/src" \
  -DZLOG_STATIC_LIB="$STREAM_ARM_ZLOG/build_aarch64/lib/libzlog.a"

cmake --build "$STREAM_CLIENT_BUILD" \
  --target netins_udp_stream_client \
  --parallel "$(nproc)"
```

原生编译不要传入 `CMAKE_TOOLCHAIN_FILE`、`SIM8810_SDK_ROOT` 和
`SIM8810_TOOLCHAIN_DIR`。

交叉编译产物上传到 AARCH64 目标机后，可在目标机检查运行时依赖：

```bash
chmod +x ./netins_udp_stream_client
ldd ./netins_udp_stream_client
```

若目标系统提供 `ldd`，输出不应包含 `not found`。

## 5. 编译 Linux x86_64 Stream Server

先确认服务端原 6.3 环境中的实际依赖路径：

```bash
cd /实际路径/xquic-stream-api-6.4
STREAM_SRC="$PWD"

STREAM_LINUX_TLS=/usr/local/babassl
STREAM_LINUX_EVENT=/data/quic/libevent-2.1.12-stable
STREAM_LINUX_ZLOG=/实际路径/zlog-1.2.18
STREAM_SERVER_BUILD="$STREAM_SRC/build-stream-server-linux-6.4"

find "$STREAM_LINUX_TLS" -maxdepth 2 \
  \( -name libssl.a -o -name libcrypto.a \) -print
```

如果静态库位于 BabaSSL 根目录：

```bash
STREAM_LINUX_SSL_LIB="$STREAM_LINUX_TLS/libssl.a"
STREAM_LINUX_CRYPTO_LIB="$STREAM_LINUX_TLS/libcrypto.a"
```

如果静态库位于 `lib/` 子目录：

```bash
STREAM_LINUX_SSL_LIB="$STREAM_LINUX_TLS/lib/libssl.a"
STREAM_LINUX_CRYPTO_LIB="$STREAM_LINUX_TLS/lib/libcrypto.a"
```

检查依赖并编译：

```bash
test "$(uname -m)" = x86_64
test -x /usr/bin/gcc
test -x /usr/bin/g++
test -f "$STREAM_LINUX_TLS/include/openssl/ssl.h"
test -f "$STREAM_LINUX_SSL_LIB"
test -f "$STREAM_LINUX_CRYPTO_LIB"
test -f "$STREAM_LINUX_EVENT/include/event2/event.h"
test -f "$STREAM_LINUX_EVENT/.libs/libevent.a"
test -f "$STREAM_LINUX_EVENT/.libs/libevent_pthreads.a"
test -f "$STREAM_LINUX_ZLOG/src/zlog.h"
test -f "$STREAM_LINUX_ZLOG/build/lib/libzlog.a"

cmake -S "$STREAM_SRC" -B "$STREAM_SERVER_BUILD" \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DXQC_ENABLE_MINI_API=ON \
  -DXQC_ENABLE_TESTING=OFF \
  -DSSL_TYPE=babassl \
  -DSSL_INC_PATH="$STREAM_LINUX_TLS/include" \
  -DSSL_LIB_PATH="$STREAM_LINUX_SSL_LIB;$STREAM_LINUX_CRYPTO_LIB" \
  -DLIBEVENT_INCLUDE_DIR="$STREAM_LINUX_EVENT/include" \
  -DLIBEVENT_LIBRARY="$STREAM_LINUX_EVENT/.libs/libevent.a" \
  -DLIBEVENT_PTHREADS_LIBRARY="$STREAM_LINUX_EVENT/.libs/libevent_pthreads.a" \
  -DZLOG_INCLUDE_DIR="$STREAM_LINUX_ZLOG/src" \
  -DZLOG_STATIC_LIB="$STREAM_LINUX_ZLOG/build/lib/libzlog.a"

cmake --build "$STREAM_SERVER_BUILD" \
  --target netins_udp_stream_server \
  --parallel "$(nproc)"
```

服务端产物：

```text
build-stream-server-linux-6.4/mini/netins_udp_stream_server
build-stream-server-linux-6.4/libxquic-static.a
```

检查架构和动态依赖：

```bash
file "$STREAM_SERVER_BUILD/mini/netins_udp_stream_server"
readelf -h "$STREAM_SERVER_BUILD/mini/netins_udp_stream_server" | grep Machine
ldd "$STREAM_SERVER_BUILD/mini/netins_udp_stream_server"
```

`Machine` 应为 `Advanced Micro Devices X86-64`，`ldd` 不应出现 `not found`。

## 6. 可选：同时编译测试目标和共享库

只构建 Client 或 Server Demo 时，CMake 会自动构建它依赖的 `libxquic-static.a`。如果还要
执行完整的同机自动验收，建议在 Linux x86_64 服务端编译目录执行全量构建：

```bash
cmake --build "$STREAM_SERVER_BUILD" --parallel "$(nproc)"
```

或只增加这些测试目标：

```bash
cmake --build "$STREAM_SERVER_BUILD" --target \
  netins_udp_stream_client \
  mini_stream_protocol_test \
  mini_stream_reorder_test \
  netins_udp_stream_api_contract_test \
  xquic \
  --parallel "$(nproc)"
```

源码静态检查：

```bash
cd "$STREAM_SRC"
python3 scripts/mini_api/check_stream_isolation.py --root .
python3 scripts/mini_api/check_abi.py \
  --header include/xquic/netins_udp.h \
  --version-script scripts/xquic.lds
```

预期分别为：

```text
STREAM ISOLATION CHECK: PASS (49 invariants)
ABI CHECK: PASS (28 public functions)
```

## 7. 编译完成后的最小启动检查

先在云端服务器启动 64 MiB 接收测试：

```bash
./netins_udp_stream_server 0.0.0.0 18460 67108864 7 \
  8388608 600000
```

服务端应输出：

```text
[stream_api_ready] listen=0.0.0.0 port=18460
[stream_api_config] role=server lanes=4 ...
```

然后在 AARCH64 客户端运行。把 `SERVER_IP` 和接口名换成真实值：

```bash
./netins_udp_stream_client SERVER_IP 18460 67108864 7 \
  8388608 8388608 0 600000 600000 \
  --interface rmnet_data0 --interface usb0 --scheduler adaptive
```

说明：

- Demo 固定使用 4 lane，命令中没有 lane 数参数；
- `--interface` 可以重复，单网卡测试只传一个，双网卡聚合测试传两个；
- `--scheduler` 不传时也默认使用 `adaptive`；
- Client 和 Server 的 `BYTES`、`SEED` 必须一致；
- 每轮服务端默认接收一个客户端，下一轮测试前需要重新启动服务端；
- 测试数据由 Client Demo 按 seed 实时生成，不需要准备输入文件。

成功时两端都应出现：

```text
[stream_api_result] ... success=1 ...
```

## 8. 常见问题判断

### CMake 提示没有 `netins_udp_stream_client/server` 目标

当前源码仍是 6.3，或者配置时关闭了 Mini API。执行：

```bash
cat VERSION
grep 'NETINS_UDP_API_RELEASE_VERSION' include/xquic/netins_udp.h
grep 'MINI_STREAM_PROTOCOL_VERSION' mini/mini_stream_protocol.h
grep 'add_executable(netins_udp_stream_client' mini/CMakeLists.txt
```

必须确认 Release 为 `6.4`、Stream 协议为 `3U`、CMake 中存在 Stream Demo 目标，并配置
`-DXQC_ENABLE_MINI_API=ON`。缺少任一项都表示替换不完整。

### 编译或链接提示找不到 `netins_udp_stream_*` API

通常是混用了 6.3 头文件、6.3 静态库或旧 CMake 缓存。不要单独手工编译 Demo；换成完整
6.4 源码并使用全新构建目录。

### `file format not recognized` 或 `Relocations in generic ELF`

混用了 AARCH64 和 x86_64 依赖。客户端所有静态依赖必须为 AARCH64，服务端所有静态依赖
必须为 x86_64。修正路径后重新创建构建目录。

### `relocation ... recompile with -fPIC`

Tongsuo/BabaSSL、Libevent 或 zlog 中至少一个静态库没有使用 `-fPIC` 编译，需要重新编译该
依赖。原 6.3 若已经能够生成 `libxquic.so`，通常不需要重新处理。

### 两端能够启动，但连接后报告协议错误

确认 Client 和 Server 都完成了相同的 6.4 文件覆盖。6.4 内部 Stream 协议为 v3，不能与
早期 Stream POC 或中间开发版本混用。

## 9. 最终检查清单

- [ ] Client 和 Server 源码的 `VERSION` 都是 `6.4`；
- [ ] 两端头文件中的 Release 都是 `6.4`；
- [ ] 两端 `MINI_STREAM_PROTOCOL_VERSION` 都是 `3U`；
- [ ] 两端都存在 Stream Client/Server CMake 目标；
- [ ] 没有复用 6.3 的 CMake 构建目录；
- [ ] Client 二进制为 AARCH64；
- [ ] Server 二进制为 x86_64；
- [ ] 两端 `ldd` 均没有 `not found`；
- [ ] 启动日志显示 `lanes=4`；
- [ ] 两端最终输出均为 `success=1`。
