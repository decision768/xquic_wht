# NetIns UDP Mini API 编译指南

本文对应当前 `5.4.2-mini-api.1`：

- 客户端：AARCH64（SIM8810 交叉编译，或直接在 AARCH64 Linux 主机编译）；
- 服务端：原生 Linux x86_64；
- 客户端和服务端必须分别使用独立构建目录，且 TLS、Libevent、zlog 必须与各自目标架构一致。

不要把旧的 `CMakeLists_aarch64.txt` 或 `CMakeLists_linux.txt` 覆盖到当前源码树。两份旧文件
只有 BabaSSL 静态库目录层级不同，当前工程使用同一份 `CMakeLists.txt` 加不同参数构建。

## 1. 依赖

- CMake 3.15 或更高；
- GCC/G++ 和 GNU Make；
- Tongsuo/BabaSSL 8.3；
- Libevent 2.1，包含 `libevent.a` 和 `libevent_pthreads.a`；
- zlog 1.2，包含 `zlog.h` 和 `libzlog.a`；
- Linux pthread、dl 和 math 库；
- 打包/ABI 检查需要 Python 3、`nm`、`tar` 和 `sha256sum`。

所有静态依赖应使用 `-fPIC` 编译，否则生成 `libxquic.so` 时可能出现
`relocation ... can not be used when making a shared object`。

参考旧配置后，可复用的路径如下：

| 用途 | 路径 |
|---|---|
| SIM8810 SDK | `/home/admin123/SIM8810_SDK_OCPU_LE3.0_1.1.0_251207` |
| AARCH64 工具链 | `/opt/crosstools/toolchain/gcc/usr/bin/aarch64-oe-linux` |
| AARCH64 Tongsuo | 源码目录相邻的 `../third_party/Tongsuo-8.3.0` |
| AARCH64 Libevent | `/home/admin123/libevent_aarch64/libevent-2.1.12-stable` |
| AARCH64 zlog 头文件 | `/home/admin123/zlog-1.2.18/src` |
| AARCH64 zlog 静态库 | `/home/admin123/zlog-1.2.18/build_aarch64/lib/libzlog.a` |
| Linux x86_64 BabaSSL | `/usr/local/babassl` |
| Linux x86_64 Libevent | `/data/quic/libevent-2.1.12-stable` |

两份旧配置没有给出 Linux x86_64 的 zlog 路径，必须按服务器实际路径填写，不能复用
AARCH64 的 `libzlog.a`。

## 2. 编译前检查

在源码根目录执行：

```bash
pwd
cmake --version
python3 --version
```

检查 AARCH64 依赖：

```bash
ARM_SDK=/home/admin123/SIM8810_SDK_OCPU_LE3.0_1.1.0_251207
ARM_TC=/opt/crosstools/toolchain/gcc/usr/bin/aarch64-oe-linux
ARM_TLS="$PWD/../third_party/Tongsuo-8.3.0"
ARM_EVENT=/home/admin123/libevent_aarch64/libevent-2.1.12-stable
ARM_ZLOG=/home/admin123/zlog-1.2.18

# 同时导出工具链输入，可兼容尚未加入 try_compile 变量传递修复的旧工具链文件。
export SIM8810_SDK_ROOT="$ARM_SDK"
export SIM8810_TOOLCHAIN_DIR="$ARM_TC"

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

检查 Linux x86_64 依赖。先把 `LINUX_ZLOG` 改成服务器上的真实目录：

```bash
LINUX_TLS=/usr/local/babassl
LINUX_EVENT=/data/quic/libevent-2.1.12-stable
LINUX_ZLOG=/实际路径/zlog-1.2.18

: "${LINUX_TLS:?LINUX_TLS 未设置}"
: "${LINUX_EVENT:?LINUX_EVENT 未设置}"
: "${LINUX_ZLOG:?LINUX_ZLOG 未设置}"

test "$(uname -m)" = x86_64
test -x /usr/bin/gcc
test -x /usr/bin/g++
test -f "$LINUX_TLS/include/openssl/ssl.h"
test -f "$LINUX_EVENT/include/event2/event.h"
test -f "$LINUX_EVENT/.libs/libevent.a"
test -f "$LINUX_EVENT/.libs/libevent_pthreads.a"
test -f "$LINUX_ZLOG/src/zlog.h"
test -f "$LINUX_ZLOG/build/lib/libzlog.a"
```

Linux 参考 CMake 把 `libssl.a`、`libcrypto.a` 放在 `/usr/local/babassl` 根目录；部分安装则
位于 `/usr/local/babassl/lib`。可用下面的命令确认：

```bash
find "$LINUX_TLS" -maxdepth 2 \
  \( -name libssl.a -o -name libcrypto.a \) -print
```

下文按旧 Linux 配置的根目录布局编写。如果实际位于 `lib/`，把对应两个参数改成
`$LINUX_TLS/lib/libssl.a` 和 `$LINUX_TLS/lib/libcrypto.a`。

## 3. AARCH64 客户端：SIM8810 交叉编译

必须使用全新的构建目录：

```bash
rm -rf build-mini-api-aarch64

cmake -S . -B build-mini-api-aarch64 \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/sim8810-aarch64.cmake" \
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

cmake --build build-mini-api-aarch64 \
  --target netins_udp_mini_api_client \
  --parallel "$(nproc)"
```

客户端产物：

```text
build-mini-api-aarch64/mini/netins_udp_mini_api_client
build-mini-api-aarch64/libxquic-static.a
```

验证目标架构：

```bash
file build-mini-api-aarch64/mini/netins_udp_mini_api_client
"$ARM_TC/aarch64-oe-linux-readelf" -h \
  build-mini-api-aarch64/mini/netins_udp_mini_api_client | grep Machine
```

结果应包含 `ARM aarch64` 或 `AArch64`。交叉编译生成的程序应复制到 SIM8810/AARCH64
目标机运行，不能直接在 x86_64 编译主机执行。

### 直接在 AARCH64 Linux 主机编译

如果不是交叉编译，而是在 AARCH64 机器上直接编译，不传工具链文件，并显式指定本机编译器：

```bash
rm -rf build-mini-api-aarch64-native

cmake -S . -B build-mini-api-aarch64-native \
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

cmake --build build-mini-api-aarch64-native \
  --target netins_udp_mini_api_client \
  --parallel "$(nproc)"
```

当前根 CMake 在未指定编译器时会采用 SIM8810 默认交叉工具链，所以原生 AARCH64 和
原生 x86_64 构建都不能省略 `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER`。

## 4. Linux x86_64 服务端：原生编译

根据上一节 `find` 的结果设置服务器 TLS 静态库路径：

```bash
LINUX_SSL_LIB="$LINUX_TLS/libssl.a"
LINUX_CRYPTO_LIB="$LINUX_TLS/libcrypto.a"
```

如果库实际在 `lib/` 子目录，则使用：

```bash
LINUX_SSL_LIB="$LINUX_TLS/lib/libssl.a"
LINUX_CRYPTO_LIB="$LINUX_TLS/lib/libcrypto.a"
```

配置和编译：

```bash
: "${LINUX_TLS:?请先设置并检查 LINUX_TLS}"
: "${LINUX_SSL_LIB:?请先设置并检查 LINUX_SSL_LIB}"
: "${LINUX_CRYPTO_LIB:?请先设置并检查 LINUX_CRYPTO_LIB}"
: "${LINUX_EVENT:?请先设置并检查 LINUX_EVENT}"
: "${LINUX_ZLOG:?请先设置并检查 LINUX_ZLOG}"

rm -rf build-mini-api-linux

cmake -S . -B build-mini-api-linux \
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

cmake --build build-mini-api-linux \
  --target netins_udp_mini_api_server \
  --parallel "$(nproc)"
```

服务端产物：

```text
build-mini-api-linux/mini/netins_udp_mini_api_server
build-mini-api-linux/libxquic-static.a
```

验证：

```bash
file build-mini-api-linux/mini/netins_udp_mini_api_server
readelf -h build-mini-api-linux/mini/netins_udp_mini_api_server | grep Machine
ldd build-mini-api-linux/mini/netins_udp_mini_api_server
```

`Machine` 应为 `Advanced Micro Devices X86-64`，`ldd` 不应出现 `not found`。

## 5. 完整 Mini API 库与交付包

只构建上述单个 demo 时，CMake 会生成它依赖的 `libxquic-static.a`，但不会保证共享库和
另一个 demo 已生成。需要 SDK/正式交付包时，对相应架构执行全量构建：

```bash
cmake --build build-mini-api-aarch64 --parallel "$(nproc)"
cmake --build build-mini-api-linux --parallel "$(nproc)"
```

全量产物包括：

```text
libxquic-static.a
libxquic.so
mini/netins_udp_mini_api_client
mini/netins_udp_mini_api_server
```

分别安装，禁止两个架构安装到同一目录：

```bash
cmake --install build-mini-api-aarch64 \
  --prefix "$PWD/stage/mini-api-aarch64" \
  --component mini_api

cmake --install build-mini-api-linux \
  --prefix "$PWD/stage/mini-api-linux-x86_64" \
  --component mini_api
```

ABI 检查：

```bash
python3 scripts/mini_api/check_abi.py \
  --library build-mini-api-linux/libxquic.so
```

正式打包：

```bash
sh scripts/mini_api/package_delivery.sh \
  build-mini-api-aarch64 "$PWD/delivery/aarch64"

sh scripts/mini_api/package_delivery.sh \
  build-mini-api-linux "$PWD/delivery/linux-x86_64"
```

每个目录会生成：

```text
netins-mini-api-5.4.2-mini-api.1.tar.gz
netins-mini-api-5.4.2-mini-api.1.tar.gz.sha256
```

## 6. 常见错误

- `C compiler ... is not a full path`：SIM8810 工具链目录错误，检查
  `SIM8810_TOOLCHAIN_DIR`。
- `try_compile` 中再次提示 `Set SIM8810_SDK_ROOT`：工具链输入没有传入 CMake 的编译器
  探测子工程。使用当前 `cmake/toolchains/sim8810-aarch64.cmake`，或在配置前执行：
  `export SIM8810_SDK_ROOT="$ARM_SDK"` 和
  `export SIM8810_TOOLCHAIN_DIR="$ARM_TC"`。修正后必须删除旧构建目录再配置。
- `ssl lib not found`：`libssl.a`/`libcrypto.a` 实际在根目录还是 `lib/` 与参数不一致。
- `file format not recognized` 或 `Relocations in generic ELF`：混用了 AARCH64 和 x86_64
  静态库；删除整个构建目录后，用同架构依赖重新配置。
- `cannot find -levent_pthreads`：需要构建启用 pthread 的 Libevent，并传入
  `libevent_pthreads.a` 的完整路径。
- `cannot find zlog.h`：`ZLOG_INCLUDE_DIR` 应指向直接包含 `zlog.h` 的目录；旧 AARCH64
  安装对应 `/home/admin123/zlog-1.2.18/src`。
- `relocation ... recompile with -fPIC`：对应静态依赖没有使用 `-fPIC`，需重新编译该依赖。
- 修改编译器、sysroot 或任何依赖架构后，不要复用 `CMakeCache.txt`；直接使用新的
  `build-mini-api-*` 目录。

## 7. 第三方链接

共享库：

```bash
cc app.c -Iinclude -Llib -lxquic -o app
```

静态库通常还需显式链接同架构依赖：

```bash
cc app.c -Iinclude -Llib -lxquic-static \
  -levent_pthreads -levent -lzlog -lssl -lcrypto \
  -ldl -lpthread -lm -o app
```

具体搜索路径和链接器应替换为目标架构对应版本；AARCH64 交叉编译时应使用
`aarch64-oe-linux-gcc`。
