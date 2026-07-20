# netins_udp connection and status API

`netins_udp` exposes read-only UDP/QUIC connection status for the real XQUIC
connections owned by `mini_client` and `mini_server`. It also provides an
independent client connection-lifecycle API; application data transfer is not
part of this connection-only revision yet.

Chinese integration guide: [`NETINS_UDP_API_使用说明.md`](NETINS_UDP_API_使用说明.md).

## Independent client connection

The library also provides a first connection-lifecycle API that creates a real
QUIC/H3 client without starting `mini_client`:

1. Initialize `netins_udp_client_config_t` with
   `netins_udp_client_config_init()`.
2. Set `server_address`, `server_port`, the optional TLS `server_name`, and up
   to `NETINS_UDP_MAX_PATHS` interface names.
3. Call `netins_udp_client_create()` and `netins_udp_client_connect()`.
4. Query the connection through `netins_udp_client_get_snapshot()` or the
   existing process-local registry.
5. Release the connection and its private engine thread with
   `netins_udp_client_close()`.

`netins_udp_client_connect()` blocks until the H3 handshake completes or the
configured timeout expires. The client owns its UDP sockets, XQUIC engine, poll
thread, multipath creation attempts, and status adapter. It does not alter or
reuse the engines owned by `mini_client` or `mini_server`.

A successful connect means that the QUIC/H3 handshake is complete. Additional
multipath paths can still be validating, so applications should poll the
snapshot until the expected `active_path_count` appears. Binding sockets to
named interfaces uses Linux `SO_BINDTODEVICE` and can require elevated network
capabilities. Certificate verification uses the TLS library's default trust
chain; custom CA loading is not part of this revision.

This version intentionally covers connection lifecycle only. Application data
send/receive and HTTP/3 request APIs will be added separately after the
connection path is validated on Linux/AArch64. See
`examples/netins_udp_connect_example.c`.

Build and run the standalone connection check with:

```sh
cmake -DNETINS_UDP_BUILD_CONNECT_EXAMPLE=ON ..
make -j netins_udp_connect_cli
./netins_udp_connect_cli 10.12.54.143 8443 test.xquic.com \
    rmnet_data0 usb0
```

## Scope

- All public names use the `netins_udp_` prefix.
- The API reports connection lifecycle, multipath state, path state, addresses,
  interface names when known, packet counts, byte counters, RTT, loss, inflight
  bytes, and interval rates.
- The registry is process-local. An independent client can query the connection
  it created in its own process. A monitor of `mini_client` or `mini_server`
  must still run in that mini process and use the same linked `netins_udp`
  implementation instance. Do not load a second private copy of `libxquic` in
  a plugin. Cross-process access requires a separate IPC service and is not
  part of this API.
- Snapshot getters only copy mutex-protected state and never call the XQUIC
  engine. A caller can close a client it created with `netins_udp_client_*`,
  but cannot close or mutate connections owned by the mini applications.

## Third-party lifecycle

1. Call `netins_udp_list_connections(NULL, 0, &count)` to query the number of
   live registered connections.
2. Allocate an ID array and call `netins_udp_list_connections()` again.
3. Use `netins_udp_get_snapshot_by_id()` for one-shot polling, or call
   `netins_udp_open()` to retain a connection handle.
4. Initialize every output structure with its matching
   `NETINS_UDP_*_INITIALIZER` macro before a getter call.
5. Release a retained handle with `netins_udp_close()`.

When XQUIC closes a connection, the connection disappears from the registry.
A handle retained before closure remains valid and returns the final CLOSED or
ERROR snapshot until the third party releases it. `netins_udp_close()` only
releases the API handle; it never closes XQUIC.

See `examples/netins_udp_snapshot_example.c` for polling code.

## Host integration

The mini applications create and bind one adapter in their H3 connection-create
callback. A persistent Libevent timer refreshes the cached snapshot every 500
ms from the same thread that drives XQUIC. Handshake and close callbacks update
the lifecycle state. Client path metadata includes the configured interface
name, while both sides report path local and peer address tuples.

Adapter allocation, binding, or timer failure disables the status adapter for
that connection but does not fail the QUIC connection or data transfer.

## Build and delivery

The implementation is part of `libxquic.so` and `libxquic-static.a`. Public
consumers include `<xquic/netins_udp.h>` and link the same XQUIC library that is
loaded by the host process. `cmake --install` installs the public header and
both library targets.

Optional unit test:

```sh
cmake -DNETINS_UDP_BUILD_TESTS=ON ..
make -j
ctest --output-on-failure -R netins_udp_unit_test
```

Runtime integration checks:

```sh
./mini_client | grep netins_udp
./mini_server | grep netins_udp
```

Each real H3 connection prints one registration line containing a non-invalid
`connection_id` and `registry_query=ok`. The one-time `multipath` marker proves
that the live snapshot exposed at least two paths. The third-party poller should
then see that ID, CONNECTED after the handshake, both path records during a
dual-path run, and no registry entry after connection close.

Validate logs from one real client/server run with:

```sh
python3 tests/netins_udp/verify_real_connection.py client.log server.log
```

Use `--allow-single-path` only for an intentional single-interface test.

## Counter meaning

- `bytes_wire_sent`: all encoded QUIC bytes sent on a path.
- `bytes_sent`: encoded bytes of packets carrying application data.
- `bytes_delivered`: transport bytes acknowledged on the sending side.
- `bytes_recv`: encoded bytes of received packets carrying stream/datagram data.
- `bytes_recv_effective`: non-duplicate stream payload accepted on the path.
- `*_reinject` and `bytes_sent_retransmitted`: reinjection/retransmission
  subsets; they must not be added again to totals.

Rates are deltas between host refreshes and are expressed in Mbit/s. A first
snapshot has a zero interval rate by design.
