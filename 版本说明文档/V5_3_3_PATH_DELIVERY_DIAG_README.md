# v5.3.3 path delivery diagnostics

This version is copied from `xquic-1.8.3-v5.3.2-clean-metrics`. It keeps the
adaptive scheduling decisions unchanged and adds connection-close diagnostics.

## Added measurements

- Per-path scheduler-selected bytes and pick counts.
- Per-path transport-delivered bytes and delivery Mbps from ACK accounting.
- Per-path application/wire send bytes.
- Actual encoded reinjection and retransmission bytes.
- Primary, probe, warmup, single-path, and cwnd-fallback pick counts.
- No-sendable-path calls, final/min/max probe percentage, adjustment counts,
  path RTT, queue ratio, cwnd, inflight, estimated bandwidth, and loss count.

The dual-path diagnostic window starts when adaptive warmup starts. Single-path
runs start at the first scheduler decision. Results are emitted only once, at
connection close, using these prefixes:

```text
[diag_adaptive_summary]
[diag_adaptive_totals]
[diag_adaptive_path]
```

These lines are produced only when the resolved scheduler is `adaptive`. Because
the scheduler callback structure changed, rebuild the library and mini programs
together instead of replacing only the final `mini_server` binary.

`selected_bytes` measures scheduler decisions and may exceed or differ from
actual transmission. `app_sent_bytes` and `wire_sent_bytes` measure transmitted
data. `delivered_bytes` is transport data acknowledged on the path and is the
main value for determining whether a second path adds delivery or replaces
delivery from the primary path.

## Source changes

- `include/xquic/xquic.h`
- `src/transport/xqc_conn.c`
- `src/transport/xqc_send_ctl.h`
- `src/transport/xqc_send_ctl.c`
- `src/transport/scheduler/xqc_scheduler_adaptive.c`

Client receive, server GET file I/O, congestion-control parameters, and adaptive
path-selection return values are unchanged from v5.3.2.
