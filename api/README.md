# NetIns UDP API

NetIns UDP is the installed QUIC/Multipath QUIC API supplied by this
repository. Third-party programs use one header and one object model:

```c
#include <xquic/netins_udp.h>
```

The public surface provides:

- blocking or nonblocking client connect;
- listener and independently owned accepted connections;
- HTTP/3 GET/POST request objects and response objects;
- connection, multipath, path, RTT, CWND, inflight and traffic snapshots;
- independent readiness descriptors for listener, connection and request
  handles;
- explicit error and close-reason queries.

Only `include/xquic/netins_udp.h` is installed. Compatibility adapters used by
the mini programs and the implementation are private and are not part of the
third-party ABI.

Documentation:

- [Chinese API guide](NETINS_UDP_API_使用说明.md)
- [Architecture and ABI contract](NETINS_UDP_API_设计说明.md)
- [Examples](examples)

Typical build:

```sh
cmake -S . -B build-linux-x86_64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-x86_64.cmake \
  -DNETINS_UDP_BUILD_TESTS=ON
cmake --build build-linux-x86_64 -j
cmake --install build-linux-x86_64 --prefix /opt/netins-udp
```

The root build supports both native Linux x86_64 and the SIM8810 AArch64
cross toolchain. It auto-enables the historical SIM8810 toolchain only when
its configured SDK and compiler paths exist. A different SIM8810 installation
can be selected without editing CMake:

```sh
cmake -S . -B build-aarch64 \
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/sim8810-aarch64.cmake \
  -DXQC_SIM8810_SDK_ROOT=/path/to/SIM8810_SDK \
  -DXQC_SIM8810_TOOLCHAIN_DIR=/path/to/aarch64-oe-linux/compiler-directory \
  -DSSL_PATH=/path/to/aarch64/Tongsuo \
  -DLIBEVENT_ROOT=/path/to/aarch64/libevent
cmake --build build-aarch64 -j
```

The two toolchain files provide an explicit one-file-to-one-architecture
mapping. The existing `XQC_USE_SIM8810_TOOLCHAIN` switch remains available for
backward-compatible builds that do not pass `CMAKE_TOOLCHAIN_FILE`. When mini
programs are enabled, `ZLOG_INCLUDE_DIR` and `ZLOG_STATIC_LIB` can be supplied
to override the architecture-specific defaults.

A consumer links `libxquic` plus the platform libraries required by the XQUIC
build:

```sh
cc app.c -I/opt/netins-udp/include -L/opt/netins-udp/lib \
  -lxquic -lssl -lcrypto -lpthread -ldl -lm
```

The public data plane is HTTP/3. Connection-level raw byte-stream send/receive
is deliberately not published because QUIC multiplexes independent streams
and cannot provide TCP-like connection byte-stream semantics without an
additional framing contract.
