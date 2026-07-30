# NetIns UDP SDK 编译指南

本文说明如何在Linux x86_64服务器和SIM8810 AArch64板卡环境编译
NetIns UDP SDK。两个环境必须分别使用对应架构的编译器、TLS库和依赖库。

## 1. 通用要求

需要准备：

- CMake 3.15或更高版本；
- GNU Make；
- C/C++编译器；
- Tongsuo/BabaSSL静态库；
- Linux pthread、dl和m系统库；
- AArch64构建需要SIM8810 SDK和交叉编译器。

每次更换源码、编译器、sysroot或TLS库后，应创建新的构建目录，不要复用其他
架构的 `CMakeCache.txt`。

## 2. 检查依赖路径

### 2.1 TLS

```sh
test -f "$NETINS_SSL_ROOT/include/openssl/ssl.h" &&
echo "SSL headers: OK"

test -f "$NETINS_SSL_ROOT/libssl.a" &&
echo "libssl.a: OK"

test -f "$NETINS_SSL_ROOT/libcrypto.a" &&
echo "libcrypto.a: OK"
```

如果库位于 `lib/` 子目录，应把后续命令中的库路径改成：

```text
$NETINS_SSL_ROOT/lib/libssl.a
$NETINS_SSL_ROOT/lib/libcrypto.a
```

### 2.2 查看静态库架构

```sh
file "$NETINS_SSL_ROOT/libssl.a"
file "$NETINS_SSL_ROOT/libcrypto.a"
```

也可以使用目标工具链的 `objdump`：

```sh
"$NETINS_TOOLCHAIN_DIR/aarch64-oe-linux-objdump" \
  -f "$NETINS_SSL_ROOT/libssl.a" |
grep architecture |
sort -u
```

## 3. Linux x86_64服务器

进入交付源码目录，并设置服务器上的实际TLS路径：

```sh
cd /path/to/netins-udp-sdk

export NETINS_SSL_ROOT="/path/to/x86_64/Tongsuo-8.3.0"
```

配置：

```sh
cmake -S . -B build-api-server-x86_64 \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/linux-x86_64.cmake" \
  -DSSL_TYPE=babassl \
  -DSSL_PATH="$NETINS_SSL_ROOT" \
  -DSSL_INC_PATH="$NETINS_SSL_ROOT/include" \
  -DSSL_LIB_PATH="$NETINS_SSL_ROOT/libssl.a;$NETINS_SSL_ROOT/libcrypto.a" \
  -DXQC_ENABLE_BBR2=OFF \
  -DXQC_ENABLE_RENO=OFF \
  -DNETINS_UDP_BUILD_DATA_DEMO=ON
```

如果Tongsuo静态库位于 `lib/` 目录，将 `SSL_LIB_PATH` 改为：

```sh
-DSSL_LIB_PATH="$NETINS_SSL_ROOT/lib/libssl.a;$NETINS_SSL_ROOT/lib/libcrypto.a"
```

编译服务端Demo：

```sh
cmake --build build-api-server-x86_64 \
  --target netins_udp_data_server \
  -j$(nproc)
```

也可以编译SDK全部默认目标：

```sh
cmake --build build-api-server-x86_64 -j$(nproc)
```

检查产物：

```sh
file build-api-server-x86_64/netins_udp_data_server
file build-api-server-x86_64/libxquic-static.a
```

x86_64可执行文件应显示 `ELF 64-bit` 和 `x86-64`。

## 4. SIM8810 AArch64客户端

设置实际路径：

```sh
cd /path/to/netins-udp-sdk

export NETINS_SIM_SDK="/path/to/SIM8810_SDK"
export NETINS_TOOLCHAIN_DIR="/path/to/aarch64-oe-linux/toolchain/bin"
export NETINS_SSL_ROOT="/path/to/aarch64/Tongsuo"
```

检查工具链：

```sh
test -d "$NETINS_SIM_SDK/sysroots_pvm" &&
echo "SIM8810 sysroot: OK"

test -x "$NETINS_TOOLCHAIN_DIR/aarch64-oe-linux-gcc" &&
echo "AArch64 gcc: OK"

"$NETINS_TOOLCHAIN_DIR/aarch64-oe-linux-gcc" --version
```

配置：

```sh
cmake -S . -B build-api-client-aarch64-final \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/sim8810-aarch64.cmake" \
  -DXQC_SIM8810_SDK_ROOT="$NETINS_SIM_SDK" \
  -DXQC_SIM8810_TOOLCHAIN_DIR="$NETINS_TOOLCHAIN_DIR" \
  -DSSL_TYPE=babassl \
  -DSSL_PATH="$NETINS_SSL_ROOT" \
  -DSSL_INC_PATH="$NETINS_SSL_ROOT/include" \
  -DSSL_LIB_PATH="$NETINS_SSL_ROOT/libssl.a;$NETINS_SSL_ROOT/libcrypto.a" \
  -DXQC_ENABLE_BBR2=OFF \
  -DXQC_ENABLE_RENO=OFF \
  -DNETINS_UDP_BUILD_DATA_DEMO=ON
```

编译客户端Demo：

```sh
cmake --build build-api-client-aarch64-final \
  --target netins_udp_data_client \
  -j$(nproc)
```

检查产物：

```sh
file build-api-client-aarch64-final/netins_udp_data_client

LC_ALL=C readelf -h \
  build-api-client-aarch64-final/netins_udp_data_client |
grep Machine
```

正确结果应包含：

```text
ARM aarch64
Machine: AArch64
```

## 5. 安装SDK

x86_64示例：

```sh
cmake --install build-api-server-x86_64 \
  --prefix "$PWD/install-x86_64"
```

AArch64示例：

```sh
cmake --install build-api-client-aarch64-final \
  --prefix "$PWD/install-aarch64"
```

安装目录包括：

```text
include/xquic/*.h
lib/libxquic-static.a
lib/libxquic.so
bin/netins_udp_data_client
bin/netins_udp_data_server
share/doc/netins_udp/
```

## 6. 第三方程序链接

动态库：

```sh
cc app.c \
  -I/opt/netins-udp/include \
  -L/opt/netins-udp/lib \
  -lxquic -lssl -lcrypto -lpthread -ldl -lm \
  -o app
```

静态库：

```sh
cc app.c \
  -I/opt/netins-udp/include \
  /opt/netins-udp/lib/libxquic-static.a \
  /path/to/libssl.a \
  /path/to/libcrypto.a \
  -lpthread -ldl -lm \
  -o app
```

头文件、XQUIC库、TLS库和目标程序必须属于同一架构。

## 7. 常见错误

### 编译器路径变成 `/aarch64-oe-linux-gcc`

说明工具链变量为空或目录填写错误。重新检查：

```sh
printf '%s\n' "$NETINS_TOOLCHAIN_DIR"
test -x "$NETINS_TOOLCHAIN_DIR/aarch64-oe-linux-gcc"
```

### `ssl include path not found`

检查：

```sh
test -f "$NETINS_SSL_ROOT/include/openssl/ssl.h"
```

并确认CMake命令使用的是当前终端中已设置的 `NETINS_SSL_ROOT`。

### 大量TLS未定义符号

通常是TLS库链接顺序、架构或静态库依赖不匹配。确认 `libssl.a` 与
`libcrypto.a` 来自同一套Tongsuo构建，并使用本交付版CMake配置。

### 更新源码后重新编译

依赖路径和CMake配置没有改变时，可以直接执行：

```sh
cmake --build BUILD_DIRECTORY --target TARGET -j$(nproc)
```

更换架构、工具链或TLS路径时必须删除旧构建目录，重新运行CMake配置。
