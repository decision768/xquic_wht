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
SNAPSHOT_RE = re.compile(r"\[stream_api_snapshot\]\s+(?P<fields>.*)")
LANE_RE = re.compile(r"\[stream_api_lane\]\s+(?P<fields>.*)")
FIELD_RE = re.compile(r"(?P<key>[a-z_]+)=(?P<value>[^\s]+)")
FIXED_LANES = 4


@dataclass(frozen=True)
class TransferCase:
    name: str
    size: int
    seed: int
    send_queue: int = 8 * 1024 * 1024
    receive_queue: int = 8 * 1024 * 1024
    flags: int = 0
    require_backpressure: bool = False
    callback_delay_ms: int = 0
    accept_delay_ms: int = 0
    require_receiver_credit: bool = False
    require_delayed_accept: bool = False
    write_chunk_size: int = 0
    require_tiny_writes: bool = False


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


def snapshot_fields(output: str, role: str) -> dict[str, str]:
    result: dict[str, str] = {}
    for line in output.splitlines():
        match = SNAPSHOT_RE.search(line)
        if not match:
            continue
        values = {
            item.group("key"): item.group("value")
            for item in FIELD_RE.finditer(match.group("fields"))
        }
        if values.get("role") == role:
            result = values
    return result


def integer(values: dict[str, str], key: str) -> int:
    value = values.get(key, "")
    if not value.isdigit():
        raise ValueError(f"missing or invalid {key}={value!r}")
    return int(value)


def lane_values(output: str, role: str, value_name: str) -> dict[int, int]:
    values = {}
    for line in output.splitlines():
        match = LANE_RE.search(line)
        if not match:
            continue
        lane_fields = {
            item.group("key"): item.group("value")
            for item in FIELD_RE.finditer(match.group("fields"))
        }
        if lane_fields.get("role") != role:
            continue
        lane = integer(lane_fields, "lane")
        values[lane] = integer(lane_fields, value_name)
    return values


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
    seed: int,
    send_queue: int,
    receive_queue: int,
    flags: int,
    callback_delay_ms: int,
    accept_delay_ms: int,
    write_chunk_size: int,
    timeout_s: float,
) -> tuple[int, str, int, str]:
    api_timeout_ms = max(3000, int(timeout_s * 1000))
    with tempfile.TemporaryDirectory(prefix="netins-stream-test-") as temp_dir:
        server_log_path = Path(temp_dir) / "server.log"
        server_log = server_log_path.open("w", encoding="utf-8")
        server_command = [
            str(server_binary), listen_ip, str(port), str(size),
            str(seed), str(receive_queue), str(api_timeout_ms),
        ]
        if callback_delay_ms:
            server_command.extend([
                "--callback-delay-ms", str(callback_delay_ms),
            ])
        if accept_delay_ms:
            server_command.extend([
                "--accept-delay-ms", str(accept_delay_ms),
            ])
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
                str(seed), str(send_queue), str(receive_queue), str(flags),
                str(api_timeout_ms), str(api_timeout_ms),
                "--scheduler", "adaptive",
            ]
            if write_chunk_size:
                client_command.extend([
                    "--write-chunk-size", str(write_chunk_size),
                ])
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


def run_multi_pair(
    server_binary: Path,
    client_binary: Path,
    listen_ip: str,
    host: str,
    port: int,
    size: int,
    seed: int,
    timeout_s: float,
) -> tuple[int, str, list[tuple[int, str]]]:
    api_timeout_ms = max(3000, int(timeout_s * 1000))
    with tempfile.TemporaryDirectory(prefix="netins-stream-multi-") as temp_dir:
        server_log_path = Path(temp_dir) / "server.log"
        with server_log_path.open("w", encoding="utf-8") as server_log:
            server = subprocess.Popen([
                str(server_binary), listen_ip, str(port), str(size),
                str(seed), str(8 * 1024 * 1024), str(api_timeout_ms), "2",
            ], stdout=server_log, stderr=subprocess.STDOUT, text=True)
        clients: list[subprocess.Popen[str]] = []
        try:
            deadline = time.monotonic() + min(10.0, timeout_s)
            while time.monotonic() < deadline:
                if "[stream_api_ready]" in read_text(server_log_path):
                    break
                if server.poll() is not None:
                    break
                time.sleep(0.05)
            if "[stream_api_ready]" not in read_text(server_log_path):
                stop_process(server)
                return server.returncode or 1, read_text(server_log_path), []
            command = [
                str(client_binary), host, str(port), str(size), str(seed),
                str(8 * 1024 * 1024), str(8 * 1024 * 1024), "0",
                str(api_timeout_ms), str(api_timeout_ms),
                "--scheduler", "adaptive",
            ]
            clients = [subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                text=True,
            ) for _ in range(2)]
            client_results = []
            for client in clients:
                try:
                    output, _ = client.communicate(timeout=timeout_s + 5)
                    client_results.append((client.returncode, output))
                except subprocess.TimeoutExpired:
                    stop_process(client)
                    output, _ = client.communicate()
                    client_results.append((124, output + "\nclient timeout"))
            try:
                server_rc = server.wait(timeout=timeout_s + 5)
            except subprocess.TimeoutExpired:
                stop_process(server)
                server_rc = 124
            return server_rc, read_text(server_log_path), client_results
        finally:
            for client in clients:
                stop_process(client)
            stop_process(server)


def run_server_abort(
    server_binary: Path,
    client_binary: Path,
    listen_ip: str,
    host: str,
    port: int,
    timeout_s: float,
) -> tuple[int, str, int, str]:
    with tempfile.TemporaryDirectory(prefix="netins-stream-abort-") as temp_dir:
        server_log_path = Path(temp_dir) / "server.log"
        with server_log_path.open("w", encoding="utf-8") as server_log:
            server = subprocess.Popen([
                str(server_binary), listen_ip, str(port), str(4 << 30),
                "101", str(8 * 1024 * 1024), "10000",
            ], stdout=server_log, stderr=subprocess.STDOUT, text=True)
        client = None
        try:
            deadline = time.monotonic() + min(10.0, timeout_s)
            while time.monotonic() < deadline:
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
            client = subprocess.Popen([
                str(client_binary), host, str(port), str(4 << 30), "101",
                "65536", str(8 * 1024 * 1024), "0", "0", "0",
                "--scheduler", "adaptive",
            ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            progress_deadline = time.monotonic() + min(15.0, timeout_s)
            while time.monotonic() < progress_deadline:
                if "[stream_api_progress] role=server" in read_text(
                        server_log_path):
                    break
                if client.poll() is not None or server.poll() is not None:
                    break
                time.sleep(0.05)
            established = (
                "[stream_api_progress] role=server"
                in read_text(server_log_path)
            )
            if not established:
                stop_process(client)
                client_output, _ = client.communicate()
                stop_process(server)
                return (
                    server.returncode or 1,
                    read_text(server_log_path),
                    client.returncode or 1,
                    client_output
                    + "\nclient did not establish an in-progress stream",
                )
            stop_process(server)
            server_rc = server.returncode or 1
            try:
                client_output, _ = client.communicate(
                    timeout=max(15.0, timeout_s)
                )
                client_rc = client.returncode
            except subprocess.TimeoutExpired:
                stop_process(client)
                client_output, _ = client.communicate()
                client_rc = 124
            return (server_rc, read_text(server_log_path),
                    client_rc, client_output)
        finally:
            if client is not None:
                stop_process(client)
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
    client_lanes = lane_values(client_output, "client", "sent")
    server_lanes = lane_values(server_output, "server", "received")
    if (len(client_lanes) != FIXED_LANES
        or sum(client_lanes.values()) != case.size):
        raise ValueError("client per-lane counters do not sum to total bytes")
    if (len(server_lanes) != FIXED_LANES
        or sum(server_lanes.values()) != case.size):
        raise ValueError("server per-lane counters do not sum to total bytes")
    if case.size >= FIXED_LANES * 65536:
        if any(value == 0 for value in client_lanes.values()):
            raise ValueError("one or more configured lanes carried no payload")
        if any(value == 0 for value in server_lanes.values()):
            raise ValueError("receiver observed one or more idle lanes")
    if case.require_backpressure:
        if integer(client_values, "eagain") == 0:
            raise ValueError("nonblocking case did not observe EAGAIN")
        if integer(client_values, "partial") == 0:
            raise ValueError("nonblocking case did not observe a partial write")
    if case.require_receiver_credit:
        if case.size <= case.receive_queue:
            raise ValueError("receiver-credit case fits in its initial credit")
        snapshot = snapshot_fields(server_output, "server")
        delivered = integer(snapshot, "records_delivered")
        high_water = integer(snapshot, "reorder_high_water")
        if delivered <= 1:
            raise ValueError("receiver-credit case did not deliver multiple records")
        if high_water == 0 or high_water > case.receive_queue:
            raise ValueError(
                "receiver queue high-water exceeded its advertised credit"
            )
        expected_delay = f"callback_delay_ms={case.callback_delay_ms}"
        expected_queue = f"receive_queue={case.receive_queue}"
        if (expected_delay not in server_output
                or expected_queue not in server_output):
            raise ValueError(
                "server did not enable the requested receiver-credit limits"
            )
    if case.require_delayed_accept:
        expected_delay = f"delay_ms={case.accept_delay_ms}"
        if ("[stream_api_accept_delay]" not in server_output
                or expected_delay not in server_output):
            raise ValueError("server did not exercise delayed stream_accept")
        snapshot = snapshot_fields(server_output, "server")
        if integer(snapshot, "preaccept_lanes") != FIXED_LANES:
            raise ValueError(
                "all four complete lane headers were not buffered before "
                "stream_accept"
            )
    if case.require_tiny_writes:
        if case.write_chunk_size != 1:
            raise ValueError("tiny-write case must use one-byte API writes")
        client_snapshot = snapshot_fields(client_output, "client")
        server_snapshot = snapshot_fields(server_output, "server")
        if integer(client_snapshot, "records_enqueued") != case.size:
            raise ValueError("one-byte writes were not framed independently")
        if integer(server_snapshot, "records_delivered") != case.size:
            raise ValueError("receiver did not deliver every one-byte record")


def print_failure(server_output: str, client_output: str) -> None:
    print("--- server output ---", file=sys.stderr)
    print(server_output.rstrip(), file=sys.stderr)
    print("--- client output ---", file=sys.stderr)
    print(client_output.rstrip(), file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Run protocol, ordering, local/receiver backpressure, "
            "delayed-accept and loopback checks"
        )
    )
    parser.add_argument("--bin-dir", type=Path, default=Path("build/mini"))
    parser.add_argument("--listen-ip", default="0.0.0.0")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--base-port", type=int, default=18460)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--full", action="store_true",
                        help="also run a 64 MiB fixed-4-lane case")
    parser.add_argument(
        "--release", action="store_true",
        help="require full cases, a real library and file-API regression",
    )
    parser.add_argument("--skip-unit", action="store_true")
    parser.add_argument("--skip-source-checks", action="store_true")
    parser.add_argument(
        "--source-root", type=Path,
        help="source or delivery root; defaults to the script's parent root",
    )
    parser.add_argument("--skip-fault", action="store_true")
    parser.add_argument(
        "--library", type=Path,
        help="optional built xquic library for exported-symbol verification",
    )
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
    if args.release and (
        not args.full or args.library is None
        or not args.file_regression_command
        or args.skip_unit or args.skip_source_checks or args.skip_fault
    ):
        parser.error(
            "--release requires --full, --library, at least one "
            "--file-regression-command, and all checks enabled"
        )

    try:
        server_binary = executable(args.bin_dir, "netins_udp_stream_server")
        client_binary = executable(args.bin_dir, "netins_udp_stream_client")
    except FileNotFoundError as error:
        print(f"ACCEPTANCE: FAIL: {error}", file=sys.stderr)
        return 1

    failures = 0
    if not args.skip_source_checks:
        repo_root = (args.source_root.resolve()
                     if args.source_root is not None
                     else Path(__file__).resolve().parents[1])
        abi_command = [
            sys.executable,
            str(repo_root / "scripts/mini_api/check_abi.py"),
            "--header", str(repo_root / "include/xquic/netins_udp.h"),
            "--version-script", str(repo_root / "scripts/xquic.lds"),
        ] + (["--library", str(args.library.resolve())]
             if args.library is not None else [])
        source_checks = [abi_command]
        internal_sources = (
            repo_root / "mini/mini_client.c",
            repo_root / "mini/mini_server.c",
            repo_root / "mini/mini_client_backend.c",
            repo_root / "src/netins_udp/netins_udp_api.c",
        )
        if all(path.is_file() for path in internal_sources):
            source_checks.append(
                [sys.executable,
                 str(repo_root / "scripts/mini_api/check_stream_isolation.py"),
                 "--root", str(repo_root)]
            )
        else:
            print("[SKIP] internal-source isolation check (delivery package)")
        for command in source_checks:
            completed = subprocess.run(
                command, cwd=repo_root, capture_output=True,
                text=True, check=False,
            )
            if completed.returncode != 0:
                failures += 1
                print(f"[FAIL] {' '.join(command)}", file=sys.stderr)
                print(completed.stdout + completed.stderr, file=sys.stderr)
            else:
                print(completed.stdout.strip())

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
        TransferCase("zero", 0, 1),
        TransferCase("one-byte", 1, 2),
        TransferCase("below-record", 65535, 3),
        TransferCase("one-record", 65536, 4),
        TransferCase("over-record", 65537, 5),
        TransferCase(
            "repeated-one-byte-writes", 1024, 11,
            send_queue=65536, receive_queue=65536,
            write_chunk_size=1, require_tiny_writes=True,
        ),
        TransferCase("multi-lane", 8 * 1024 * 1024, 6),
        TransferCase(
            "nonblocking-send-queue-backpressure", 8 * 1024 * 1024, 7,
            send_queue=65536, flags=1, require_backpressure=True,
        ),
        TransferCase(
            "receiver-credit-slow-callback", 2 * 1024 * 1024, 8,
            receive_queue=65536, callback_delay_ms=20,
            require_receiver_credit=True,
        ),
        TransferCase(
            "delayed-accept-data", 1024 * 1024, 9,
            accept_delay_ms=500, require_delayed_accept=True,
        ),
        TransferCase(
            "delayed-accept-empty", 0, 10,
            accept_delay_ms=500, require_delayed_accept=True,
        ),
    ]
    if args.full:
        cases.append(TransferCase(
            "64MiB-fixed-4-lane", 64 * 1024 * 1024, 24,
        ))

    for index, case in enumerate(cases):
        port = args.base_port + index
        server_rc, server_output, client_rc, client_output = run_pair(
            server_binary, client_binary, args.listen_ip, args.host, port,
            case.size, case.seed, case.send_queue, case.receive_queue,
            case.flags, case.callback_delay_ms, case.accept_delay_ms,
            case.write_chunk_size, args.timeout,
        )
        try:
            validate_success(
                case, server_rc, server_output, client_rc, client_output
            )
            print(f"[PASS] {case.name}: bytes={case.size} lanes={FIXED_LANES}")
        except (RuntimeError, ValueError) as error:
            failures += 1
            print(f"[FAIL] {case.name}: {error}", file=sys.stderr)
            print_failure(server_output, client_output)

    multi_port = args.base_port + len(cases)
    server_rc, server_output, client_results = run_multi_pair(
        server_binary, client_binary, args.listen_ip, args.host,
        multi_port, 2 * 1024 * 1024, 88, args.timeout,
    )
    server_successes = sum(
        1 for line in server_output.splitlines()
        if "[stream_api_result]" in line
        and "role=server" in line and "success=1" in line
    )
    multi_ok = server_rc == 0 and server_successes == 2
    for client_rc, client_output in client_results:
        values = fields(client_output, "client")
        multi_ok = multi_ok and client_rc == 0 and values.get("success") == "1"
    if multi_ok and len(client_results) == 2:
        print("[PASS] two concurrent accepted connections")
    else:
        failures += 1
        print("[FAIL] concurrent accepted connections", file=sys.stderr)
        print_failure(
            server_output,
            "\n--- client ---\n".join(output for _, output in client_results),
        )

    if not args.skip_fault:
        abort_port = multi_port + 1
        server_rc, server_output, client_rc, client_output = run_server_abort(
            server_binary, client_binary, args.listen_ip, args.host,
            abort_port, args.timeout,
        )
        abort_snapshot = snapshot_fields(client_output, "client")
        abort_last_error = abort_snapshot.get("last_error", "")
        abort_ok = (
            server_rc != 0 and client_rc != 0
            and client_rc != 124
            and "[stream_api_ready]" in server_output
            and "[stream_api_progress] role=server" in server_output
            and "success=1" not in client_output
            and abort_last_error.isdigit()
            and int(abort_last_error) != 0
        )
        if abort_ok:
            print("[PASS] server interruption rejected with observable error")
        else:
            failures += 1
            print("[FAIL] server interruption was not observed", file=sys.stderr)
            print_failure(server_output, client_output)

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
    if not args.release:
        print("STREAM API DEVELOPMENT ACCEPTANCE: PASS "
              f"({len(cases)} transfer case(s)); release mode not requested")
        return 0
    print(f"STREAM API RELEASE ACCEPTANCE: PASS ({len(cases)} transfer case(s))")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
