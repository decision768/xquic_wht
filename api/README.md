# NetIns UDP API

NetIns UDP provides QUIC and Multipath QUIC client/server connectivity through
a C API. Applications establish a connection and transfer continuous data
through `netins_udp_send()` and `netins_udp_recv()`.

```c
#include <xquic/netins_udp.h>
```

## Features

- blocking and nonblocking client connections;
- server listener and accepted connection objects;
- reliable ordered connection-level send/recv;
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

The installed `netins_udp_data_client.c` and
`netins_udp_data_server.c` demonstrate the basic ordered `send/recv` channel.
They do not include mini or private XQUIC headers. `netins_udp_api_stress.sh`
runs repeatable bidirectional transfers through those baseline demos and
validates the received files.
Applications transfer files through the same ordered `send/recv` byte stream;
no separate file-transfer API or private XQUIC header is required.

The complete API reference, lifecycle rules and examples are documented in
[NETINS_UDP_API_使用说明.md](NETINS_UDP_API_使用说明.md).

## Linux x86_64 build

```sh
cmake -S . -B build-linux-x86_64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-x86_64.cmake
cmake --build build-linux-x86_64 -j
cmake --install build-linux-x86_64 --prefix /opt/netins-udp
```

Add `-DNETINS_UDP_BUILD_DATA_DEMO=ON` to build the standalone
`netins_udp_data_client` and `netins_udp_data_server` executables. These demos
send and receive either ordinary data or file contents through the same API.

For source, ABI and public data-loopback validation:

```sh
cmake -S . -B build-test \
  -DNETINS_UDP_BUILD_TESTS=ON \
  -DNETINS_UDP_BUILD_DATA_DEMO=ON
cmake --build build-test -j
ctest --test-dir build-test --output-on-failure
./build-test/netins_udp_data_loopback_test \
  server.crt server.key 18444
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

To force the delivered static library, replace `-L... -lxquic` with the
absolute `/opt/netins-udp/lib/libxquic.a` path.

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
