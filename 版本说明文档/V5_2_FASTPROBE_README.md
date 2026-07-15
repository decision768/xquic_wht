# V5.2 Fastprobe Scheduler

## Goal

V5.2 targets the GET aggregation case where the second path is active but does
not improve goodput. The observed baseline was:

- path0 = 5G1 / rmnet_data0, slower path.
- path1 = 5G2 / rmnet_data1, faster path.
- `bw` scheduler used both paths, but often sent about 37% to 43% on path0.
- Dual-path server-side phase stayed around 51 to 55 Mbps in the sampled logs.
- Client summary logs showed Agg GET average below the best single 5G2 run.

The new scheduler avoids forcing heavy traffic onto a weak path. It keeps the
fast path as primary and lets the slow path send only a bounded probe share
unless the paths are close in estimated capacity.

## New Scheduler Names

Set `mp_sched` to one of:

```text
fastonly
fastprobe5
fastprobe10
fastprobe20
fastprobe
```

`fastprobe` is equivalent to `fastprobe10`.

The packaged server config defaults to:

```text
mp_sched=fastprobe10
```

## Scheduling Logic

For each send decision, the scheduler:

1. Collects active, non-frozen, cwnd-sendable paths.
2. Estimates each path capacity with `xqc_send_ctl_get_est_bw()`.
3. Falls back to `cwnd / srtt` when bandwidth samples are not available.
4. Smooths capacity per path with a small EMA.
5. Picks the highest-capacity path as the primary path.
6. Computes a target share for weaker paths:

```text
ratio >= 80% of best path: use natural capacity share
50% <= ratio < 80%: cap weak path at 2x configured probe share
35% <= ratio < 50%: cap weak path at configured probe share
ratio < 35%: cap weak path at half configured probe share
```

The primary path receives the remaining share. A deficit/credit scheduler then
keeps the long-term packet selection near those target shares.

## Files Changed

```text
src/transport/scheduler/xqc_scheduler_fastprobe.c
src/transport/scheduler/xqc_scheduler_fastprobe.h
include/xquic/xquic.h
CMakeLists.txt
mini/mini_server.c
mini/mini_client.c
mini/server_config.txt
```

## Recommended Test Order

For GET-focused testing, change only server `mp_sched` between runs:

```text
fastonly
fastprobe5
fastprobe10
fastprobe20
bw
```

Keep the same client, file size, network switching flow, and logging. Compare:

```text
client GET goodput
diag_server_get_phase dual_mbps
diag_server_get_path path0/path1 send_share
cwnd_blocked
loss_rate
path srtt
```

Expected interpretation:

- `fastonly` > `bw`: slow path traffic was harmful in this environment.
- `fastprobe5/10` > `fastonly`: slow path helps, but only at low share.
- `fastprobe20` < `fastprobe10`: the slow-path share is already too high.
- `fastprobe20` > `fastprobe10`: the slow path can carry more traffic than the
  current diagnostics suggested.
