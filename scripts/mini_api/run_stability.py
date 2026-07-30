#!/usr/bin/env python3
"""Repeat a Mini API command to validate lifecycle and file integrity."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from target_common import (
    atomic_write_json,
    run_logged_command,
    sha256_file,
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case-id", required=True)
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--expected-file", type=Path, required=True)
    parser.add_argument(
        "--actual-file-template",
        required=True,
        help="path with optional {iteration} placeholder",
    )
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=1800)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command_template = list(args.command)
    if command_template and command_template[0] == "--":
        command_template = command_template[1:]
    if not command_template:
        parser.error("a command is required after --")
    if args.iterations <= 0:
        parser.error("--iterations must be positive")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    expected_sha256 = sha256_file(args.expected_file)
    iterations = []
    for iteration in range(1, args.iterations + 1):
        actual_path = Path(
            args.actual_file_template.format(iteration=iteration)
        )
        substitutions = {
            "iteration": str(iteration),
            "actual_file": str(actual_path),
        }
        command = [
            argument.format(**substitutions) for argument in command_template
        ]
        execution = run_logged_command(
            command,
            args.output_dir / f"{args.case_id}-r{iteration}.log",
            args.timeout_seconds,
        )
        actual_sha256 = (
            sha256_file(actual_path) if actual_path.is_file() else None
        )
        passed = (
            execution["return_code"] == 0
            and not execution["timed_out"]
            and actual_sha256 == expected_sha256
        )
        iterations.append(
            {
                "iteration": iteration,
                "status": "PASS" if passed else "FAIL",
                "command": command,
                "actual_file": str(actual_path),
                "actual_sha256": actual_sha256,
                **execution,
            }
        )
        if not passed:
            break

    result = {
        "schema_version": 1,
        "stage": 9,
        "status": "PASS"
        if len(iterations) == args.iterations
        and all(item["status"] == "PASS" for item in iterations)
        else "FAIL",
        "case_id": args.case_id,
        "requested_iterations": args.iterations,
        "completed_iterations": len(iterations),
        "expected_file": str(args.expected_file),
        "expected_sha256": expected_sha256,
        "iterations": iterations,
    }
    atomic_write_json(args.output_dir / f"{args.case_id}.json", result)
    print(
        f"{result['status']} {args.case_id} "
        f"iterations={len(iterations)}/{args.iterations}"
    )
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())

