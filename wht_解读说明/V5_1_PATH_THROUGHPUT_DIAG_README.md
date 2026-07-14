# v5.1 Path Throughput Diagnostics

This version is based on `xquic-1.8.3-v4-server-bw`.

It does not change the GET scheduling algorithm.  It only adds server-side
diagnostics so a GET run can be split into single-path and dual-path phases.

## Main Changes

Modified files:

- `mini/mini_server_cb.h`
- `mini/mini_server_cb.c`

Inherited from v4:

- server default scheduler: `bw`
- `mini/server_config.txt`: `mp_sched=bw`
- `mp_ack_on_any_path=0` on mini server and mini client

## New Logs

### Per GET Stream Summary

```text
[diag_server_get_path][response_done]
```

Important fields:

- `stream`
- `offset`
- `len`
- `start_both`
- `end_both`
- `start_active`
- `end_active`
- `duration_ms`
- `server_mbps`
- `conn_send_delta`
- `conn_lost_delta`
- `loss_delta_rate`
- `path_send_delta`
- `srtt_start`
- `srtt_end`

Use this line to compare single-path and dual-path stream throughput.

### Per Path Delta

```text
[diag_server_get_path][path_delta]
```

Important fields:

- `stream`
- `idx`
- `id`
- `send_delta`
- `send_share`
- `recv_delta`
- `send_pkts_delta`
- `recv_pkts_delta`
- `start_srtt`
- `end_srtt`
- `end_send_bytes`
- `end_recv_bytes`

Use this line to see how much each GET stream actually used each path.

### Running Phase Summary

```text
[diag_server_get_phase][summary]
```

Important fields:

- `single_streams`
- `single_bytes`
- `single_time_ms`
- `single_mbps`
- `dual_streams`
- `dual_bytes`
- `dual_time_ms`
- `dual_mbps`

Use this line to quickly compare:

```text
single-path phase throughput vs dual-path phase throughput
```

## Notes

`server_mbps`, `single_mbps`, and `dual_mbps` are server-side response send
measurements.  They are useful for phase comparison, but client-side goodput is
still the final throughput metric.

