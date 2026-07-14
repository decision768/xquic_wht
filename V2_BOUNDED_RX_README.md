# xquic-1.8.3-v2-bounded-rx

This version is based on `xquic-1.8.3-v1.1-server-pathdiag`.

The goal of v2 is to bound the client RX queue drain work.  v1 fixed the
notification/libevent ownership issue, and v1.1 proved that the second path can
be created and used.  The remaining client-side problem is that one drain round
can still process a huge backlog before calling `xqc_engine_finish_recv()`.

## Main Change

`mini/mini_client.c` now limits each RX drain round with both budgets:

```c
#define CLIENT_RX_BATCH_MAX_PACKETS 512
#define CLIENT_RX_BATCH_MAX_US 5000
```

Each drain round:

1. pops and processes at most 512 queued packets;
2. stops after roughly 5000 us once at least one packet was processed;
3. calls `xqc_engine_finish_recv()` after the batch;
4. reactivates the RX queue event if backlog remains.

This keeps ACK generation, timers, HTTP/3 callbacks, and application scheduling
from being delayed by a single huge queue drain.

## Expected Log Changes

Compared with v1/v1.1:

- `drain_max_pkts` should drop from thousands/tens of thousands to a bounded value;
- `drain_max_us` should drop sharply;
- `finish_recv` should increase;
- `drain_round` should increase;
- `q_max` and `pkt_wait_max_us` should stop growing without bound.

The existing GET diagnostic script can still be used:

```bash
python D:\5g\stress_get_diag_gx.py
```

Deploy this v2 source on the client side.  The server-side v1.1 path diagnostics
are still included in this directory.
