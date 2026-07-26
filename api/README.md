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
cmake -S . -B build \
  -DSSL_TYPE=openssl \
  -DSSL_PATH=/path/to/openssl \
  -DNETINS_UDP_BUILD_TESTS=ON
cmake --build build -j
cmake --install build --prefix /opt/netins-udp
```

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
