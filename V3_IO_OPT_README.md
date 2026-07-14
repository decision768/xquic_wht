# xquic-1.8.3-v3-io-opt

This version is based on `xquic-1.8.3-v2-bounded-rx`.

v3 focuses on I/O pressure around GET:

1. Client GET download writes no longer call `pwrite()` directly in the H3 read callback.
2. Server GET file responses use fd-based `pread()` instead of `FILE *` + `fread()`.
3. Both changes add diagnostics so the effect can be measured.

## Client GET Async Write

Changed files:

- `mini/mini_client.h`
- `mini/mini_client.c`
- `mini/mini_client_cb.c`

Old flow:

```text
H3 read callback
-> xqc_h3_request_recv_body()
-> pwrite()
-> return to XQUIC
```

New flow:

```text
H3 read callback
-> xqc_h3_request_recv_body()
-> copy body chunk into bounded write queue
-> return to XQUIC

download writer thread
-> pwrite() queued chunks by offset
```

The queue is bounded by:

```c
#define XQC_MINI_CLI_WRITE_QUEUE_MAX_TASKS 1024
#define XQC_MINI_CLI_WRITE_QUEUE_MAX_BYTES (64 * 1024 * 1024)
```

Download completion now waits for the async write queue to become empty before
calling `fsync()` and closing the file.

New client `[diag_io]` fields:

```text
async_enq              chunks enqueued to the async writer
async_enq_bytes        bytes enqueued to the async writer
async_q_depth          current queued write tasks
async_q_bytes          current queued bytes
async_q_max            maximum queued write tasks
async_q_bytes_max      maximum queued bytes
async_wait_max_us      max wait time when the queue was full
```

Existing fields still apply:

```text
pwrite_cnt
pwrite_bytes
pwrite_max_us
fsync_us
```

In v3, `pwrite_*` is measured in the writer thread, not in the H3 callback.

## Server GET File Read Optimization

Changed files:

- `mini/mini_server_cb.h`
- `mini/mini_server_cb.c`

Old flow:

```text
prepare response
-> fopen()
-> fseek()

send body callback
-> fread()
-> xqc_h3_request_send_body()
```

New flow:

```text
prepare response
-> stat()
-> open()

send body callback
-> pread(fd, offset)
-> xqc_h3_request_send_body()
```

This removes `FILE *` buffered I/O from the GET send path and avoids relying on
a mutable file position. It is still synchronous file reading; it is not a
server-side read thread.

New server log:

```text
[diag_server_io][response_done]
```

Important fields:

```text
file_read_cnt
file_read_bytes
file_read_max_us
h3_send_cnt
h3_send_bytes
h3_send_max_us
```

Use these to distinguish:

```text
file_read_max_us high  -> server storage read may be a GET bottleneck
h3_send_max_us high    -> XQUIC/H3 send path or flow control may be limiting
```

## What v3 Does Not Change

v3 does not change:

```text
congestion control
multipath scheduler
GET request segmentation
POST upload logic
server POST pwrite/fsync path
v2 RX bounded-drain logic
v1/v1.1 path diagnostics
```

## Expected Result

If client disk write is a bottleneck, v3 should reduce:

```text
client H3 callback blocking
RX queue wait spikes
GET goodput jitter
```

If server file reads are a bottleneck, v3 should show lower or more visible
server-side read costs via `[diag_server_io]`.

If aggregation is still poor while I/O costs are low, the likely next focus is:

```text
path active timing
GET scheduler behavior
weak-path drag
test file size
POST/GET goodput measurement consistency
```
