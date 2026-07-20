#!/usr/bin/env python3

import argparse
import re
import sys
from pathlib import Path


def verify_role(role, log_path, require_multipath):
    text = Path(log_path).read_text(encoding="utf-8", errors="replace")
    prefix = rf"\[netins_udp\]\[{role}\]"

    registered = set(re.findall(
        prefix + r" registered connection_id=(\d+) registry_query=ok", text))
    connected = set(re.findall(
        prefix + r" connected connection_id=(\d+)", text))
    closed = set(re.findall(
        prefix + r" closed connection_id=(\d+)", text))
    multipath = set(re.findall(
        prefix + r" multipath connection_id=(\d+) paths=([2-9]\d*)", text))
    multipath_ids = {connection_id for connection_id, _ in multipath}
    closed_paths = re.findall(
        prefix + r" closed connection_id=(\d+).*? paths=(\d+)", text)
    multipath_ids.update(
        connection_id for connection_id, path_count in closed_paths
        if int(path_count) >= 2)

    failures = []
    if not registered:
        failures.append("no successful registry registration")
    if registered and not registered.intersection(connected):
        failures.append("no registered connection reached CONNECTED")
    if registered and not registered.intersection(closed):
        failures.append("no registered connection reached CLOSED/ERROR")
    if require_multipath and registered \
            and not registered.intersection(multipath_ids):
        failures.append("no registered connection exposed two paths")

    if failures:
        print(f"{role}: FAIL: " + "; ".join(failures))
        return False

    print(
        f"{role}: PASS registered={len(registered)} "
        f"connected={len(connected)} multipath={len(multipath_ids)} "
        f"closed={len(closed)}"
    )
    return True


def main():
    parser = argparse.ArgumentParser(
        description="Verify netins_udp lifecycle markers from a real run")
    parser.add_argument("client_log")
    parser.add_argument("server_log")
    parser.add_argument(
        "--allow-single-path", action="store_true",
        help="do not require the one-time paths>=2 marker")
    args = parser.parse_args()

    require_multipath = not args.allow_single_path
    client_ok = verify_role("client", args.client_log, require_multipath)
    server_ok = verify_role("server", args.server_log, require_multipath)
    return 0 if client_ok and server_ok else 1


if __name__ == "__main__":
    sys.exit(main())
