# V0 Diagnostic Build

This copy keeps the original client receive/write behavior and only adds lightweight diagnostics.

## Output Prefixes

- `[diag_rx]`: client RX queue, notification, drain, and socket event counters.
- `[diag_quic]`: client QUIC connection counters from `xqc_conn_get_stats()`.
- `[diag_path]`: client per-path counters from `xqc_conn_get_stats().paths_info[]`.
- `[diag_io]`: client download `pwrite()` and final `fsync()` counters.
- `[diag_server_quic]`: server QUIC send/receive/loss/RTT counters.
- `[diag_server_path]`: server per-path send/receive counters.

## Important Fields

- `notify_write`: pipe notifications attempted by RX thread.
- `notify_read`: pipe notifications consumed by event loop.
- `notify_skip`: packets enqueued while `rx_notify_pending` was already `1`.
- `q_cur` / `q_max`: current and maximum client RX queue depth.
- `drain_max_us`: maximum time spent in one RX queue drain call.
- `pkt_wait_max_us`: maximum packet wait time from enqueue to processing.
- `finish_recv`: number of `xqc_engine_finish_recv()` calls after queue drain.
- `socket_ev_read`: Libevent socket readable callbacks on client UDP sockets.
- `loss_rate`: `lost_count / send_count` from the local QUIC endpoint.
- `pwrite_max_us`: maximum observed synchronous file write latency.
- `fsync_us`: final file sync latency.

For GET/download loss, prefer the server-side `[diag_server_quic] loss_rate`, because the server is the large data sender.
