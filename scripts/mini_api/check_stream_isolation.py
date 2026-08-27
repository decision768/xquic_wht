#!/usr/bin/env python3
"""Guard the file API and stream API connection-mode isolation contract."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root", type=Path,
        default=Path(__file__).resolve().parents[2],
    )
    args = parser.parse_args()
    root = args.root.resolve()
    sources = {
        "client": root / "mini/mini_client.c",
        "server": root / "mini/mini_server.c",
        "client_backend": root / "mini/mini_client_backend.c",
        "client_cb": root / "mini/mini_client_cb.c",
        "server_backend": root / "mini/mini_server_backend.c",
        "stream_backend": root / "mini/mini_stream_backend.c",
        "api": root / "src/netins_udp/netins_udp_api.c",
        "public_header": root / "include/xquic/netins_udp.h",
    }
    try:
        text = {
            name: path.read_text(encoding="utf-8")
            for name, path in sources.items()
        }
    except OSError as error:
        print(f"STREAM ISOLATION CHECK: FAIL: {error}", file=sys.stderr)
        return 1

    checks = (
        (
            "normal client connections retain xqc_h3_connect",
            "client",
            r"if\s*\(\s*args->stream_poc\.enabled\s*\|\|\s*"
            r"args->stream_api_enabled\s*\)\s*\{.*?"
            r"xqc_h3_ext_connect\s*\(.*?\}\s*else\s*\{.*?"
            r"xqc_h3_connect\s*\(",
        ),
        (
            "client enables H3 extension only for stream mode",
            "client",
            r"egn_cfg\.enable_h3_ext\s*=\s*"
            r"args->stream_poc\.enabled\s*\|\|\s*"
            r"args->stream_api_enabled\s*;",
        ),
        (
            "server enables H3 extension only for stream mode",
            "server",
            r"egn_cfg\.enable_h3_ext\s*=\s*"
            r"args->stream_poc\.enabled\s*\|\|\s*"
            r"args->stream_api_enabled\s*;",
        ),
        (
            "file transfer rejects an active stream",
            "api",
            r"netins_udp_transfer\s*\(.*?client->active_stream\s*!=\s*NULL",
        ),
        (
            "file serving rejects an active stream",
            "api",
            r"netins_udp_serve\s*\(.*?connection->active_stream\s*!=\s*NULL",
        ),
        (
            "stream creation requires explicit stream mode",
            "api",
            r"netins_udp_stream_create\s*\(.*?"
            r"!connection->backend_config\.stream_mode",
        ),
        (
            "stream open rejects an active file transfer",
            "client_backend",
            r"mini_backend_client_stream_open\s*\(.*?"
            r"client->connection->transfer_active",
        ),
        (
            "server stream ownership is per accepted connection",
            "server_backend",
            r"mini_backend_server_stream_accept\s*\(.*?"
            r"connection->stream_api_state\s*==\s*NULL.*?"
            r"external_event_driver\s*=\s*1",
        ),
        (
            "successful stream close does not send RESET_STREAM",
            "stream_backend",
            r"mini_stream_close_lanes_locked\s*\(.*?int\s+abort.*?"
            r"if\s*\(\s*abort\s*\).*?xqc_h3_ext_bytestream_close",
        ),
        (
            "application delivery callback runs outside the stream lock",
            "stream_backend",
            r"mini_stream_delivery_worker\s*\(.*?"
            r"pthread_mutex_unlock\(&stream->lock\).*?callback\(",
        ),
        (
            "public stream configuration has one fixed four-lane width",
            "public_header",
            r"NETINS_UDP_STREAM_LANE_COUNT\s+4U.*?"
            r"typedef\s+struct\s+netins_udp_stream_config_s\s*\{\s*"
            r"uint32_t\s+struct_size;\s*uint32_t\s+api_version;\s*"
            r".*?uint32_t\s+reserved32;\s*uint32_t\s+flags;",
        ),
        (
            "server buffers Bytestreams that arrive before stream accept",
            "server_backend",
            r"mini_backend_server_stream_bs_create\s*\(.*?"
            r"stream_api_pending_head.*?xqc_h3_ext_bytestream_set_user_data",
        ),
        (
            "pending Bytestream cleanup does not touch a stream after close",
            "server_backend",
            r"xqc_h3_ext_bytestream_close\(pending->bs\).*?"
            r"mini_backend_server_pending_free\(pending->connection,\s*"
            r"pending,\s*0\s*\)",
        ),
        (
            "client state polling yields to the active stream worker",
            "client_backend",
            r"mini_backend_client_poll\s*\(.*?"
            r"client->active_stream\s*!=\s*NULL\s*\)\s*\{\s*"
            r"return\s+0\s*;.*?pthread_mutex_lock\(&client->engine_lock\)",
        ),
        (
            "client stream worker shares engine serialization with snapshots",
            "client_backend",
            r"mini_backend_client_stream_open\s*\(.*?"
            r"&client->engine_lock.*?mini_backend_client_get_snapshot\s*\("
            r".*?mini_backend_client_wake_stream\(client\).*?"
            r"pthread_mutex_lock\(&client->engine_lock\).*?"
            r"xqc_conn_get_stats",
        ),
        (
            "connection queries remain live while the stream worker drives I/O",
            "api",
            r"netins_udp_get_state\s*\(.*?"
            r"handle->active_stream\s*==\s*NULL\s*\)\s*\{\s*"
            r"poll_error\s*=\s*mini_backend_client_poll.*?"
            r"mini_backend_client_is_closed",
        ),
        (
            "client normalizes H3 no-error before notifying stream state",
            "client_cb",
            r"connection_error\s*=\s*h3_error\s*==\s*H3_NO_ERROR\s*"
            r"\?\s*0\s*:\s*h3_error\s*;.*?"
            r"mini_stream_backend_on_connection_close\(.*?connection_error",
        ),
        (
            "internal stream worker serializes event-base entry",
            "stream_backend",
            r"mini_stream_worker\s*\(.*?mini_stream_engine_acquire\(stream\)"
            r".*?event_base_loop\(stream->event_base,\s*EVLOOP_ONCE\).*?"
            r"mini_stream_engine_release\(stream\)",
        ),
        (
            "client stream worker installs a background logger scope",
            "stream_backend",
            r"mini_stream_worker\s*\(.*?"
            r"mini_stream_worker_log_scope_enter\(stream\).*?"
            r"event_base_loop\(stream->event_base,\s*EVLOOP_ONCE\).*?"
            r"mini_backend_log_scope_leave\(\)",
        ),
        (
            "server stream worker installs a background logger scope",
            "server_backend",
            r"mini_backend_server_io_worker\s*\(.*?"
            r"mini_backend_server_worker_log_scope_enter\(server\).*?"
            r"event_base_loop\(server->ctx\.eb,\s*EVLOOP_ONCE\).*?"
            r"mini_backend_log_scope_leave\(\)",
        ),
        (
            "server accept timeout reuses one bounded elapsed value",
            "server_backend",
            r"mini_backend_server_accept\s*\(.*?"
            r"xqc_usec_t\s+elapsed\s*=.*?elapsed\s*>=\s*timeout_us.*?"
            r"remaining_us\s*=\s*timeout_us\s*-\s*elapsed",
        ),
        (
            "server worker exit stops every external stream driver",
            "server_backend",
            r"io_worker_running\s*=\s*0;.*?"
            r"mini_stream_backend_external_driver_stopped",
        ),
        (
            "server connection statistics are serialized with engine drive",
            "server_backend",
            r"pthread_mutex_lock\(&server->engine_lock\);.*?"
            r"xqc_conn_get_stats.*?"
            r"pthread_mutex_unlock\(&server->engine_lock\);",
        ),
        (
            "server connection close publishes errors under the backend lock",
            "server_backend",
            r"mini_backend_server_on_connection_closed\s*\(.*?"
            r"pthread_mutex_lock\(&server->lock\);\s*"
            r"connection->last_error_code\s*=\s*error_code;",
        ),
    )

    failures = []
    for label, source_name, pattern in checks:
        if re.search(pattern, text[source_name], re.DOTALL) is None:
            failures.append(label)
    if failures:
        for label in failures:
            print(f"missing invariant: {label}", file=sys.stderr)
        print("STREAM ISOLATION CHECK: FAIL", file=sys.stderr)
        return 1
    print(f"STREAM ISOLATION CHECK: PASS ({len(checks)} invariants)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
