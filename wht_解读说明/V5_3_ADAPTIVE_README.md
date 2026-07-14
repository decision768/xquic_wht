# V5.3 Adaptive Multipath Scheduler

## Purpose

V5.3 removes the fixed assumption that a particular interface or path ID is
always faster. It keeps the V1-V3 receive and file-I/O fixes, preserves the
V5.2 schedulers for comparison, and adds a new server scheduler:

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
- Adjusts the secondary target in 5 percentage point steps.
- Uses one-window hill-climbing trials: keep a higher probe share only when
  total delivery improves by at least 2% without harming the primary.
- Reverts a failed trial and waits two evaluation windows before probing up
  again.
- Restricts the target to 5%-45% and never exceeds the score-derived natural
  share.
- Uses actual sent bytes in a 1 MiB / 500 ms window rather than packet count.
- If the primary is temporarily cwnd-blocked and the secondary has already
  consumed its allowance, waits for the primary instead of allowing unlimited
  slow-path overflow.

## Server Diagnostics

The scheduler writes low-rate stdout diagnostics without requiring zlog:

```text
[diag_adaptive] event=warmup_start ...
[diag_adaptive] event=primary_select ...
[diag_adaptive] event=adjust ...
```

`primary_rate`, `total_rate`, `score`, and `secondary_score` are bytes per
second. `probe_old` and `probe_new` are percentages.

## Changed Files

```text
src/transport/scheduler/xqc_scheduler_adaptive.c
src/transport/scheduler/xqc_scheduler_adaptive.h
include/xquic/xquic.h
CMakeLists.txt
CMakeLists-v5.3-3090.txt
mini/mini_client.h
mini/mini_client.c
mini/mini_client_cb.c
mini/mini_server.c
mini/server_config.txt
```

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
`CMakeLists-v5.3-3090.txt` before configuring a clean build directory.

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
both path byte counters increase
Agg median goodput >= best single-path median goodput
file checksum matches
```

The first performance target is to avoid regression against the best single
path. The later target remains at least 80% of the sum of the two independent
single-path goodputs.
