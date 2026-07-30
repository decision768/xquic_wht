#!/usr/bin/env python3
"""Run one compatibility/performance case and emit a machine-readable result."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from target_common import (
    atomic_write_json,
    expected_rx_limit,
    parse_log,
    run_logged_command,
    rx_budget_matches,
    sha256_file,
)


COMBOS = ("mini-mini", "api-api", "api-mini", "mini-api")
NETWORKS = ("5G1", "5G2", "Agg")
DIRECTIONS = ("GET", "POST")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case-id", required=True)
    parser.add_argument("--combo", choices=COMBOS, required=True)
    parser.add_argument("--client-kind", choices=("mini", "api"), required=True)
    parser.add_argument("--direction", choices=DIRECTIONS, required=True)
    parser.add_argument("--network", choices=NETWORKS, required=True)
    parser.add_argument("--size-bytes", type=int, required=True)
    parser.add_argument("--round", type=int, choices=range(1, 6), required=True)
    parser.add_argument("--expected-paths", type=int, choices=(1, 2), required=True)
    parser.add_argument("--expected-file", type=Path, required=True)
    parser.add_argument("--actual-file", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=1800)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a client command is required after --")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    log_path = args.output_dir / f"{args.case_id}.log"
    result_path = args.output_dir / f"{args.case_id}.json"
    execution = run_logged_command(command, log_path, args.timeout_seconds)
    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    parsed = parse_log(log_text, args.client_kind)

    expected_sha256 = (
        sha256_file(args.expected_file)
        if args.expected_file.is_file()
        else None
    )
    actual_sha256 = (
        sha256_file(args.actual_file) if args.actual_file.is_file() else None
    )
    path_count_ok = parsed["observed_path_count"] == args.expected_paths
    budget_ok = rx_budget_matches(
        parsed["rx_budgets"], args.expected_paths
    )
    result = {
        "schema_version": 1,
        "stage": 8,
        "status": "PASS",
        "case_id": args.case_id,
        "combo": args.combo,
        "client_kind": args.client_kind,
        "direction": args.direction,
        "network": args.network,
        "size_bytes": args.size_bytes,
        "round": args.round,
        "expected_paths": args.expected_paths,
        "expected_rx_limit": expected_rx_limit(args.expected_paths),
        "command": command,
        "log_path": str(log_path),
        **execution,
        **parsed,
        "path_count_ok": path_count_ok,
        "rx_budget_ok": budget_ok,
        "expected_sha256": expected_sha256,
        "actual_sha256": actual_sha256,
        "sha256_match": (
            expected_sha256 is not None
            and expected_sha256 == actual_sha256
        ),
    }
    required_checks = (
        execution["return_code"] == 0,
        not execution["timed_out"],
        parsed["throughput_mbps"] is not None,
        path_count_ok,
        budget_ok,
        result["sha256_match"],
    )
    if not all(required_checks):
        result["status"] = "FAIL"

    atomic_write_json(result_path, result)
    print(
        f"{result['status']} {args.case_id} "
        f"throughput={parsed['throughput_mbps']} "
        f"sha256_match={result['sha256_match']} "
        f"paths={parsed['observed_path_count']} "
        f"rx_budget_ok={budget_ok}"
    )
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
