# NetIns UDP Mini API 编译指南

## 1. 依赖

- CMake 3.15 或更高；
- C11 编译器；
- Tongsuo/BabaSSL 8.3 或兼容 BoringSSL；
- Libevent 2.1，含 pthread 支持；
- zlog 1.2；
- Linux pthread、dl 和 math 库；
- 打包/ABI 检查需要 Python 3、`nm`、`tar` 和 `sha256sum`。

依赖库必须与目标架构一致并使用 `-fPIC`，否则共享库链接会失败。

## 2. Linux x86_64

根 CMake 默认保留项目的 SIM8810 配置，因此 x86_64 构建应显式指定主机编译器：

```bash
cmake -S . -B build-x86_64 \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_BUILD_TYPE=Release \
  -DXQC_ENABLE_MINI_API=ON \
  -DXQC_ENABLE_TESTING=OFF \
  -DSSL_INC_PATH=/opt/tongsuo/include \
  -DSSL_LIB_PATH="/opt/tongsuo/lib/libssl.a;/opt/tongsuo/lib/libcrypto.a" \
  -DLIBEVENT_INCLUDE_DIR=/opt/libevent/include \
  -DLIBEVENT_LIBRARY=/opt/libevent/lib/libevent.a \
  -DLIBEVENT_PTHREADS_LIBRARY=/opt/libevent/lib/libevent_pthreads.a \
  -DZLOG_INCLUDE_DIR=/opt/zlog/include \
  -DZLOG_STATIC_LIB=/opt/zlog/lib/libzlog.a

cmake --build build-x86_64 --parallel
```

## 3. SIM8810 AArch64

```bash
cmake -S . -B build-sim8810 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/sim8810-aarch64.cmake \
  -DSIM8810_SDK_ROOT=/opt/SIM8810_SDK_OCPU_LE3.0 \
  -DSIM8810_TOOLCHAIN_DIR=/opt/crosstools/toolchain/gcc/usr/bin/aarch64-oe-linux \
  -DCMAKE_BUILD_TYPE=Release \
  -DXQC_ENABLE_MINI_API=ON \
  -DXQC_ENABLE_TESTING=OFF \
  -DSSL_INC_PATH=/opt/aarch64/tongsuo/include \
  -DSSL_LIB_PATH="/opt/aarch64/tongsuo/lib/libssl.a;/opt/aarch64/tongsuo/lib/libcrypto.a" \
  -DLIBEVENT_INCLUDE_DIR=/opt/aarch64/libevent/include \
  -DLIBEVENT_LIBRARY=/opt/aarch64/libevent/lib/libevent.a \
  -DLIBEVENT_PTHREADS_LIBRARY=/opt/aarch64/libevent/lib/libevent_pthreads.a \
  -DZLOG_INCLUDE_DIR=/opt/aarch64/zlog/include \
  -DZLOG_STATIC_LIB=/opt/aarch64/zlog/lib/libzlog.a

cmake --build build-sim8810 --parallel
```

## 4. 目标与安装

正式目标：

```text
xquic-static
xquic
netins_udp_mini_api_client
netins_udp_mini_api_server
```

安装到临时目录：

```bash
cmake --install build-x86_64 \
  --prefix "$PWD/stage-x86_64" \
  --component mini_api
```

安装规则只放入公开头文件、库、两个 API demo 和正式文档；阶段测试脚本与内部诊断材料不会
进入交付目录。

## 5. ABI 检查

```bash
python3 scripts/mini_api/check_abi.py \
  --library build-x86_64/libxquic.so
```

检查器比较：

- `netins_udp.h` 的全部 `NETINS_UDP_PUBLIC` 函数；
- `scripts/xquic.lds` 的导出清单；
- 动态库实际定义的符号。

任一公开函数漏导出或导出表出现无声明函数都会返回非 0。

## 6. 正式打包

完成目标架构全量构建后：

```bash
sh scripts/mini_api/package_delivery.sh \
  build-x86_64 "$PWD/delivery/x86_64"
```

脚本执行 component install、交付文件完整性检查、动态库 ABI 检查，最后生成：

```text
netins-mini-api-5.4.2-mini-api.1.tar.gz
netins-mini-api-5.4.2-mini-api.1.tar.gz.sha256
```

SIM8810 包在对应目标环境或具备 AArch64 `nm` 的交叉环境中用同样方式生成。

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

具体依赖顺序以目标 SDK 链接器为准。
