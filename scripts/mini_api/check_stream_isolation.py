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
        "server_backend": root / "mini/mini_server_backend.c",
        "stream_backend": root / "mini/mini_stream_backend.c",
        "api": root / "src/netins_udp/netins_udp_api.c",
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
            r"connection->stream_api_state\s*!=\s*NULL.*?"
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
