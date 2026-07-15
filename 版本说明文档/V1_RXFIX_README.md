# XQUIC v1 RX Fix Notes

This tree is based on `xquic-1.8.3-v0-diag`.

v1 keeps the v0 diagnostic counters and applies the first RX-path fix:

1. Keep the existing pipe-based RX queue notification.
2. Clear `rx_notify_pending` when the event-loop thread consumes the pipe notification.
3. Stop registering per-path UDP sockets as persistent Libevent `EV_READ` events.
4. Keep Libevent for RX queue notifications, engine timers, connection timeout, and path retry timers.

Expected diagnostic changes versus v0:

```text
notify_write        grows over time instead of staying at 1
notify_read         grows with notify_write
notify_skip         drops sharply
socket_ev_read      stays at 0 or near 0
q_max               should be lower under GET load
finish_recv         should happen more regularly
```

v1 intentionally does not yet add bounded drain processing, asynchronous file writing,
or server-side scheduler changes. Those belong to later versions so each change can be
measured independently.
