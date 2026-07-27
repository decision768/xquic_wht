# NetIns UDP API

NetIns UDP provides QUIC and Multipath QUIC client/server connectivity through
a C API. The application data plane uses HTTP/3 request and response objects.

```c
#include <xquic/netins_udp.h>
```

## Features

- blocking and nonblocking client connections;
- server listener and accepted connection objects;
- HTTP/3 GET, POST, request body and response body transfer;
- connection state, multipath state and per-path statistics;
- `poll`/`epoll` readiness notifications;
- connection errors and close reasons;
- optional application logging callbacks.

## Installed files

- `include/xquic/netins_udp.h`
- `lib/libxquic.so`
- `lib/libxquic.a`
- `${CMAKE_INSTALL_DOCDIR}/netins_udp/NETINS_UDP_API_使用说明.md`
- `${CMAKE_INSTALL_DOCDIR}/netins_udp/netins_udp_*_example.c`

The complete API reference, lifecycle rules and examples are documented in
[NETINS_UDP_API_使用说明.md](NETINS_UDP_API_使用说明.md).

## Linux x86_64 build

```sh
cmake -S . -B build-linux-x86_64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-x86_64.cmake
cmake --build build-linux-x86_64 -j
cmake --install build-linux-x86_64 --prefix /opt/netins-udp
```

## Linux AArch64 build

```sh
cmake -S . -B build-aarch64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/sim8810-aarch64.cmake \
  -DXQC_SIM8810_SDK_ROOT=/path/to/SIM8810_SDK \
  -DXQC_SIM8810_TOOLCHAIN_DIR=/path/to/aarch64-oe-linux/compiler-directory \
  -DSSL_PATH=/path/to/aarch64/Tongsuo \
  -DLIBEVENT_ROOT=/path/to/aarch64/libevent
cmake --build build-aarch64 -j
```

Use the toolchain file that matches the target platform. The AArch64 SDK,
compiler, TLS library and Libevent library must all target the same ABI.

## Application linking

```sh
cc app.c -I/opt/netins-udp/include -L/opt/netins-udp/lib \
  -lxquic -lssl -lcrypto -lpthread -ldl -lm
```

At startup, verify that the header and runtime library match:

```c
if (netins_udp_get_api_version() != NETINS_UDP_API_VERSION) {
    return -1;
}
```

## Logging and statistics

The library does not emit application logs unless
`netins_udp_set_logger()` is called. INFO reports low-frequency lifecycle and
error information. DEBUG is intentionally compact.

Use `netins_udp_get_snapshot()` and the statistics APIs for path traffic, RTT,
CWND, inflight and throughput data. Log messages are not a stable statistics
interface.
