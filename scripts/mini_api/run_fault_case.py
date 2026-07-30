#!/usr/bin/env python3
"""Run one stage-9 fault injection case on a controlled target machine."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Optional

from target_common import atomic_write_json, parse_log, sha256_file


FAULT_TYPES = (
    "disconnect-5g1",
    "disconnect-5g2",
    "restore-path",
    "server-exit",
    "client-exit",
    "timeout",
)


def expand(command: Optional[str], client_pid: int) -> Optional[str]:
    if command is None:
        return None
    return command.replace("{client_pid}", str(client_pid))


def run_control_command(
    label: str,
    command: Optional[str],
    client_pid: int,
    log_stream: Any,
) -> Optional[int]:
    expanded = expand(command, client_pid)
    if expanded is None:
        return None
    log_stream.write(f"\n[harness] {label}: {expanded}\n")
    log_stream.flush()
    try:
        completed = subprocess.run(
            expanded,
            shell=True,
            stdout=log_stream,
            stderr=subprocess.STDOUT,
            timeout=60,
            check=False,
        )
        log_stream.write(
            f"[harness] {label} return_code={completed.returncode}\n"
        )
        log_stream.flush()
        return completed.returncode
    except subprocess.TimeoutExpired:
        log_stream.write(f"[harness] {label} timed out\n")
        log_stream.flush()
        return None


def terminate_process(process: subprocess.Popen[Any]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=10)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=10)


def wait_until_or_exit(
    process: subprocess.Popen[Any], deadline: float
) -> bool:
    while time.monotonic() < deadline:
        if process.poll() is not None:
            return False
        time.sleep(min(0.1, max(0.0, deadline - time.monotonic())))
    return process.poll() is None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case-id", required=True)
    parser.add_argument("--fault-type", choices=FAULT_TYPES, required=True)
    parser.add_argument("--client-kind", choices=("mini", "api"), default="api")
    parser.add_argument(
        "--expected-outcome", choices=("success", "error"), required=True
    )
    parser.add_argument("--fault-after-seconds", type=float, required=True)
    parser.add_argument("--recover-after-seconds", type=float, default=0)
    parser.add_argument("--fault-command")
    parser.add_argument("--recovery-command")
    parser.add_argument("--reconnect-command")
    parser.add_argument("--expected-error-pattern")
    parser.add_argument("--expected-file", type=Path)
    parser.add_argument("--actual-file", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=1800)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = list(args.command)
    if command and command[0] == "--":
        command = command[1:]
    if not command:
        parser.error("a client command is required after --")
    if args.expected_outcome == "success" and (
        args.expected_file is None or args.actual_file is None
    ):
        parser.error("successful cases require --expected-file and --actual-file")
    if (
        args.expected_outcome == "error"
        and args.fault_type != "client-exit"
        and not args.expected_error_pattern
    ):
        parser.error(
            "error cases except client-exit require --expected-error-pattern"
        )

    args.output_dir.mkdir(parents=True, exist_ok=True)
    log_path = args.output_dir / f"{args.case_id}.log"
    result_path = args.output_dir / f"{args.case_id}.json"
    started = time.monotonic()
    fault_return_code = None
    recovery_return_code = None
    reconnect_return_code = None
    timed_out = False

    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        process = subprocess.Popen(
            command, stdout=log, stderr=subprocess.STDOUT
        )
        fault_deadline = started + args.fault_after_seconds
        process_alive_at_fault = wait_until_or_exit(process, fault_deadline)
        try:
            if process_alive_at_fault:
                fault_return_code = run_control_command(
                    "fault", args.fault_command, process.pid, log
                )
            if args.recovery_command is not None:
                recovery_deadline = (
                    fault_deadline + args.recover_after_seconds
                )
                wait_until_or_exit(process, recovery_deadline)
        finally:
            if args.recovery_command is not None:
                recovery_return_code = run_control_command(
                    "recovery", args.recovery_command, process.pid, log
                )

        remaining = args.timeout_seconds - (time.monotonic() - started)
        try:
            process.wait(timeout=max(0.001, remaining))
        except subprocess.TimeoutExpired:
            timed_out = True
            terminate_process(process)

        if args.reconnect_command:
            reconnect_return_code = run_control_command(
                "reconnect", args.reconnect_command, process.pid, log
            )
        return_code = process.returncode

    log_text = log_path.read_text(encoding="utf-8", errors="replace")
    parsed = parse_log(log_text, args.client_kind)
    expected_sha256 = (
        sha256_file(args.expected_file)
        if args.expected_file is not None and args.expected_file.is_file()
        else None
    )
    actual_sha256 = (
        sha256_file(args.actual_file)
        if args.actual_file is not None and args.actual_file.is_file()
        else None
    )
    sha256_match = (
        expected_sha256 is not None and expected_sha256 == actual_sha256
    )
    error_pattern_seen: Optional[bool] = (
        args.expected_error_pattern in log_text
        if args.expected_error_pattern
        else args.fault_type == "client-exit"
    )
    recovery_ok = (
        args.recovery_command is None or recovery_return_code == 0
    )
    reconnect_ok = (
        args.reconnect_command is None or reconnect_return_code == 0
    )
    fault_ok = (
        args.fault_command is None or fault_return_code == 0
    )

    if args.expected_outcome == "success":
        outcome_ok = return_code == 0 and not timed_out and sha256_match
    else:
        outcome_ok = (
            (return_code != 0 or timed_out) and error_pattern_seen is True
        )

    result = {
        "schema_version": 1,
        "stage": 9,
        "status": "PASS"
        if (
            process_alive_at_fault
            and fault_ok
            and recovery_ok
            and reconnect_ok
            and outcome_ok
        )
        else "FAIL",
        "case_id": args.case_id,
        "fault_type": args.fault_type,
        "expected_outcome": args.expected_outcome,
        "command": command,
        "fault_command": args.fault_command,
        "recovery_command": args.recovery_command,
        "reconnect_command": args.reconnect_command,
        "process_alive_at_fault": process_alive_at_fault,
        "fault_return_code": fault_return_code,
        "recovery_return_code": recovery_return_code,
        "reconnect_return_code": reconnect_return_code,
        "return_code": return_code,
        "timed_out": timed_out,
        "wall_seconds": time.monotonic() - started,
        "expected_error_pattern": args.expected_error_pattern,
        "error_pattern_seen": error_pattern_seen,
        "expected_sha256": expected_sha256,
        "actual_sha256": actual_sha256,
        "sha256_match": sha256_match,
        "log_path": str(log_path),
        **parsed,
    }
    atomic_write_json(result_path, result)
    print(
        f"{result['status']} {args.case_id} "
        f"fault={fault_return_code} recovery={recovery_return_code} "
        f"return_code={return_code} sha256_match={sha256_match}"
    )
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
