#!/usr/bin/env python3
# -*- coding: utf-8 -*-
from __future__ import annotations

import argparse
import csv
import getpass
import logging
import math
import os
import re
import shlex
import statistics
import sys
import time
import uuid
from dataclasses import dataclass, field
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

try:
    import paramiko
except ImportError:  # Keep --help usable before paramiko is installed.
    paramiko = None


LOG = logging.getLogger("stream_api_benchmark")


# =============================================================================
# 用户常用配置区
# =============================================================================
DEFAULT_BOARD_SSH_HOST = "192.168.11.1"
DEFAULT_SERVER_SSH_HOST = "124.70.219.194"
DEFAULT_QUIC_HOST = "124.70.219.194"
DEFAULT_QUIC_PORT = 18446

DEFAULT_BOARD_DIR = "/oemdata/gx/xquic_file"
DEFAULT_SERVER_DIR = "/root/test_xquic_server"
DEFAULT_CLIENT_EXECUTABLE = "./netins_udp_stream_client"
DEFAULT_SERVER_EXECUTABLE = "./netins_udp_stream_server"

TRANSFER_SIZE_MIB = 100
TRANSFER_BYTES = TRANSFER_SIZE_MIB * 1024 * 1024
DEFAULT_ITERATIONS = 10
DEFAULT_RETRIES = 1
DEFAULT_COOLDOWN_S = 2.0
DEFAULT_SCHEDULER = "balanced"
DEFAULT_SEED = 7

DEFAULT_WRITE_TIMEOUT_MS = 600000
DEFAULT_FINISH_TIMEOUT_MS = 600000
DEFAULT_COMMAND_TIMEOUT_S = 690.0
DEFAULT_SERVER_READY_TIMEOUT_S = 15.0
DEFAULT_RESOURCE_INTERVAL_S = 0.5
DEFAULT_BOARD_MONITOR_SCRIPT = "/oemdata/monitor_board_resource.sh"
DEFAULT_SERVER_MONITOR_SCRIPT = (
    "/root/test_xquic_server/monitor_server_memory.sh"
)
# =============================================================================
# 用户常用配置区结束
# =============================================================================


MODE_INTERFACES = {
    "rmnet": ("rmnet_data0",),
    "usb": ("usb0",),
    "agg": ("rmnet_data0", "usb0"),
}
DISPLAY_MODE_NAMES = {
    "rmnet": "5G1",
    "usb": "5G2",
    "agg": "Agg",
}
DIRECTION_ORDER = ("get", "post")
MODE_ORDER = ("rmnet", "agg", "usb")
SCHEDULERS = (
    "default",
    "minrtt",
    "backup",
    "balanced",
    "rap",
    "act",
    "bandwidth",
    "fastprobe",
    "fastprobe5",
    "fastprobe10",
    "fastprobe20",
    "fastonly",
    "adaptive",
)

FIELD_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)=([^\s\r\n]+)")
RESULT_MARKER = "[stream_api_result]"
CONFIG_MARKER = "[stream_api_config]"
INTERFACE_MARKER = "[stream_api_interface]"
LANE_MARKER = "[stream_api_lane]"
TRANSFER_MARKER = "[stream_api_transfer]"
EXPECTED_TRANSFER_METRICS = {
    "get": "first_body_to_last_body",
    "post": "first_write_to_xquic_flush",
}


@dataclass
class CommandResult:
    output: str
    stderr: str
    exit_code: int
    elapsed_s: float
    timed_out: bool = False
    truncated: bool = False

    @property
    def diagnostic_output(self) -> str:
        if self.output and self.stderr:
            return self.output + "\n" + self.stderr
        return self.output or self.stderr


class SSHRemote:
    """Paramiko runner that preserves stdout/stderr and enforces a timeout."""

    def __init__(
        self,
        hostname: str,
        username: str,
        password: Optional[str],
        key_filename: Optional[str],
        port: int,
        strict_host_key: bool,
        label: str,
    ) -> None:
        self.hostname = hostname
        self.username = username
        self.password = password or None
        self.key_filename = key_filename or None
        self.port = port
        self.strict_host_key = strict_host_key
        self.label = label
        self.client = None

    def connect(self) -> None:
        if paramiko is None:
            raise RuntimeError(
                "缺少 paramiko，请先执行: python -m pip install paramiko"
            )
        self.close()
        client = paramiko.SSHClient()
        client.load_system_host_keys()
        if self.strict_host_key:
            client.set_missing_host_key_policy(paramiko.RejectPolicy())
        else:
            client.set_missing_host_key_policy(paramiko.AutoAddPolicy())

        connect_args = {
            "hostname": self.hostname,
            "port": self.port,
            "username": self.username,
            "timeout": 15,
            "banner_timeout": 15,
            "auth_timeout": 20,
            "channel_timeout": 30,
        }
        if self.key_filename:
            connect_args.update(
                {
                    "key_filename": self.key_filename,
                    "passphrase": self.password,
                    "allow_agent": False,
                    "look_for_keys": False,
                }
            )
        elif self.password:
            connect_args.update(
                {
                    "password": self.password,
                    "allow_agent": False,
                    "look_for_keys": False,
                }
            )
        else:
            connect_args.update({"allow_agent": True, "look_for_keys": True})

        client.connect(**connect_args)
        transport = client.get_transport()
        if transport is None or not transport.is_active():
            client.close()
            raise RuntimeError(f"{self.label} SSH transport 未激活")
        transport.set_keepalive(30)
        key = transport.get_remote_server_key()
        fingerprint = ":".join(
            f"{value:02x}" for value in key.get_fingerprint()
        )
        LOG.info(
            "%s SSH已连接: %s@%s:%s host_key=%s %s",
            self.label,
            self.username,
            self.hostname,
            self.port,
            key.get_name(),
            fingerprint,
        )
        self.client = client

    def is_connected(self) -> bool:
        if self.client is None:
            return False
        transport = self.client.get_transport()
        return bool(transport and transport.is_active())

    def run(
        self,
        command: str,
        timeout_s: float,
        output_path: Optional[Path] = None,
        stderr_path: Optional[Path] = None,
        max_capture_bytes: int = 16 * 1024 * 1024,
    ) -> CommandResult:
        if not self.is_connected():
            raise RuntimeError(f"{self.label} SSH未连接")
        transport = self.client.get_transport()
        channel = transport.open_session(timeout=min(float(timeout_s), 30.0))
        channel.set_combine_stderr(False)
        channel.exec_command(command)

        started = time.monotonic()
        deadline = started + float(timeout_s)
        captured = bytearray()
        stderr_captured = bytearray()
        truncated = False
        stderr_truncated = False
        sink = output_path.open("wb") if output_path is not None else None
        stderr_sink = (
            stderr_path.open("wb") if stderr_path is not None else None
        )
        stable_after_exit = None
        try:
            while True:
                made_progress = False
                while channel.recv_ready():
                    chunk = channel.recv(65536)
                    if not chunk:
                        break
                    made_progress = True
                    if sink is not None:
                        sink.write(chunk)
                        sink.flush()
                    captured.extend(chunk)
                    if len(captured) > max_capture_bytes:
                        del captured[: len(captured) - max_capture_bytes]
                        truncated = True

                while channel.recv_stderr_ready():
                    chunk = channel.recv_stderr(65536)
                    if not chunk:
                        break
                    made_progress = True
                    if stderr_sink is not None:
                        stderr_sink.write(chunk)
                        stderr_sink.flush()
                    stderr_captured.extend(chunk)
                    if len(stderr_captured) > max_capture_bytes:
                        del stderr_captured[
                            : len(stderr_captured) - max_capture_bytes
                        ]
                        stderr_truncated = True

                if channel.exit_status_ready():
                    if made_progress or stable_after_exit is None:
                        stable_after_exit = time.monotonic()
                    elif time.monotonic() - stable_after_exit >= 0.20:
                        exit_code = channel.recv_exit_status()
                        break

                if time.monotonic() >= deadline:
                    channel.close()
                    return CommandResult(
                        output=captured.decode("utf-8", errors="replace"),
                        stderr=stderr_captured.decode(
                            "utf-8", errors="replace"
                        ),
                        exit_code=124,
                        elapsed_s=time.monotonic() - started,
                        timed_out=True,
                        truncated=truncated or stderr_truncated,
                    )
                if not made_progress:
                    time.sleep(0.05)
        finally:
            if sink is not None:
                sink.close()
            if stderr_sink is not None:
                stderr_sink.close()
            channel.close()

        return CommandResult(
            output=captured.decode("utf-8", errors="replace"),
            stderr=stderr_captured.decode("utf-8", errors="replace"),
            exit_code=exit_code,
            elapsed_s=time.monotonic() - started,
            truncated=truncated or stderr_truncated,
        )

    def close(self) -> None:
        client = self.client
        self.client = None
        if client is not None:
            try:
                client.close()
            except Exception:
                pass

    def download(
        self,
        remote_path: str,
        local_path: Path,
        socket_timeout_s: float = 120.0,
    ) -> None:
        if not self.is_connected():
            raise RuntimeError(f"{self.label} SSH未连接")
        local_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = local_path.with_suffix(local_path.suffix + ".part")
        sftp = self.client.open_sftp()
        try:
            sftp.get_channel().settimeout(socket_timeout_s)
            sftp.get(remote_path, str(temporary))
            os.replace(temporary, local_path)
        except Exception:
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass
            raise
        finally:
            sftp.close()


@dataclass
class ServerRun:
    token: str
    pid: int
    wrapper_pid: int
    state_dir: str
    stdout_path: str
    stderr_path: str
    done_path: str
    server_rc_path: str
    resource_csv_path: str
    resource_stop_path: str
    monitor_rc_path: str


@dataclass
class AttemptRecord:
    start_time: str
    end_time: str
    round: int
    direction: str
    mode: str
    display_mode: str
    attempt: int
    success: bool
    bytes: int
    interfaces: str
    scheduler: str
    payload_elapsed_us: int
    payload_throughput_mbps: float
    client_exit: int
    error: str
    board_resource: Dict[str, object] = field(default_factory=dict)
    server_resource: Dict[str, object] = field(default_factory=dict)


def q(value: object) -> str:
    return shlex.quote(str(value))


def configure_logging() -> None:
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(
        logging.Formatter(
            "%(asctime)s %(levelname)s %(message)s", "%Y-%m-%d %H:%M:%S"
        )
    )
    LOG.handlers.clear()
    LOG.addHandler(handler)
    LOG.setLevel(logging.INFO)


def connect_with_optional_prompt(remote: SSHRemote, password_label: str) -> None:
    try:
        remote.connect()
        return
    except Exception as first_error:
        if remote.password or remote.key_filename or not sys.stdin.isatty():
            raise
        LOG.warning("%s首次SSH认证失败: %s", remote.label, first_error)
    remote.password = getpass.getpass(f"{password_label} SSH密码: ")
    remote.connect()


def parse_marker_lines(output: str, marker: str) -> List[Dict[str, str]]:
    records: List[Dict[str, str]] = []
    for line in output.splitlines():
        if marker not in line:
            continue
        marker_index = line.find(marker)
        fields_text = line[marker_index + len(marker) :]
        records.append({key: value for key, value in FIELD_RE.findall(fields_text)})
    return records


def parse_positive_int(value: str, name: str, errors: List[str]) -> int:
    if not re.fullmatch(r"\d+", value or ""):
        errors.append(f"{name}不是非负整数")
        return 0
    return int(value)


def last_role_record(
    output: str, marker: str, role: str
) -> Optional[Dict[str, str]]:
    records = [
        record
        for record in parse_marker_lines(output, marker)
        if record.get("role") == role
    ]
    return records[-1] if records else None


def parse_lane_bytes(output: str, role: str, field: str) -> Dict[int, int]:
    values: Dict[int, int] = {}
    for record in parse_marker_lines(output, LANE_MARKER):
        if record.get("role") != role:
            continue
        if not re.fullmatch(r"\d+", record.get("lane", "")):
            continue
        if not re.fullmatch(r"\d+", record.get(field, "")):
            continue
        values[int(record["lane"])] = int(record[field])
    return values


def parse_payload_transfer_metric(
    output: str, expected_bytes: int, direction: str, errors: List[str]
) -> Tuple[int, float]:
    """Return the client metric using file-API-like data-plane boundaries."""
    records = [
        record
        for record in parse_marker_lines(output, TRANSFER_MARKER)
        if record.get("role") == "client"
        and record.get("direction") == direction
    ]
    if not records:
        errors.append("缺少client stream_api_transfer")
        return 0, 0.0
    record = records[-1]
    expected_metric = EXPECTED_TRANSFER_METRICS[direction]
    if record.get("metric") != expected_metric:
        errors.append(
            f"client metric={record.get('metric', '<missing>')}，"
            f"预期{expected_metric}"
        )
    metric_bytes = parse_positive_int(
        record.get("bytes", ""), "client metric bytes", errors
    )
    elapsed_us = parse_positive_int(
        record.get("elapsed_us", ""), "client metric elapsed_us", errors
    )
    if metric_bytes != expected_bytes:
        errors.append(
            f"client metric bytes={metric_bytes}，预期{expected_bytes}"
        )
    if elapsed_us <= 0:
        errors.append("client metric elapsed_us必须大于0")
        return 0, 0.0
    throughput_mbps = expected_bytes * 8.0 / elapsed_us
    try:
        reported_mbps = float(record.get("throughput_mbps", ""))
    except ValueError:
        reported_mbps = 0.0
    tolerance_mbps = max(0.01, throughput_mbps * 0.0001)
    if (
        not math.isfinite(reported_mbps)
        or reported_mbps <= 0
        or abs(reported_mbps - throughput_mbps) > tolerance_mbps
    ):
        errors.append(
            "client metric throughput_mbps无效或与字节/时间不一致: "
            f"reported={reported_mbps:.6f} calculated={throughput_mbps:.6f}"
        )
    return elapsed_us, throughput_mbps


def validate_client_output(
    output: str,
    exit_code: int,
    expected_bytes: int,
    expected_interfaces: Sequence[str],
    expected_scheduler: str,
    direction: str,
) -> Tuple[Dict[str, object], List[str]]:
    errors: List[str] = []
    result = last_role_record(output, RESULT_MARKER, "client")
    config = last_role_record(output, CONFIG_MARKER, "client")
    if exit_code != 0:
        errors.append(f"client exit={exit_code}")
    if result is None:
        errors.append("缺少client stream_api_result")
        result = {}
    if config is None:
        errors.append("缺少client stream_api_config")
        config = {}

    success = parse_positive_int(result.get("success", ""), "success", errors)
    lanes = parse_positive_int(result.get("lanes", ""), "lanes", errors)
    accepted = parse_positive_int(
        result.get("accepted", ""), "accepted", errors
    )
    peer_received = parse_positive_int(
        result.get("peer_received", ""), "peer_received", errors
    )
    received = parse_positive_int(
        result.get("received", ""), "received", errors
    )
    eagain = parse_positive_int(result.get("eagain", ""), "eagain", errors)
    partial = parse_positive_int(result.get("partial", ""), "partial", errors)

    if success != 1:
        errors.append("client success不是1")
    if lanes != 4:
        errors.append(f"client lanes={lanes}，预期4")
    if accepted != expected_bytes:
        errors.append(f"accepted={accepted}，预期{expected_bytes}")
    if peer_received != expected_bytes:
        errors.append(f"peer_received={peer_received}，预期{expected_bytes}")
    if direction == "get" and received != expected_bytes:
        errors.append(f"received={received}，预期{expected_bytes}")

    config_lanes = parse_positive_int(
        config.get("lanes", ""), "config lanes", errors
    )
    config_interfaces = parse_positive_int(
        config.get("interfaces", ""), "config interfaces", errors
    )
    if config_lanes != 4:
        errors.append(f"client config lanes={config_lanes}，预期4")
    if config_interfaces != len(expected_interfaces):
        errors.append(
            f"client interfaces={config_interfaces}，预期{len(expected_interfaces)}"
        )
    if result.get("direction") != direction:
        errors.append(
            f"client result direction={result.get('direction', '<missing>')}，"
            f"预期{direction}"
        )
    if config.get("direction") != direction:
        errors.append(
            f"client config direction={config.get('direction', '<missing>')}，"
            f"预期{direction}"
        )
    if config.get("scheduler") != expected_scheduler:
        errors.append(
            f"scheduler={config.get('scheduler', '<missing>')}，"
            f"预期{expected_scheduler}"
        )

    actual_interfaces = [
        record.get("name", "")
        for record in parse_marker_lines(output, INTERFACE_MARKER)
    ]
    if actual_interfaces != list(expected_interfaces):
        errors.append(
            f"接口输出={actual_interfaces}，预期={list(expected_interfaces)}"
        )

    lane_field = "sent" if direction == "post" else "received"
    lane_bytes = parse_lane_bytes(output, "client", lane_field)
    if set(lane_bytes) != {0, 1, 2, 3}:
        errors.append(f"client lane统计不完整: {sorted(lane_bytes)}")
    elif sum(lane_bytes.values()) != expected_bytes:
        errors.append(
            f"client lane字节和={sum(lane_bytes.values())}，预期{expected_bytes}"
        )
    elif any(value <= 0 for value in lane_bytes.values()):
        errors.append("100MiB用例中存在未承载数据的lane")

    payload_elapsed_us, payload_throughput_mbps = (
        parse_payload_transfer_metric(
            output, expected_bytes, direction, errors
        )
    )

    parsed: Dict[str, object] = {
        "lanes": lanes,
        "payload_elapsed_us": payload_elapsed_us,
        "payload_throughput_mbps": payload_throughput_mbps,
        "eagain": eagain,
        "partial": partial,
        "lane_bytes": lane_bytes,
    }
    return parsed, errors


def board_preflight(board: SSHRemote, args) -> None:
    checks = [
        f"cd -- {q(args.board_dir)}",
        f"test -x {q(args.client_executable)}",
        f"test -x {q(args.board_monitor_script)}",
        "command -v bash >/dev/null",
        "command -v env >/dev/null",
        "command -v awk >/dev/null",
        "command -v date >/dev/null",
        "command -v getconf >/dev/null",
        "command -v grep >/dev/null",
        "command -v ip >/dev/null",
        "command -v tr >/dev/null",
        "test -r /proc/uptime",
    ]
    for interface in ("rmnet_data0", "usb0"):
        variable = interface.replace("-", "_")
        checks.append(f"ip -o link show dev {q(interface)} >/dev/null")
        checks.append(
            f"src_{variable}=$(ip -o -4 addr show dev {q(interface)} "
            "| awk '{print $4; exit}')"
        )
        checks.append(f"test -n \"$src_{variable}\"")
        if not args.skip_route_check:
            checks.append(
                f"ip -4 route get {q(args.quic_host)} "
                f"from \"${{src_{variable}%/*}}\" "
                f"oif {q(interface)} >/dev/null"
            )
    command = " && ".join(checks)
    result = board.run(command, timeout_s=30)
    if result.exit_code != 0:
        raise RuntimeError(
            "板卡预检失败。请确认Stream Client已放入指定目录、具有执行权限，"
            f"并确认两张网卡存在且有IPv4地址。\n{result.diagnostic_output}"
        )


def server_preflight(server: SSHRemote, args) -> None:
    checks = [
        f"cd -- {q(args.server_dir)}",
        f"test -x {q(args.server_executable)}",
        f"test -x {q(args.server_monitor_script)}",
        "command -v bash >/dev/null",
        "command -v env >/dev/null",
        "command -v nohup >/dev/null",
        "command -v awk >/dev/null",
        "command -v grep >/dev/null",
        "command -v ss >/dev/null",
        "command -v date >/dev/null",
        "command -v getconf >/dev/null",
        "command -v sed >/dev/null",
        "command -v tr >/dev/null",
    ]
    result = server.run(" && ".join(checks), timeout_s=30)
    if result.exit_code != 0:
        raise RuntimeError(
            "云服务器预检失败。请确认Stream Server已放入指定目录并具有执行权限。"
            f"\n{result.diagnostic_output}"
        )


def session_process_cleanup(remote: SSHRemote, token: str) -> None:
    if not re.fullmatch(r"stream_[A-Za-z0-9_]+", token):
        raise ValueError("invalid session token")
    script = (
        f"target={q('STREAM_API_BENCH_SESSION=' + token)}\n"
        "pids=\"\"\n"
        "for procdir in /proc/[0-9]*; do\n"
        "  [ -r \"$procdir/environ\" ] || continue\n"
        "  if tr '\\0' '\\n' < \"$procdir/environ\" 2>/dev/null "
        "| grep -Fqx -- \"$target\"; then\n"
        "    pids=\"$pids ${procdir##*/}\"\n"
        "  fi\n"
        "done\n"
        "for pid in $pids; do kill -TERM \"$pid\" 2>/dev/null || true; done\n"
        "sleep 0.2\n"
        "for pid in $pids; do kill -KILL \"$pid\" 2>/dev/null || true; done\n"
        "for pid in $pids; do wait \"$pid\" 2>/dev/null || true; done\n"
    )
    remote.run(f"bash -c {q(script)}", timeout_s=10)


def start_server(
    server: SSHRemote, args, token: str, seed: int, direction: str
) -> ServerRun:
    if not re.fullmatch(r"stream_[A-Za-z0-9_]+", token):
        raise ValueError("invalid session token")
    state_dir = f"/tmp/stream_api_bench_{token}"
    stdout_path = f"{state_dir}/server.stdout.log"
    stderr_path = f"{state_dir}/server.stderr.log"
    pid_path = f"{state_dir}/server.pid"
    done_path = f"{state_dir}/server.done"
    resource_csv_path = f"{state_dir}/server_resource.csv"
    resource_stop_path = f"{state_dir}/server_resource.stop"
    resource_stdout_path = f"{state_dir}/server_monitor.stdout.log"
    resource_stderr_path = f"{state_dir}/server_monitor.stderr.log"
    server_rc_path = f"{state_dir}/server.rc"
    monitor_rc_path = f"{state_dir}/server_monitor.rc"
    argv = [
        args.server_executable,
        args.server_bind,
        str(args.quic_port),
        str(args.total_bytes),
        str(seed),
        str(args.finish_timeout_ms),
        "1",
        f"--{direction}",
    ]
    if direction == "get":
        argv.extend(
            [
                "--write-timeout-ms",
                str(args.write_timeout_ms),
                "--flags",
                str(args.flags),
            ]
        )
    server_command = " ".join(q(item) for item in argv)
    process_name = args.server_executable.rsplit("/", 1)[-1]
    wrapper_script = (
        "set -uo pipefail\n"
        "export LC_ALL=C\n"
        f"cd -- {q(args.server_dir)} || exit 90\n"
        f"{server_command} > {q(stdout_path)} 2> {q(stderr_path)} &\n"
        "server_pid=$!\n"
        "server_start=$(awk '{print $22}' \"/proc/$server_pid/stat\" "
        "2>/dev/null || true)\n"
        f"printf '%s %s\\n' \"$server_pid\" \"$server_start\" > {q(pid_path)}\n"
        f"{q(args.server_monitor_script)} {q(resource_csv_path)} "
        f"{q(resource_stop_path)} {q(args.resource_interval_s)} "
        f"{q(process_name)} {q(pid_path)} 0 > {q(resource_stdout_path)} "
        f"2> {q(resource_stderr_path)} &\n"
        "monitor_pid=$!\n"
        "server_rc=0\n"
        "wait \"$server_pid\" || server_rc=$?\n"
        f"touch -- {q(resource_stop_path)}\n"
        "monitor_rc=0\n"
        "wait \"$monitor_pid\" || monitor_rc=$?\n"
        f"printf '%s\\n' \"$server_rc\" > {q(server_rc_path)}\n"
        f"printf '%s\\n' \"$monitor_rc\" > {q(monitor_rc_path)}\n"
        f"touch -- {q(done_path)}\n"
        "exit 0\n"
    )
    port_pattern = f":{args.quic_port}([[:space:]]|$)"
    command = (
        f"cd -- {q(args.server_dir)} || exit 90; "
        "if ss -H -lunp 2>/dev/null "
        f"| grep -Eq {q(port_pattern)}; then "
        "echo '__STREAM_PORT_BUSY__'; exit 73; fi; "
        f"mkdir -- {q(state_dir)} || exit 74; "
        f"chmod 700 {q(state_dir)} || exit 76; "
        f"rm -f -- {q(done_path)} {q(pid_path)}; "
        f"nohup env STREAM_API_BENCH_SESSION={q(token)} "
        f"bash -c {q(wrapper_script)} > {q(state_dir + '/wrapper.stdout.log')} "
        f"2> {q(state_dir + '/wrapper.stderr.log')} < /dev/null & "
        "wrapper_pid=$!; "
        "count=0; while [ ! -s " + q(pid_path) + " ] && [ \"$count\" -lt 100 ]; do "
        "if ! kill -0 \"$wrapper_pid\" 2>/dev/null; then break; fi; "
        "sleep 0.05; count=$((count + 1)); done; "
        f"if ! test -s {q(pid_path)}; then "
        f"cat {q(state_dir + '/wrapper.stderr.log')} {q(stderr_path)} "
        "2>/dev/null; exit 75; fi; "
        f"read server_pid server_start < {q(pid_path)}; "
        "if ! kill -0 \"$server_pid\" 2>/dev/null; then "
        f"cat {q(stdout_path)} {q(stderr_path)} 2>/dev/null; exit 75; fi; "
        "printf '__STREAM_SERVER_PID__=%s\\n' \"$server_pid\"; "
        "printf '__STREAM_WRAPPER_PID__=%s\\n' \"$wrapper_pid\""
    )
    result = server.run(command, timeout_s=20)
    match = re.search(r"__STREAM_SERVER_PID__=(\d+)", result.output)
    wrapper_match = re.search(r"__STREAM_WRAPPER_PID__=(\d+)", result.output)
    if result.exit_code != 0 or match is None or wrapper_match is None:
        raise RuntimeError(
            f"启动Stream Server失败: exit={result.exit_code}\n"
            f"{result.diagnostic_output}"
        )
    return ServerRun(
        token=token,
        pid=int(match.group(1)),
        wrapper_pid=int(wrapper_match.group(1)),
        state_dir=state_dir,
        stdout_path=stdout_path,
        stderr_path=stderr_path,
        done_path=done_path,
        server_rc_path=server_rc_path,
        resource_csv_path=resource_csv_path,
        resource_stop_path=resource_stop_path,
        monitor_rc_path=monitor_rc_path,
    )


def wait_server_ready(
    server: SSHRemote, run: ServerRun, timeout_s: float
) -> None:
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        command = (
            f"if test -f {q(run.stdout_path)} && "
            f"grep -Fq '[stream_api_ready]' {q(run.stdout_path)}; then "
            "echo READY; "
            f"elif kill -0 {run.pid} 2>/dev/null; then echo WAIT; "
            "else echo DEAD; "
            f"cat {q(run.stdout_path)} {q(run.stderr_path)} 2>/dev/null; "
            "exit 1; fi"
        )
        result = server.run(command, timeout_s=10)
        if "READY" in result.output:
            return
        if result.exit_code != 0 or "DEAD" in result.output:
            raise RuntimeError(
                "Stream Server在ready前退出:\n" + result.diagnostic_output
            )
        time.sleep(0.10)
    raise RuntimeError(f"等待Stream Server ready超时({timeout_s}s)")


def stop_server_resource_monitor(server: SSHRemote, run: ServerRun) -> str:
    command = (
        f"if test -f {q(run.monitor_rc_path)}; then "
        f"cat -- {q(run.monitor_rc_path)}; exit 0; fi; "
        f"touch -- {q(run.resource_stop_path)} || exit 2; "
        "count=0; "
        f"while test -e {q(run.resource_stop_path)} "
        f"&& ! test -f {q(run.monitor_rc_path)} "
        "&& test \"$count\" -lt 100; do "
        "sleep 0.05; count=$((count + 1)); done; "
        f"if test -f {q(run.monitor_rc_path)}; then "
        f"cat -- {q(run.monitor_rc_path)}; "
        f"elif ! test -e {q(run.resource_stop_path)}; then echo 0; "
        "else exit 3; fi"
    )
    result = server.run(command, timeout_s=10)
    value = result.output.strip()
    if result.exit_code != 0 or not value.isdigit():
        return "MONITOR_STOP_ERROR"
    return "OK" if value == "0" else f"MONITOR_RC_{value}"


def cleanup_server_token(server: SSHRemote, token: str) -> None:
    if not re.fullmatch(r"stream_[A-Za-z0-9_]+", token):
        raise ValueError("invalid session token")
    state_dir = f"/tmp/stream_api_bench_{token}"
    try:
        session_process_cleanup(server, token)
    finally:
        command = f"rm -rf -- {q(state_dir)}"
        server.run(command, timeout_s=15)


def cleanup_server_run(server: SSHRemote, run: ServerRun) -> None:
    cleanup_server_token(server, run.token)


def build_client_command(
    args, mode: str, token: str, seed: int, direction: str
) -> str:
    if not re.fullmatch(r"stream_[A-Za-z0-9_]+", token):
        raise ValueError("invalid session token")
    argv = [
        args.client_executable,
        args.quic_host,
        str(args.quic_port),
        str(args.total_bytes),
        str(seed),
        str(args.flags),
        str(args.write_timeout_ms),
        str(args.finish_timeout_ms),
        f"--{direction}",
    ]
    for interface in MODE_INTERFACES[mode]:
        argv.extend(["--interface", interface])
    if direction == "post":
        argv.extend(["--scheduler", args.scheduler])
    client_command = " ".join(q(item) for item in argv)
    board_state_dir = f"/tmp/stream_api_bench_{token}_board"
    resource_csv = f"{board_state_dir}/client_resource.csv"
    resource_stop = f"{board_state_dir}/client_resource.stop"
    client_stdout = f"{board_state_dir}/client.stdout.log"
    client_stderr = f"{board_state_dir}/client.stderr.log"
    monitor_stdout = f"{board_state_dir}/monitor.stdout.log"
    monitor_stderr = f"{board_state_dir}/monitor.stderr.log"
    script = (
        "set -uo pipefail\n"
        "export LC_ALL=C\n"
        f"cd -- {q(args.board_dir)} || exit 90\n"
        f"mkdir -p -- {q(board_state_dir)} || exit 91\n"
        f"rm -f -- {q(resource_csv)} {q(resource_stop)}\n"
        "client_rc=0\n"
        f"env STREAM_API_BENCH_SESSION={q(token)} {client_command} "
        f"> {q(client_stdout)} 2> {q(client_stderr)} &\n"
        "client_pid=$!\n"
        f"{q(args.board_monitor_script)} {q(resource_csv)} "
        f"{q(resource_stop)} {q(args.resource_interval_s)} \"$client_pid\" "
        f"> {q(monitor_stdout)} 2> {q(monitor_stderr)} &\n"
        "monitor_pid=$!\n"
        "wait \"$client_pid\" || client_rc=$?\n"
        f"touch -- {q(resource_stop)}\n"
        "monitor_rc=0\n"
        "wait \"$monitor_pid\" || monitor_rc=$?\n"
        f"cat -- {q(client_stdout)} 2>/dev/null || true\n"
        f"cat -- {q(client_stderr)} >&2 2>/dev/null || true\n"
        f"printf '__STREAM_BOARD_RESOURCE_CSV__=%s\\n' {q(resource_csv)}\n"
        "printf '__STREAM_BOARD_MONITOR_RC__=%s\\n' \"$monitor_rc\"\n"
        "printf '__STREAM_CLIENT_RC__=%s\\n' \"$client_rc\"\n"
        "exit \"$client_rc\"\n"
    )
    return (
        f"env STREAM_API_BENCH_SESSION={q(token)} "
        f"bash -c {q(script)}"
    )


def empty_board_resource(status: str, csv_path: str = "") -> Dict[str, object]:
    return {
        "resource_status": status,
        "resource_csv": csv_path,
        "resource_samples": 0,
        "client_process_cpu_avg_pct": None,
        "client_process_cpu_max_pct": None,
        "client_process_rss_avg_kb": None,
        "client_process_rss_max_kb": None,
        "client_process_rss_delta_kb": None,
        "client_process_hwm_max_kb": None,
        "client_process_vmsize_max_kb": None,
    }


def parse_board_resource(path: Path) -> Dict[str, object]:
    required = {
        "client_pid",
        "client_process_cpu_pct",
        "client_rss_kb",
        "client_hwm_kb",
        "client_vmsize_kb",
    }
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            if not reader.fieldnames or not required.issubset(reader.fieldnames):
                return empty_board_resource("PARSE_ERROR", str(path))
            rows = list(reader)
        pids: List[int] = []
        cpu: List[float] = []
        rss: List[float] = []
        hwm: List[float] = []
        vmsize: List[float] = []
        for row in rows:
            pid = int(row["client_pid"])
            values = (
                float(row["client_process_cpu_pct"]),
                float(row["client_rss_kb"]),
                float(row["client_hwm_kb"]),
                float(row["client_vmsize_kb"]),
            )
            if pid <= 0 or not all(math.isfinite(value) for value in values):
                raise ValueError("invalid board resource value")
            if values[0] < 0 or min(values[1:]) < 0:
                raise ValueError("negative board resource value")
            pids.append(pid)
            cpu.append(values[0])
            rss.append(values[1])
            hwm.append(values[2])
            vmsize.append(values[3])
    except (OSError, csv.Error, KeyError, TypeError, ValueError):
        return empty_board_resource("PARSE_ERROR", str(path))
    if not rows:
        return empty_board_resource("EMPTY", str(path))
    if len(set(pids)) != 1:
        return empty_board_resource("PARSE_ERROR", str(path))
    return {
        "resource_status": "OK",
        "resource_csv": str(path),
        "resource_samples": len(rows),
        "client_process_cpu_avg_pct": statistics.mean(cpu),
        "client_process_cpu_max_pct": max(cpu),
        "client_process_rss_avg_kb": statistics.mean(rss),
        "client_process_rss_max_kb": max(rss),
        "client_process_rss_delta_kb": rss[-1] - rss[0],
        "client_process_hwm_max_kb": max(hwm),
        "client_process_vmsize_max_kb": max(vmsize),
    }


def empty_server_resource(status: str, csv_path: str = "") -> Dict[str, object]:
    return {
        "server_memory_status": status,
        "server_memory_aggregation_eligible": False,
        "server_memory_csv": csv_path,
        "server_memory_samples": 0,
        "server_cpu_samples": 0,
        "server_system_cpu_avg_pct": None,
        "server_system_cpu_max_pct": None,
        "server_process_cpu_avg_pct": None,
        "server_process_cpu_max_pct": None,
        "server_system_mem_avg_kb": None,
        "server_system_mem_max_kb": None,
        "server_system_mem_total_kb": None,
        "server_system_mem_delta_kb": None,
        "server_process_rss_avg_kb": None,
        "server_process_rss_max_kb": None,
        "server_process_rss_delta_kb": None,
        "server_process_hwm_max_kb": None,
        "server_process_vmsize_max_kb": None,
    }


SERVER_RESOURCE_FIELDS = [
    "run_tag",
    "mode",
    "direction",
    "round",
    "attempt",
    "monitor_status",
    "elapsed_s",
    "timestamp_utc",
    "system_cpu_pct",
    "system_used_kb",
    "system_used_pct",
    "mem_total_kb",
    "mem_available_kb",
    "cached_kb",
    "buffers_kb",
    "server_pid",
    "server_process_cpu_pct",
    "server_rss_kb",
    "server_hwm_kb",
    "server_vmsize_kb",
]


def initialize_server_memory_csv(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        csv.DictWriter(handle, fieldnames=SERVER_RESOURCE_FIELDS).writeheader()


def parse_and_append_server_resource(
    source: Path,
    combined: Path,
    token: str,
    direction: str,
    mode: str,
    round_index: int,
    attempt: int,
    monitor_status: str,
) -> Dict[str, object]:
    raw_fields = set(SERVER_RESOURCE_FIELDS[6:])
    try:
        with source.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            if not reader.fieldnames or not raw_fields.issubset(reader.fieldnames):
                return empty_server_resource("PARSE_ERROR", str(combined))
            rows = list(reader)
        if not rows:
            return empty_server_resource("EMPTY", str(combined))
        system_cpu: List[float] = []
        process_cpu: List[float] = []
        system_mem: List[float] = []
        system_total: List[float] = []
        process_rss: List[float] = []
        process_hwm: List[float] = []
        process_vmsize: List[float] = []
        pids: List[int] = []
        normalized: List[Dict[str, object]] = []
        for row in rows:
            values = (
                float(row["system_cpu_pct"]),
                float(row["server_process_cpu_pct"]),
                float(row["system_used_kb"]),
                float(row["mem_total_kb"]),
                float(row["server_rss_kb"]),
                float(row["server_hwm_kb"]),
                float(row["server_vmsize_kb"]),
            )
            pid = int(row["server_pid"])
            if not all(math.isfinite(value) for value in values):
                raise ValueError("non-finite server resource value")
            if not 0 <= values[0] <= 100 or values[1] < 0:
                raise ValueError("invalid server CPU value")
            if min(values[2:]) < 0 or values[3] <= 0 or pid <= 0:
                raise ValueError("invalid server memory value")
            system_cpu.append(values[0])
            process_cpu.append(values[1])
            system_mem.append(values[2])
            system_total.append(values[3])
            process_rss.append(values[4])
            process_hwm.append(values[5])
            process_vmsize.append(values[6])
            pids.append(pid)
            normalized.append(
                {
                    **row,
                    "run_tag": token,
                    "mode": DISPLAY_MODE_NAMES[mode],
                    "direction": direction,
                    "round": round_index,
                    "attempt": attempt,
                    "monitor_status": monitor_status,
                }
            )
        if len(set(pids)) != 1:
            raise ValueError("server PID changed")
    except (OSError, csv.Error, KeyError, TypeError, ValueError):
        return empty_server_resource("PARSE_ERROR", str(combined))

    with combined.open("a", encoding="utf-8", newline="") as handle:
        csv.DictWriter(
            handle,
            fieldnames=SERVER_RESOURCE_FIELDS,
            extrasaction="ignore",
        ).writerows(normalized)
    return {
        "server_memory_status": monitor_status,
        "server_memory_aggregation_eligible": monitor_status == "OK",
        "server_memory_csv": str(combined),
        "server_memory_samples": len(rows),
        "server_cpu_samples": len(rows),
        "server_system_cpu_avg_pct": statistics.mean(system_cpu),
        "server_system_cpu_max_pct": max(system_cpu),
        "server_process_cpu_avg_pct": statistics.mean(process_cpu),
        "server_process_cpu_max_pct": max(process_cpu),
        "server_system_mem_avg_kb": statistics.mean(system_mem),
        "server_system_mem_max_kb": max(system_mem),
        "server_system_mem_total_kb": max(system_total),
        "server_system_mem_delta_kb": system_mem[-1] - system_mem[0],
        "server_process_rss_avg_kb": statistics.mean(process_rss),
        "server_process_rss_max_kb": max(process_rss),
        "server_process_rss_delta_kb": process_rss[-1] - process_rss[0],
        "server_process_hwm_max_kb": max(process_hwm),
        "server_process_vmsize_max_kb": max(process_vmsize),
    }


def collect_board_client_logs(
    board: SSHRemote,
    token: str,
    stdout_path: Path,
    stderr_path: Path,
) -> None:
    if not re.fullmatch(r"stream_[A-Za-z0-9_]+", token):
        raise ValueError("invalid session token")
    remote_dir = f"/tmp/stream_api_bench_{token}_board"
    remote_logs = (
        (f"{remote_dir}/client.stdout.log", stdout_path),
        (f"{remote_dir}/client.stderr.log", stderr_path),
    )
    for remote_path, local_path in remote_logs:
        try:
            board.download(remote_path, local_path)
        except Exception as exc:
            LOG.debug("客户端远端日志回收失败 %s: %s", remote_path, exc)


def collect_board_resource(
    board: SSHRemote,
    token: str,
    client_output: str,
    resource_dir: Path,
) -> Dict[str, object]:
    if not re.fullmatch(r"stream_[A-Za-z0-9_]+", token):
        raise ValueError("invalid session token")
    remote_dir = f"/tmp/stream_api_bench_{token}_board"
    remote_csv = f"{remote_dir}/client_resource.csv"
    local_csv = resource_dir / f"{token}_resource.csv"
    monitor_match = re.findall(
        r"__STREAM_BOARD_MONITOR_RC__=(\d+)", client_output
    )
    monitor_status = (
        "OK" if monitor_match and monitor_match[-1] == "0"
        else f"MONITOR_RC_{monitor_match[-1]}" if monitor_match
        else "MISSING_STATUS"
    )
    try:
        board.download(remote_csv, local_csv)
        parsed = parse_board_resource(local_csv)
        if parsed["resource_status"] == "OK" and monitor_status != "OK":
            parsed["resource_status"] = monitor_status
        return parsed
    except Exception as exc:
        LOG.warning("主板资源CSV下载失败: %s", exc)
        return empty_board_resource("DOWNLOAD_ERROR", str(local_csv))
    finally:
        try:
            board.run(f"rm -rf -- {q(remote_dir)}", timeout_s=15)
        except Exception as exc:
            LOG.warning("主板资源临时文件清理失败: %s", exc)


def collect_server_resource(
    server: SSHRemote,
    run: ServerRun,
    resource_dir: Path,
    combined_csv: Path,
    direction: str,
    mode: str,
    round_index: int,
    attempt: int,
    monitor_status: str,
) -> Dict[str, object]:
    local_temp = resource_dir / f".{run.token}_server_resource.csv"
    try:
        server.download(run.resource_csv_path, local_temp)
        return parse_and_append_server_resource(
            local_temp,
            combined_csv,
            run.token,
            direction,
            mode,
            round_index,
            attempt,
            monitor_status,
        )
    except Exception as exc:
        LOG.warning("云服务器资源CSV下载失败: %s", exc)
        return empty_server_resource("DOWNLOAD_ERROR", str(combined_csv))
    finally:
        try:
            local_temp.unlink()
        except FileNotFoundError:
            pass


def attempt_once(
    board: SSHRemote,
    server: SSHRemote,
    args,
    session: str,
    round_index: int,
    direction: str,
    mode: str,
    attempt: int,
    resource_dir: Path,
    raw_client_dir: Path,
    server_memory_csv: Path,
) -> AttemptRecord:
    display_mode = DISPLAY_MODE_NAMES[mode]
    # Within one direction the interface set is the only performance variable.
    seed = args.seed
    start_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")
    token = (
        f"stream_{session}_r{round_index:03d}_{direction}_{mode}_"
        f"a{attempt:02d}_{time.time_ns()}"
    )
    raw_stdout_path = raw_client_dir / f"{token}.stdout.log"
    raw_stderr_path = raw_client_dir / f"{token}.stderr.log"
    run: Optional[ServerRun] = None
    client_result = CommandResult("", "", 125, 0.0)
    parsed: Dict[str, object] = {
        "lanes": 0,
        "payload_elapsed_us": 0,
        "payload_throughput_mbps": 0.0,
    }
    errors: List[str] = []
    board_resource = empty_board_resource("NOT_AVAILABLE")
    server_resource = empty_server_resource("NOT_AVAILABLE")
    server_monitor_status = "MONITOR_STOP_ERROR"

    try:
        run = start_server(server, args, token, seed, direction)
        wait_server_ready(server, run, args.server_ready_timeout_s)
        command = build_client_command(args, mode, token, seed, direction)
        client_result = board.run(
            command,
            timeout_s=args.command_timeout_s,
            output_path=raw_stdout_path,
            stderr_path=raw_stderr_path,
            max_capture_bytes=8 * 1024 * 1024,
        )
        if client_result.timed_out:
            errors.append("controller client timeout")
        expected_scheduler = "adaptive" if direction == "get" else args.scheduler
        client_parsed, client_errors = validate_client_output(
            client_result.output,
            client_result.exit_code,
            args.total_bytes,
            MODE_INTERFACES[mode],
            expected_scheduler,
            direction,
        )
        errors.extend(client_errors)
        # Match the file benchmark with the client direction-specific metric.
        parsed = client_parsed
        if client_result.truncated:
            errors.append("client输出被截断")
    except Exception as exc:
        errors.append(f"{type(exc).__name__}: {exc}")
    finally:
        try:
            session_process_cleanup(board, token)
        except Exception as exc:
            LOG.debug("主板临时进程静默清理失败: %s", exc)
        collect_board_client_logs(
            board, token, raw_stdout_path, raw_stderr_path
        )
        try:
            board_resource = collect_board_resource(
                board, token, client_result.output, resource_dir
            )
        except Exception as exc:
            LOG.warning("主板资源结果处理失败: %s", exc)
        if run is not None:
            try:
                server_monitor_status = stop_server_resource_monitor(
                    server, run
                )
            except Exception as exc:
                LOG.debug("云服务器资源监控静默停止失败: %s", exc)
            try:
                server_resource = collect_server_resource(
                    server,
                    run,
                    resource_dir,
                    server_memory_csv,
                    direction,
                    mode,
                    round_index,
                    attempt,
                    server_monitor_status,
                )
            except Exception as exc:
                LOG.warning("云服务器资源结果处理失败: %s", exc)
        try:
            if run is not None:
                cleanup_server_run(server, run)
            else:
                cleanup_server_token(server, token)
        except Exception as exc:
            LOG.debug("服务端临时进程静默清理失败: %s", exc)

    if client_result.stderr.strip() and client_result.exit_code != 0:
        errors.append("client stderr: " + client_result.stderr.strip()[-1000:])
    return AttemptRecord(
        start_time=start_time,
        end_time=datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f"),
        round=round_index,
        direction=direction,
        mode=mode,
        display_mode=display_mode,
        attempt=attempt,
        success=not errors,
        bytes=args.total_bytes,
        interfaces="+".join(MODE_INTERFACES[mode]),
        scheduler="adaptive" if direction == "get" else args.scheduler,
        payload_elapsed_us=int(parsed.get("payload_elapsed_us", 0)),
        payload_throughput_mbps=float(
            parsed.get("payload_throughput_mbps", 0.0)
        ),
        client_exit=client_result.exit_code,
        error="; ".join(errors),
        board_resource=board_resource,
        server_resource=server_resource,
    )


SPEED_FIELDS = [
    "iter", "filesize", "time_ms", "mbps", "mode", "start_time", "end_time"
]


def write_speed_log(log_root: Path, record: AttemptRecord) -> Path:
    timestamp = datetime.now().strftime("%H%M%S_%f")
    path = log_root / (
        f"{record.display_mode}_summary_{record.direction}_"
        f"iter{record.round:02d}_{timestamp}.log"
    )
    row: Dict[str, object] = {
        "iter": record.round,
        "filesize": record.bytes if record.success else "",
        "time_ms": (
            f"{record.payload_elapsed_us / 1000.0:.3f}" if record.success else ""
        ),
        "mbps": (
            f"{record.payload_throughput_mbps:.3f}" if record.success else ""
        ),
        "mode": record.direction,
        "start_time": record.start_time,
        "end_time": record.end_time,
    }
    average = dict(row)
    average["iter"] = "AVERAGE"
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=SPEED_FIELDS)
        writer.writeheader()
        writer.writerow(row)
        writer.writerow(average)
    return path


def aggregate_board_resources(records: Sequence[AttemptRecord]) -> Dict[str, object]:
    runs = [
        record.board_resource
        for record in records
        if record.board_resource.get("resource_status") == "OK"
        and int(record.board_resource.get("resource_samples") or 0) > 0
    ]
    result: Dict[str, object] = {
        "resource_valid": len(runs),
        "resource_missing": len(records) - len(runs),
        "client_process_cpu_avg_pct": "",
        "client_process_cpu_peak_avg_pct": "",
        "client_process_cpu_max_pct": "",
        "client_process_rss_avg_kb": "",
        "client_process_rss_max_kb": "",
        "client_process_rss_delta_avg_kb": "",
        "client_process_hwm_max_kb": "",
        "client_process_vmsize_max_kb": "",
        "resource_samples": 0,
    }
    if not runs:
        return result
    samples = sum(int(run["resource_samples"]) for run in runs)
    result.update(
        {
            "client_process_cpu_avg_pct": sum(
                float(run["client_process_cpu_avg_pct"])
                * int(run["resource_samples"])
                for run in runs
            ) / samples,
            "client_process_cpu_peak_avg_pct": statistics.mean(
                float(run["client_process_cpu_max_pct"]) for run in runs
            ),
            "client_process_cpu_max_pct": max(
                float(run["client_process_cpu_max_pct"]) for run in runs
            ),
            "client_process_rss_avg_kb": sum(
                float(run["client_process_rss_avg_kb"])
                * int(run["resource_samples"])
                for run in runs
            ) / samples,
            "client_process_rss_max_kb": max(
                float(run["client_process_rss_max_kb"]) for run in runs
            ),
            "client_process_rss_delta_avg_kb": statistics.mean(
                float(run["client_process_rss_delta_kb"]) for run in runs
            ),
            "client_process_hwm_max_kb": max(
                float(run["client_process_hwm_max_kb"]) for run in runs
            ),
            "client_process_vmsize_max_kb": max(
                float(run["client_process_vmsize_max_kb"]) for run in runs
            ),
            "resource_samples": samples,
        }
    )
    return result


def aggregate_server_resources(records: Sequence[AttemptRecord]) -> Dict[str, object]:
    runs = [
        record.server_resource
        for record in records
        if record.server_resource.get("server_memory_aggregation_eligible") is True
        and int(record.server_resource.get("server_memory_samples") or 0) > 0
    ]
    result: Dict[str, object] = {
        "server_memory_valid": len(runs),
        "server_memory_missing": len(records) - len(runs),
        "server_system_cpu_avg_pct": "",
        "server_system_cpu_peak_avg_pct": "",
        "server_system_cpu_max_pct": "",
        "server_process_cpu_avg_pct": "",
        "server_process_cpu_peak_avg_pct": "",
        "server_process_cpu_max_pct": "",
        "server_system_mem_avg_kb": "",
        "server_system_mem_max_kb": "",
        "server_system_mem_total_kb": "",
        "server_system_mem_delta_avg_kb": "",
        "server_process_rss_avg_kb": "",
        "server_process_rss_max_kb": "",
        "server_process_rss_delta_avg_kb": "",
        "server_process_hwm_max_kb": "",
        "server_process_vmsize_max_kb": "",
        "server_memory_samples": 0,
        "server_cpu_samples": 0,
    }
    if not runs:
        return result
    samples = sum(int(run["server_memory_samples"]) for run in runs)
    cpu_samples = sum(int(run["server_cpu_samples"]) for run in runs)
    result.update(
        {
            "server_system_cpu_avg_pct": sum(
                float(run["server_system_cpu_avg_pct"])
                * int(run["server_cpu_samples"])
                for run in runs
            ) / cpu_samples,
            "server_system_cpu_peak_avg_pct": statistics.mean(
                float(run["server_system_cpu_max_pct"]) for run in runs
            ),
            "server_system_cpu_max_pct": max(
                float(run["server_system_cpu_max_pct"]) for run in runs
            ),
            "server_process_cpu_avg_pct": sum(
                float(run["server_process_cpu_avg_pct"])
                * int(run["server_cpu_samples"])
                for run in runs
            ) / cpu_samples,
            "server_process_cpu_peak_avg_pct": statistics.mean(
                float(run["server_process_cpu_max_pct"]) for run in runs
            ),
            "server_process_cpu_max_pct": max(
                float(run["server_process_cpu_max_pct"]) for run in runs
            ),
            "server_system_mem_avg_kb": sum(
                float(run["server_system_mem_avg_kb"])
                * int(run["server_memory_samples"])
                for run in runs
            ) / samples,
            "server_system_mem_max_kb": max(
                float(run["server_system_mem_max_kb"]) for run in runs
            ),
            "server_system_mem_total_kb": max(
                float(run["server_system_mem_total_kb"]) for run in runs
            ),
            "server_system_mem_delta_avg_kb": statistics.mean(
                float(run["server_system_mem_delta_kb"]) for run in runs
            ),
            "server_process_rss_avg_kb": sum(
                float(run["server_process_rss_avg_kb"])
                * int(run["server_memory_samples"])
                for run in runs
            ) / samples,
            "server_process_rss_max_kb": max(
                float(run["server_process_rss_max_kb"]) for run in runs
            ),
            "server_process_rss_delta_avg_kb": statistics.mean(
                float(run["server_process_rss_delta_kb"]) for run in runs
            ),
            "server_process_hwm_max_kb": max(
                float(run["server_process_hwm_max_kb"]) for run in runs
            ),
            "server_process_vmsize_max_kb": max(
                float(run["server_process_vmsize_max_kb"]) for run in runs
            ),
            "server_memory_samples": samples,
            "server_cpu_samples": cpu_samples,
        }
    )
    return result


def write_stress_summary(
    log_root: Path,
    session: str,
    records: Sequence[AttemptRecord],
    planned_iterations: int,
) -> Path:
    fieldnames = [
        "mode", "direction", "planned", "successful", "retried_successes",
        "failed", "success_rate", "average_filesize", "average_time_ms",
        "average_mbps", "resource_valid", "resource_missing",
        "client_process_cpu_avg_pct", "client_process_cpu_peak_avg_pct",
        "client_process_cpu_max_pct", "client_process_rss_avg_kb",
        "client_process_rss_max_kb", "client_process_rss_delta_avg_kb",
        "client_process_hwm_max_kb", "client_process_vmsize_max_kb",
        "resource_samples", "server_memory_valid", "server_memory_missing",
        "server_system_cpu_avg_pct", "server_system_cpu_peak_avg_pct",
        "server_system_cpu_max_pct", "server_process_cpu_avg_pct",
        "server_process_cpu_peak_avg_pct", "server_process_cpu_max_pct",
        "server_system_mem_avg_kb", "server_system_mem_max_kb",
        "server_system_mem_total_kb", "server_system_mem_delta_avg_kb",
        "server_process_rss_avg_kb", "server_process_rss_max_kb",
        "server_process_rss_delta_avg_kb", "server_process_hwm_max_kb",
        "server_process_vmsize_max_kb", "server_memory_samples",
        "server_cpu_samples",
    ]
    path = log_root / f"stress_summary_{session}.csv"
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for direction in DIRECTION_ORDER:
            for mode in MODE_ORDER:
                mode_records = [
                    record for record in records
                    if record.direction == direction and record.mode == mode
                ]
                successful = [record for record in mode_records if record.success]
                count = len(successful)
                writer.writerow(
                    {
                        "mode": DISPLAY_MODE_NAMES[mode],
                        "direction": direction,
                        "planned": planned_iterations,
                        "successful": count,
                        "retried_successes": sum(
                            1 for record in successful if record.attempt > 1
                        ),
                        "failed": planned_iterations - count,
                        "success_rate": f"{count / planned_iterations:.4f}",
                        "average_filesize": (
                            statistics.mean(record.bytes for record in successful)
                            if successful else ""
                        ),
                        "average_time_ms": (
                            statistics.mean(
                                record.payload_elapsed_us / 1000.0
                                for record in successful
                            ) if successful else ""
                        ),
                        "average_mbps": (
                            statistics.mean(
                                record.payload_throughput_mbps
                                for record in successful
                            ) if successful else ""
                        ),
                        **aggregate_board_resources(successful),
                        **aggregate_server_resources(successful),
                    }
                )
    return path


def write_aggregation_summary(
    log_root: Path,
    session: str,
    records: Sequence[AttemptRecord],
    planned_iterations: int,
    direction: str,
) -> Tuple[Path, List[Dict[str, object]]]:
    fieldnames = [
        "round", "5G1_mbps", "5G2_mbps", "Agg_mbps",
        "best_single_mbps", "sum_single_mbps", "gain_vs_best_single_pct",
        "aggregation_efficiency_pct", "status", "missing_modes",
    ]
    path = log_root / f"aggregation_summary_{direction}_{session}.csv"
    successful = {
        (record.round, record.mode): record
        for record in records
        if record.direction == direction and record.success
    }
    rows: List[Dict[str, object]] = []
    complete: List[Dict[str, object]] = []
    for round_index in range(1, planned_iterations + 1):
        samples = {
            mode: successful.get((round_index, mode))
            for mode in ("rmnet", "usb", "agg")
        }
        missing = [
            DISPLAY_MODE_NAMES[mode]
            for mode, record in samples.items() if record is None
        ]
        row: Dict[str, object] = {
            "round": round_index,
            "5G1_mbps": (
                f"{samples['rmnet'].payload_throughput_mbps:.3f}"
                if samples["rmnet"] else ""
            ),
            "5G2_mbps": (
                f"{samples['usb'].payload_throughput_mbps:.3f}"
                if samples["usb"] else ""
            ),
            "Agg_mbps": (
                f"{samples['agg'].payload_throughput_mbps:.3f}"
                if samples["agg"] else ""
            ),
            "best_single_mbps": "",
            "sum_single_mbps": "",
            "gain_vs_best_single_pct": "",
            "aggregation_efficiency_pct": "",
            "status": "INCOMPLETE" if missing else "COMPLETE",
            "missing_modes": ";".join(missing),
        }
        if not missing:
            speed_5g1 = samples["rmnet"].payload_throughput_mbps
            speed_5g2 = samples["usb"].payload_throughput_mbps
            speed_agg = samples["agg"].payload_throughput_mbps
            best = max(speed_5g1, speed_5g2)
            total = speed_5g1 + speed_5g2
            row.update(
                {
                    "best_single_mbps": f"{best:.3f}",
                    "sum_single_mbps": f"{total:.3f}",
                    "gain_vs_best_single_pct": (
                        f"{(speed_agg / best - 1.0) * 100.0:.3f}" if best else ""
                    ),
                    "aggregation_efficiency_pct": (
                        f"{speed_agg / total * 100.0:.3f}" if total else ""
                    ),
                }
            )
            complete.append(row)
        rows.append(row)
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
        if complete:
            average: Dict[str, object] = {
                "round": "AVERAGE",
                "status": "COMPLETE_ROUNDS_ONLY",
                "missing_modes": "",
            }
            for name in fieldnames[1:8]:
                average[name] = f"{statistics.mean(float(row[name]) for row in complete):.3f}"
            writer.writerow(average)
    return path, rows


def parse_args(argv: Optional[Sequence[str]] = None):
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description=(
            "Stream API正式交付压测：每次100 MiB、4-lane，"
            "每轮按GET和POST的5G1/Agg/5G2顺序执行"
        )
    )
    parser.add_argument("--board-ssh-host", default=DEFAULT_BOARD_SSH_HOST)
    parser.add_argument("--board-ssh-port", type=int, default=22)
    parser.add_argument("--board-ssh-user", default="root")
    parser.add_argument(
        "--board-ssh-password",
        default=os.environ.get("MINI_BENCH_BOARD_PASSWORD", ""),
    )
    parser.add_argument("--board-ssh-key", default="")
    parser.add_argument("--server-ssh-host", default=DEFAULT_SERVER_SSH_HOST)
    parser.add_argument("--server-ssh-port", type=int, default=22)
    parser.add_argument("--server-ssh-user", default="root")
    parser.add_argument(
        "--server-ssh-password",
        default=os.environ.get("MINI_BENCH_SERVER_PASSWORD", ""),
    )
    parser.add_argument("--server-ssh-key", default="")
    parser.add_argument("--strict-host-key", action="store_true")

    parser.add_argument("--board-dir", default=DEFAULT_BOARD_DIR)
    parser.add_argument("--server-dir", default=DEFAULT_SERVER_DIR)
    parser.add_argument("--client-executable", default=DEFAULT_CLIENT_EXECUTABLE)
    parser.add_argument("--server-executable", default=DEFAULT_SERVER_EXECUTABLE)
    parser.add_argument("--server-bind", default="0.0.0.0")
    parser.add_argument("--quic-host", default=DEFAULT_QUIC_HOST)
    parser.add_argument("--quic-port", type=int, default=DEFAULT_QUIC_PORT)

    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    parser.add_argument("--retries", type=int, default=DEFAULT_RETRIES)
    parser.add_argument("--cooldown-s", type=float, default=DEFAULT_COOLDOWN_S)
    parser.add_argument(
        "--scheduler",
        choices=SCHEDULERS,
        default=DEFAULT_SCHEDULER,
        help="POST调度器(默认balanced)；GET使用adaptive",
    )
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--flags", type=int, choices=(0, 1), default=0)
    parser.add_argument(
        "--write-timeout-ms", type=int, default=DEFAULT_WRITE_TIMEOUT_MS
    )
    parser.add_argument(
        "--finish-timeout-ms", type=int, default=DEFAULT_FINISH_TIMEOUT_MS
    )
    parser.add_argument(
        "--command-timeout-s", type=float, default=DEFAULT_COMMAND_TIMEOUT_S
    )
    parser.add_argument(
        "--server-ready-timeout-s",
        type=float,
        default=DEFAULT_SERVER_READY_TIMEOUT_S,
    )
    parser.add_argument(
        "--skip-route-check",
        action="store_true",
        help="只检查接口和IPv4地址，不执行指定oif的route get预检",
    )
    parser.add_argument(
        "--board-monitor-script",
        default=DEFAULT_BOARD_MONITOR_SCRIPT,
        help="主板端现有monitor_board_resource.sh路径",
    )
    parser.add_argument(
        "--server-monitor-script",
        default=DEFAULT_SERVER_MONITOR_SCRIPT,
        help="云端现有monitor_server_memory.sh路径",
    )
    parser.add_argument(
        "--resource-interval-s",
        type=float,
        default=DEFAULT_RESOURCE_INTERVAL_S,
    )
    parser.add_argument(
        "--output-root",
        default=str(script_dir / "log"),
    )
    args = parser.parse_args(argv)

    if args.iterations <= 0:
        parser.error("--iterations必须大于0")
    if args.retries < 0:
        parser.error("--retries不能小于0")
    if args.cooldown_s < 0:
        parser.error("--cooldown-s不能小于0")
    if not 1 <= args.quic_port <= 65535:
        parser.error("--quic-port无效")
    if not 1 <= args.board_ssh_port <= 65535:
        parser.error("--board-ssh-port无效")
    if not 1 <= args.server_ssh_port <= 65535:
        parser.error("--server-ssh-port无效")
    if not 0 <= args.seed <= 0xFFFFFFFF:
        parser.error("--seed必须在0到4294967295之间")
    for name in ("write_timeout_ms", "finish_timeout_ms"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')}必须大于0")
    for name in (
        "command_timeout_s",
        "server_ready_timeout_s",
    ):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')}必须大于0")
    if args.resource_interval_s <= 0:
        parser.error("--resource-interval-s必须大于0")

    args.total_bytes = TRANSFER_BYTES
    return args


def print_final_console_summary(
    final_records: Sequence[AttemptRecord],
    aggregation_rows: Dict[str, Sequence[Dict[str, object]]],
) -> None:
    LOG.info("================ Stream API压测结果 ================")
    LOG.info(
        "统计口径: GET首次至末次数据回调；POST首次写入至本地队列flush完成"
    )
    for direction in DIRECTION_ORDER:
        LOG.info("---------------- %s方向 ----------------", direction.upper())
        for mode in ("rmnet", "agg", "usb"):
            speeds = [
                record.payload_throughput_mbps
                for record in final_records
                if record.direction == direction
                and record.mode == mode
                and record.success
            ]
            if speeds:
                LOG.info(
                    "%s %s: success=%s avg=%.3f median=%.3f "
                    "min=%.3f max=%.3f Mbps",
                    direction.upper(),
                    DISPLAY_MODE_NAMES[mode],
                    len(speeds),
                    statistics.mean(speeds),
                    statistics.median(speeds),
                    min(speeds),
                    max(speeds),
                )
            else:
                LOG.info(
                    "%s %s: 没有成功样本",
                    direction.upper(),
                    DISPLAY_MODE_NAMES[mode],
                )
        complete = [
            row for row in aggregation_rows[direction]
            if row["status"] == "COMPLETE"
        ]
        if complete:
            LOG.info(
                "%s聚合增益(相对最佳单网卡): 平均 %.3f%%",
                direction.upper(),
                statistics.mean(
                    float(row["gain_vs_best_single_pct"]) for row in complete
                ),
            )
            LOG.info(
                "%s聚合效率(Agg/(5G1+5G2)): 平均 %.3f%%",
                direction.upper(),
                statistics.mean(
                    float(row["aggregation_efficiency_pct"])
                    for row in complete
                ),
            )


def main(argv: Optional[Sequence[str]] = None) -> int:
    configure_logging()
    args = parse_args(argv)
    session = datetime.now().strftime("%Y%m%d_%H%M%S") + "_" + uuid.uuid4().hex[:8]
    output_root = Path(args.output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    resource_dir = output_root / "resources" / session
    raw_client_dir = output_root / "raw_client" / session
    resource_dir.mkdir(parents=True, exist_ok=True)
    raw_client_dir.mkdir(parents=True, exist_ok=True)
    server_memory_csv = output_root / f"server_memory_{session}.csv"
    initialize_server_memory_csv(server_memory_csv)

    board = SSHRemote(
        args.board_ssh_host,
        args.board_ssh_user,
        args.board_ssh_password,
        args.board_ssh_key,
        args.board_ssh_port,
        args.strict_host_key,
        "板卡",
    )
    server = SSHRemote(
        args.server_ssh_host,
        args.server_ssh_user,
        args.server_ssh_password,
        args.server_ssh_key,
        args.server_ssh_port,
        args.strict_host_key,
        "云端",
    )
    final_records: List[AttemptRecord] = []
    interrupted = False
    fatal_error = ""

    LOG.info(
        "测试配置: size=%s MiB bytes=%s iterations=%s order=%s "
        "directions=get,post get_scheduler=adaptive post_scheduler=%s lanes=4",
        TRANSFER_SIZE_MIB,
        args.total_bytes,
        args.iterations,
        ",".join(MODE_ORDER),
        args.scheduler,
    )
    try:
        connect_with_optional_prompt(server, "云端")
        connect_with_optional_prompt(board, "板卡")
        server_preflight(server, args)
        board_preflight(board, args)

        for round_index in range(1, args.iterations + 1):
            LOG.info("========== 轮次 %s/%s ==========", round_index, args.iterations)
            for direction in DIRECTION_ORDER:
                for mode in MODE_ORDER:
                    successful: Optional[AttemptRecord] = None
                    last_record: Optional[AttemptRecord] = None
                    for attempt in range(1, args.retries + 2):
                        LOG.info(
                            "开始: round=%s direction=%s mode=%s "
                            "interfaces=%s attempt=%s/%s",
                            round_index,
                            direction.upper(),
                            DISPLAY_MODE_NAMES[mode],
                            ",".join(MODE_INTERFACES[mode]),
                            attempt,
                            args.retries + 1,
                        )
                        record = attempt_once(
                            board,
                            server,
                            args,
                            session,
                            round_index,
                            direction,
                            mode,
                            attempt,
                            resource_dir,
                            raw_client_dir,
                            server_memory_csv,
                        )
                        last_record = record
                        if record.success:
                            successful = record
                            LOG.info(
                                "成功: direction=%s mode=%s payload_mbps=%.3f "
                                "payload_elapsed_ms=%.3f",
                                record.direction.upper(),
                                record.display_mode,
                                record.payload_throughput_mbps,
                                record.payload_elapsed_us / 1000.0,
                            )
                            break
                        LOG.error(
                            "失败: direction=%s mode=%s attempt=%s reason=%s",
                            record.direction.upper(),
                            record.display_mode,
                            attempt,
                            record.error or "unknown",
                        )
                        if attempt <= args.retries and args.cooldown_s > 0:
                            time.sleep(args.cooldown_s)
                    final_record = successful or last_record
                    if final_record is not None:
                        final_records.append(final_record)
                        speed_path = write_speed_log(output_root, final_record)
                        LOG.info("速率日志: %s", speed_path)
                    if args.cooldown_s > 0:
                        time.sleep(args.cooldown_s)

    except KeyboardInterrupt:
        interrupted = True
        LOG.warning("用户中断")
    except Exception as exc:
        fatal_error = f"{type(exc).__name__}: {exc}"
        LOG.exception("压测终止: %s", fatal_error)
    finally:
        try:
            board.close()
        except Exception:
            pass
        try:
            server.close()
        except Exception:
            pass

    stress_path = write_stress_summary(
        output_root, session, final_records, args.iterations
    )
    aggregation_paths: Dict[str, Path] = {}
    aggregation_rows: Dict[str, Sequence[Dict[str, object]]] = {}
    for direction in DIRECTION_ORDER:
        path, rows = write_aggregation_summary(
            output_root,
            session,
            final_records,
            args.iterations,
            direction,
        )
        aggregation_paths[direction] = path
        aggregation_rows[direction] = rows
    print_final_console_summary(final_records, aggregation_rows)
    LOG.info("GET聚合率汇总: %s", aggregation_paths["get"])
    LOG.info("POST聚合率汇总: %s", aggregation_paths["post"])
    LOG.info("最终资源汇总: %s", stress_path)
    LOG.info("云服务器资源汇总: %s", server_memory_csv)
    LOG.info("主板资源目录: %s", resource_dir)
    LOG.info("客户端原始日志目录: %s", raw_client_dir)

    if interrupted:
        return 130
    if fatal_error:
        return 2
    expected = args.iterations * len(MODE_ORDER) * len(DIRECTION_ORDER)
    all_successful = (
        len(final_records) == expected
        and all(record is not None and record.success for record in final_records)
    )
    return 0 if all_successful else 1


if __name__ == "__main__":
    sys.exit(main())
