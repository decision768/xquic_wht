# v5.3.2 clean metrics

This version is copied from `xquic-1.8.3-v5.3.1-adaptive-fix` and changes only
diagnostic behavior and GET timing.

## Changes

- Server multipath logging no longer calls `xqc_h3_conn_send_ping()`.
- Periodic server transfer-state and adaptive adjustment output is disabled in
  transfer hot paths. Lifecycle, path-event, error, and completion summaries remain.
- Client download progress is printed at 10 percent milestones.
- GET keeps `download_total_bytes` unchanged and measures headline Mbps from the
  first received HTTP/3 body chunk to the last received HTTP/3 body chunk.
- Async writer draining and `fsync()` remain enabled and diagnosed, but their time
  is excluded from the GET headline Mbps.

The existing `all download streams finished` line is preserved so current stress
scripts continue to parse the summary logs without changes.
