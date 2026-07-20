# netins_udp connection status API

`netins_udp` exposes read-only UDP/QUIC connection status for the real XQUIC
connections owned by `mini_client` and `mini_server`.

Chinese integration guide: [`NETINS_UDP_API_使用说明.md`](NETINS_UDP_API_使用说明.md).

## Scope

- All public names use the `netins_udp_` prefix.
- The API reports connection lifecycle, multipath state, path state, addresses,
  interface names when known, packet counts, byte counters, RTT, loss, inflight
  bytes, and interval rates.
- The registry is process-local. A third-party module must run in the same
  process and use the same linked `netins_udp` implementation instance as
  `mini_client` or `mini_server`. Code compiled directly into the host
  executable meets this rule. Do not load a second private copy of `libxquic`
  in a plugin. Cross-process access requires a separate IPC service and is not
  part of this API.
- Third-party code only reads mutex-protected snapshots. It never calls the
  XQUIC engine and cannot close or mutate the transport connection.

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
python3 api/tests/verify_real_connection.py client.log server.log
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
