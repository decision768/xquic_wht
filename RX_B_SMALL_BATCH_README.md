# RX Experiment B: Small Queue Batch

This build keeps the existing client receive architecture unchanged:

```text
per-path RX threads
-> shared linked-list queue
-> notification pipe
-> XQUIC engine thread
```

The only receive behavior change from experiment A is:

```text
CLIENT_RX_BATCH_MAX_PACKETS: 512 -> 32
CLIENT_RX_BATCH_MAX_US:      5000 (unchanged)
```

This isolates the effect of calling `xqc_engine_finish_recv()` after a smaller
packet batch. It does not remove RX threads, the shared queue, packet copies,
locks, the notification pipe, or `rx_notify_pending`.

Expected client startup marker:

```text
[resolved-rx] mode=queue_small_batch_b batch_packets=32 budget_us=5000
```

Keep the server scheduler, congestion-control settings, ACK policy, stream
layout, socket buffers, and file workload identical to experiment A.
