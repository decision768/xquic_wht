# RX 40/256 Dynamic Budget Experiment

This directory is based on the direct fair RX version of
`xquic-1.8.3-v5.3.3-path-delivery-diag` at commit `c3f78344`.

Only the client receive budget is changed:

- one receive-ready path: read at most 40 packets from that path per round;
- two receive-ready paths: read at most 256 packets from each path per round;
- alternate the first path serviced in each round;
- call `xqc_engine_finish_recv()` once after a non-empty round;
- no packet-processing time budget and no extra RX rounds.

The receive buffer has 256 slots per path, which is the maximum per-path
budget. A dual-path round can therefore process at most 512 packets before
`xqc_engine_finish_recv()`.

The client prints `[diag_rx_budget]` only when it changes between single-path
and dual-path modes.

This version has no `netins_udp` dependency. Build this source tree as a whole,
or copy both `mini/mini_client.c` and `mini/mini_client.h` into the exact same
base revision before rebuilding `mini_client`.
