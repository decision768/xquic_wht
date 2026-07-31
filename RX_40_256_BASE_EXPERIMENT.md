# RX 40/256 Dynamic Budget Experiment

This directory is based on the direct fair RX version of
`xquic-1.8.3-v5.3.3-path-delivery-diag` at commit `c3f78344`.

The current Mini API integration selects the client receive budget from
XQUIC's transport-layer ACTIVE path count:

- fewer than two transport ACTIVE paths: read at most 40 packets from each
  local receive-ready path per round;
- two or more transport ACTIVE paths: read at most 256 packets from each
  local receive-ready path per round;
- alternate the first path serviced in each round;
- call `xqc_engine_finish_recv()` once after a non-empty round;
- no packet-processing time budget and no extra RX rounds.

The receive buffer has 256 slots per path, which is the maximum per-path
budget. A dual-path round can therefore process at most 512 packets before
`xqc_engine_finish_recv()`.

The client prints `[diag_rx_budget]` when the transport ACTIVE count, local
I/O-ready count, or selected budget changes.  In that log, `active_paths`
is the transport ACTIVE count; `budget_paths`, `io_ready_paths`, and
`source=transport_active` make the selected mode and local socket state
directly comparable.

This version has no `netins_udp` dependency. Build this source tree as a whole,
or copy both `mini/mini_client.c` and `mini/mini_client.h` into the exact same
base revision before rebuilding `mini_client`.
