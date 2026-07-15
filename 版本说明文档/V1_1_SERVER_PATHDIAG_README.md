# xquic-1.8.3-v1.1-server-pathdiag

This version is based on `xquic-1.8.3-v1-rxfix`.

The goal of v1.1 is to diagnose why GET aggregation still mainly uses path0 after
the client RX notification/libevent issue was fixed in v1.

## Main Changes

- Server transport callbacks now register path lifecycle notifications:
  - `ready_to_create_path_notify`
  - `path_created_notify`
  - `path_removed_notify`
- Server logs forced multipath/path snapshots at key moments:
  - H3 connection creation
  - H3 handshake finished
  - path ready/created/removed
  - GET request headers parsed
  - GET response start
  - first GET body send entry
- Server scheduler parsing now recognizes `balanced` and `rap`.
- Server startup prints the requested and resolved multipath scheduler:
  - `[diag_server_sched] requested=... resolved=...`

## Important Log Tags

- `[diag_server_path_event]`
  Path lifecycle event. If path1 is never created, the problem is before the
  scheduler can use the second path.

- `[diag_server_get]`
  GET-specific server timing point. Use this to see whether the second path is
  already active when the response starts.

- `[mini_mp]` and `[diag_server_path]`
  Per-path XQUIC stats snapshot. Check `active_path_cnt`, `both_active`,
  `path_id`, `state`, `send_bytes`, and `recv_bytes`.

- `[diag_server_sched]`
  Confirms whether the configured scheduler was actually selected.

## Expected Diagnosis

For a healthy two-path GET test, the server log should show:

- at least two `path_id` entries in `[diag_server_path]`;
- `active_path_cnt >= 2` near `get_response_start` or `get_send_body_enter`;
- server `send_bytes` increasing on both paths;
- client `recv_bytes` increasing on both paths.

If the server still shows only `path_id=0` during GET response start, the next
fix should focus on path creation/validation or GET stream multipath eligibility,
not on the client RX queue.
