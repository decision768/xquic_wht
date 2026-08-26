#!/usr/bin/env python3
"""Run local end-to-end acceptance checks for the NetIns data-stream API."""

from __future__ import annotations

import argparse
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence


RESULT_RE = re.compile(r"\[stream_api_result\]\s+(?P<fields>.*)")
FIELD_RE = re.compile(r"(?P<key>[a-z_]+)=(?P<value>[^\s]+)")


@dataclass(frozen=True)
class TransferCase:
    name: str
    size: int
    lanes: int
    seed: int
    send_queue: int = 8 * 1024 * 1024
    flags: int = 0
    require_backpressure: bool = False


def executable(bin_dir: Path, name: str) -> Path:
    suffixes = (".exe", "") if os.name == "nt" else ("", ".exe")
    directories = (bin_dir, bin_dir / "mini", bin_dir / "demo")
    for directory in directories:
        for suffix in suffixes:
            candidate = directory / f"{name}{suffix}"
            if candidate.is_file():
                return candidate.resolve()
    raise FileNotFoundError(f"cannot find {name} under {bin_dir}")


def fields(output: str, role: str) -> dict[str, str]:
    for line in output.splitlines():
        match = RESULT_RE.search(line)
        if not match:
            continue
        values = {
            item.group("key"): item.group("value")
            for item in FIELD_RE.finditer(match.group("fields"))
        }
        if values.get("role") == role:
            return values
    return {}


def integer(values: dict[str, str], key: str) -> int:
    value = values.get(key, "")
    if not value.isdigit():
        raise ValueError(f"missing or invalid {key}={value!r}")
    return int(value)


def read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def decoded(value: str | bytes | None) -> str:
    if value is None:
        return ""
    if isinstance(value, bytes):
        return value.decode("utf-8", errors="replace")
    return value


def stop_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=3)


def run_pair(
    server_binary: Path,
    client_binary: Path,
    listen_ip: str,
    host: str,
    port: int,
    size: int,
    server_lanes: int,
    client_lanes: int,
    seed: int,
    send_queue: int,
    flags: int,
    timeout_s: float,
) -> tuple[int, str, int, str]:
    api_timeout_ms = max(3000, int(timeout_s * 1000))
    receive_queue = 8 * 1024 * 1024
    with tempfile.TemporaryDirectory(prefix="netins-stream-test-") as temp_dir:
        server_log_path = Path(temp_dir) / "server.log"
        server_log = server_log_path.open("w", encoding="utf-8")
        server_command = [
            str(server_binary), listen_ip, str(port), str(size),
            str(server_lanes), str(seed), str(receive_queue),
            str(api_timeout_ms),
        ]
        server = subprocess.Popen(
            server_command,
            stdout=server_log,
            stderr=subprocess.STDOUT,
            text=True,
        )
        server_log.close()
        try:
            ready_deadline = time.monotonic() + min(10.0, timeout_s)
            while time.monotonic() < ready_deadline:
                if "[stream_api_ready]" in read_text(server_log_path):
                    break
                if server.poll() is not None:
                    break
                time.sleep(0.05)
            if "[stream_api_ready]" not in read_text(server_log_path):
                stop_process(server)
                return (
                    server.returncode or 1,
                    read_text(server_log_path),
                    1,
                    "client not started: server did not become ready",
                )

            client_command = [
                str(client_binary), host, str(port), str(size),
                str(client_lanes), str(seed), str(send_queue),
                str(receive_queue), str(flags), str(api_timeout_ms),
                str(api_timeout_ms),
            ]
            try:
                client_result = subprocess.run(
                    client_command,
                    capture_output=True,
                    text=True,
                    timeout=timeout_s + 5,
                    check=False,
                )
                client_output = client_result.stdout + client_result.stderr
                client_rc = client_result.returncode
            except subprocess.TimeoutExpired as error:
                client_output = decoded(error.stdout) + decoded(error.stderr)
                client_output += "\nclient controller timeout"
                client_rc = 124
            try:
                server_rc = server.wait(timeout=timeout_s + 5)
            except subprocess.TimeoutExpired:
                stop_process(server)
                server_rc = 124
            return server_rc, read_text(server_log_path), client_rc, client_output
        finally:
            stop_process(server)


def validate_success(
    case: TransferCase,
    server_rc: int,
    server_output: str,
    client_rc: int,
    client_output: str,
) -> None:
    if server_rc != 0 or client_rc != 0:
        raise RuntimeError(f"server rc={server_rc}, client rc={client_rc}")
    server_values = fields(server_output, "server")
    client_values = fields(client_output, "client")
    if integer(server_values, "success") != 1:
        raise ValueError("server did not report success=1")
    if integer(client_values, "success") != 1:
        raise ValueError("client did not report success=1")
    if integer(server_values, "received") != case.size:
        raise ValueError("server byte count differs from requested size")
    if integer(client_values, "accepted") != case.size:
        raise ValueError("client accepted byte count differs from requested size")
    if integer(client_values, "peer_received") != case.size:
        raise ValueError("peer receipt byte count differs from requested size")
    if case.require_backpressure:
        if integer(client_values, "eagain") == 0:
            raise ValueError("nonblocking case did not observe EAGAIN")
        if integer(client_values, "partial") == 0:
            raise ValueError("nonblocking case did not observe a partial write")


def print_failure(server_output: str, client_output: str) -> None:
    print("--- server output ---", file=sys.stderr)
    print(server_output.rstrip(), file=sys.stderr)
    print("--- client output ---", file=sys.stderr)
    print(client_output.rstrip(), file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Run protocol, ordering, backpressure and loopback checks"
    )
    parser.add_argument("--bin-dir", type=Path, default=Path("build/mini"))
    parser.add_argument("--listen-ip", default="0.0.0.0")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--base-port", type=int, default=18460)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--full", action="store_true",
                        help="also run 64 MiB cases on 1, 4 and 8 lanes")
    parser.add_argument("--skip-unit", action="store_true")
    parser.add_argument("--skip-fault", action="store_true")
    parser.add_argument(
        "--file-regression-command",
        action="append",
        default=[],
        metavar="COMMAND",
        help="quoted file-API regression command; may be repeated",
    )
    args = parser.parse_args()
    if not 1 <= args.base_port <= 65520 or args.timeout <= 0:
        parser.error("invalid base port or timeout")

    try:
        server_binary = executable(args.bin_dir, "netins_udp_stream_server")
        client_binary = executable(args.bin_dir, "netins_udp_stream_client")
    except FileNotFoundError as error:
        print(f"ACCEPTANCE: FAIL: {error}", file=sys.stderr)
        return 1

    failures = 0
    if not args.skip_unit:
        for name in (
            "mini_stream_protocol_test",
            "mini_stream_reorder_test",
            "netins_udp_stream_api_contract_test",
        ):
            try:
                binary = executable(args.bin_dir, name)
                completed = subprocess.run(
                    [str(binary)], timeout=30, check=False,
                    capture_output=True, text=True,
                )
                if completed.returncode != 0:
                    failures += 1
                    print(f"[FAIL] {name}", file=sys.stderr)
                    print(completed.stdout + completed.stderr, file=sys.stderr)
                else:
                    print(f"[PASS] {name}")
            except (FileNotFoundError, subprocess.TimeoutExpired) as error:
                failures += 1
                print(f"[FAIL] {name}: {error}", file=sys.stderr)

    cases = [
        TransferCase("zero", 0, 1, 1),
        TransferCase("one-byte", 1, 1, 2),
        TransferCase("below-record", 65535, 1, 3),
        TransferCase("one-record", 65536, 4, 4),
        TransferCase("over-record", 65537, 4, 5),
        TransferCase("multi-lane", 8 * 1024 * 1024, 8, 6),
        TransferCase(
            "nonblocking-backpressure", 8 * 1024 * 1024, 4, 7,
            send_queue=65536, flags=1, require_backpressure=True,
        ),
    ]
    if args.full:
        for lanes in (1, 4, 8):
            cases.append(TransferCase(
                f"64MiB-{lanes}-lane", 64 * 1024 * 1024,
                lanes, 20 + lanes,
            ))

    for index, case in enumerate(cases):
        port = args.base_port + index
        server_rc, server_output, client_rc, client_output = run_pair(
            server_binary, client_binary, args.listen_ip, args.host, port,
            case.size, case.lanes, case.lanes, case.seed, case.send_queue,
            case.flags, args.timeout,
        )
        try:
            validate_success(
                case, server_rc, server_output, client_rc, client_output
            )
            print(f"[PASS] {case.name}: bytes={case.size} lanes={case.lanes}")
        except (RuntimeError, ValueError) as error:
            failures += 1
            print(f"[FAIL] {case.name}: {error}", file=sys.stderr)
            print_failure(server_output, client_output)

    if not args.skip_fault:
        port = args.base_port + len(cases)
        server_rc, server_output, client_rc, client_output = run_pair(
            server_binary, client_binary, args.listen_ip, args.host, port,
            65536, 4, 1, 99, 8 * 1024 * 1024, 0,
            min(args.timeout, 10.0),
        )
        combined = server_output + client_output
        unexpected_success = (
            server_rc == 0 or client_rc == 0
            or "success=1" in combined
            or "last_error=" not in combined
        )
        if unexpected_success:
            failures += 1
            print("[FAIL] lane-mismatch fault was not rejected", file=sys.stderr)
            print_failure(server_output, client_output)
        else:
            print("[PASS] lane-mismatch fault rejected with observable error")

    for command_text in args.file_regression_command:
        command: Sequence[str] = shlex.split(
            command_text, posix=os.name != "nt"
        )
        if not command:
            failures += 1
            print("[FAIL] empty file API regression command", file=sys.stderr)
            continue
        try:
            completed = subprocess.run(
                command, timeout=args.timeout, check=False
            )
            if completed.returncode != 0:
                failures += 1
                print(
                    f"[FAIL] file API regression rc={completed.returncode}: "
                    f"{command_text}", file=sys.stderr,
                )
            else:
                print(f"[PASS] file API regression: {command_text}")
        except (OSError, subprocess.TimeoutExpired) as error:
            failures += 1
            print(
                f"[FAIL] file API regression: {command_text}: {error}",
                file=sys.stderr,
            )

    if failures:
        print(f"STREAM API ACCEPTANCE: FAIL ({failures} failure(s))")
        return 1
    print(f"STREAM API ACCEPTANCE: PASS ({len(cases)} transfer case(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
