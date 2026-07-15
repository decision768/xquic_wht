# V5.3.1 Adaptive Scheduler Fix

## Purpose

V5.3.1 keeps the V5.3 path discovery and adaptive scheduler, then fixes the
probe collapse observed in the 100 MiB GET log. That run initialized a false
primary baseline near 927 Mbps, reduced the secondary target from 20% to 5%,
and used path0 for only about 13.6% of transmitted bytes.

```text
mp_sched=adaptive
```

## Client GET Path Gate

For a multi-interface GET, the client waits until the configured transport
paths are `ACTIVE` and `AVAILABLE` before launching the request. The wait is
bounded to five 200 ms rounds. If another path is unavailable, the GET falls
back to the ready path instead of hanging.

Expected client diagnostics:

```text
[diag_path_gate] wait after handshake ...
[diag_path_gate] waiting ...
[diag_path_gate] launch ... fallback=0
[diag_path_map] slot=0 path_id=0 interface=...
```

Single-interface configurations launch immediately.

Before creating `path0`, the client checks configured interfaces in order and
promotes the first one that currently has an IPv4 address. This is an
availability fallback only: no interface name or expected speed is hard-coded,
and the scheduler still decides which path is faster from warmup measurements.

## Adaptive Scheduler State Machine

```text
WAIT_PATHS -> WARMUP -> BULK
```

### WARMUP

- Sends at least 256 KiB on each available path.
- Waits for at least 64 KiB of ACK-confirmed delivery per path when possible.
- Uses a minimum 100 ms and maximum 800 ms warmup duration.
- Estimates path capacity from ACK delivery rate and XQUIC estimated bandwidth.
- Penalizes queue growth (`srtt / min_rtt`) and recent loss.

### Primary selection

- Selects the highest-scoring path after warmup.
- Keeps the primary for at least one second.
- Switches only when a challenger is at least 20% better for three consecutive
  500 ms evaluation windows.
- Fails over immediately when the primary path disappears.

### Adaptive probing

- Starts the secondary share at 5%, 10%, 20%, or 40% according to the warmup
  score ratio.
- Evaluates ACK-confirmed total rate and primary rate every 500 ms.
- Initializes the primary baseline from the first complete 500 ms delivery
  window. Warmup ACK bursts are never used as the baseline.
- Smooths subsequent baseline samples with a bounded 80/20 EWMA.
- Adjusts the secondary target in 5 percentage point steps.
- Uses one-window hill-climbing trials and keeps a higher share when total
  delivery stays within 2% of the reference window.
- Reverts a failed trial and waits two evaluation windows before probing up
  again.
- Allows a 0%-45% target. A path is reduced toward zero only when its own queue
  is persistently worse, rather than because the primary path is queued.
- When the primary queue is worse than the secondary queue, the next upward
  trial shifts traffic toward the secondary path.
- Uses actual sent bytes in a 1 MiB / 500 ms window rather than packet count.
- Is work-conserving: if the primary is cwnd-blocked but the secondary can
  send, it uses the secondary instead of deliberately returning no path.

## Server Diagnostics

The scheduler writes low-rate stdout diagnostics without requiring zlog:

```text
[diag_adaptive] event=warmup_start ...
[diag_adaptive] event=primary_select ...
[diag_adaptive] event=adjust ... reason=baseline_calibrate|explore_up|trial_keep|trial_revert
[diag_adaptive_path] path=... rate=... delivered_share=... sent_share=... target_share=...
```

`primary_rate`, `total_rate`, `score`, and `secondary_score` are bytes per
second. `probe_old`, `probe_new`, and all share fields are percentages.

## Files Changed From V5.3

```text
src/transport/scheduler/xqc_scheduler_adaptive.c
CMakeLists-v5.3.1-3090.txt
mini/server_config.txt
V5_3_1_ADAPTIVE_FIX_README.md
```

The rest of the source tree is copied unchanged from V5.3, including the
client path-readiness gate and scheduler registration.

## Build

Native project build:

```bash
rm -rf build
mkdir build
cd build
cmake .. -DXQC_ENABLE_TESTING=ON
make -j
```

For the 3090 server layout, replace the project root `CMakeLists.txt` with
`CMakeLists-v5.3.1-3090.txt` before configuring a clean build directory.

## First Test Matrix

Use the same network switching flow and run at least five GETs per item:

```text
5G1 only
5G2 only
Agg + fastprobe10
Agg + adaptive
```

Acceptance checks for `adaptive`:

```text
client launch fallback=0 during Agg
server warmup_start appears
one primary_select after warmup
no repeated primary switching on a stable network
secondary probe percentage remains bounded
first adjust reason is baseline_calibrate and baseline is close to primary_rate
probe does not collapse directly from 15% or 20% to 5%
diag_adaptive_path sent_share follows target_share over stable windows
both path byte counters increase
Agg median goodput >= best single-path median goodput
file checksum matches
```

The first performance target is to avoid regression against the best single
path. The later target remains at least 80% of the sum of the two independent
single-path goodputs.
