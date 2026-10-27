#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Run Mini API GET and POST benchmarks from a Windows/Linux controller.

The cloud server is always started from /root/test_xquic_server by default.
The board client and cloud server are controlled through SSH.

Active GX-style outputs (each algorithm has its own directory):
  log/<月日_时分秒_算法名称组合>/（同名重复实验追加_02、_03等后缀）
    experiment_config.json / test_plan.csv / case_results.csv
    attempts.jsonl / results.jsonl / server_starts.jsonl
    algorithm_summary.csv
    <scheduler>/
    5G1_summary_get_iterXX_*.log / 5G1_summary_post_iterXX_*.log
    5G2_summary_get_iterXX_*.log / 5G2_summary_post_iterXX_*.log
    Agg_summary_get_iterXX_*.log / Agg_summary_post_iterXX_*.log
    resources/<session>/*_resource.csv
    raw_client/<session>/*.stdout.log / *.stderr.log
    server_memory_<session>.csv
    stress_summary_<session>.csv
    aggregation_summary_get_<session>.csv
    aggregation_summary_post_<session>.csv

Warmup logs live under <scheduler>/warmup/ and are excluded from summaries.
算法按完整周期轮流执行：adaptive 一轮 → minrtt 一轮 → adaptive 下一轮。
每轮仍按原配置执行 GET/POST 和三种链路，算法分别保存结果。
GET 轮换云端发送算法，板卡接收端固定 adaptive；POST 轮换板卡发送算法，
云端接收端固定 adaptive。服务端需要先部署支持 --scheduler 的配套版本。
仅在所需服务端算法变化时重启，核对启动输出中的实际算法后才开始传输。
The controller persists each client's stdout/stderr separately for audit.
Server startup output is kept in the existing remote session directory until cleanup.
"""

from __future__ import annotations

import argparse
import csv
import getpass
import json
import logging
import math
import os
import posixpath
import re
import shlex
import statistics
import sys
import time
import uuid
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

try:
    import paramiko
except ImportError:  # Deferred so --help still works without dependencies.
    paramiko = None

LOG = logging.getLogger("mini_api_get_post_benchmark")


# =============================================================================
# 用户常用配置区
# =============================================================================
# 直接在这里修改即可。命令行参数仍然有效，并且会覆盖这里的默认值。
# 例如：python 副本改进版.py --iterations 10

# 主板和云服务器地址
DEFAULT_BOARD_SSH_HOST = "192.168.11.1"
DEFAULT_SERVER_SSH_HOST = "124.70.219.194"
DEFAULT_QUIC_HOST = "124.70.219.194"
DEFAULT_QUIC_PORT = 18446

# POST 上传源文件（主板上的绝对路径）
DEFAULT_POST_LOCAL = "/oemdata/gx/xquic_file/download/kkk.txt"

# 超时参数
DEFAULT_CLIENT_TIMEOUT_MS = 600000
DEFAULT_COMMAND_TIMEOUT_S = 690.0

# SSH密码从环境变量读取，也可填写下方的代码配置。


# 多算法实验配置：直接修改这里，然后在编辑器里运行本文件，无须命令行参数。
# 留空 ()：兼容原脚本，GET/POST分别使用DEFAULT_GET/POST_SCHEDULER。
# 可选算法见SUPPORTED_SCHEDULERS；算法名称不得重复。
DEFAULT_SCHEDULERS = ("adaptive", "minrtt")
DEFAULT_EXECUTION_MODE = "round-robin"  # round-robin=周期轮换；sequential=整批执行
DEFAULT_SEQUENCE = "fixed"  # 固定按算法列表轮流执行：A→B→A→B
DEFAULT_DRY_RUN = False  # True=只保存计划，不连接SSH；False=实际测试
DEFAULT_WARMUP_CYCLES = 0  # 每个算法的预热周期数；预热数据不进入正式汇总
DEFAULT_ALGORITHM_COOLDOWN_S = 2.0  # 算法周期之间的额外冷却时间
DEFAULT_CC = "bbr"  # 多算法实验统一传给客户端的拥塞控制参数

# 连接、路径和输出配置（密码也可直接填字符串；不写入实验配置快照）。
DEFAULT_BOARD_SSH_PORT = 22
DEFAULT_BOARD_SSH_USER = "root"
DEFAULT_BOARD_SSH_PASSWORD = os.environ.get("MINI_BENCH_BOARD_PASSWORD", "")
DEFAULT_BOARD_SSH_KEY = ""
DEFAULT_SERVER_SSH_PORT = 22
DEFAULT_SERVER_SSH_USER = "root"
DEFAULT_SERVER_SSH_PASSWORD = os.environ.get("MINI_BENCH_SERVER_PASSWORD", "")
DEFAULT_SERVER_SSH_KEY = ""
DEFAULT_STRICT_HOST_KEY = False
DEFAULT_SERVER_DIR = "/root/test_xquic_server"
DEFAULT_SERVER_EXECUTABLE = "./netins_udp_mini_api_server"
DEFAULT_SERVER_BIND = "0.0.0.0"
DEFAULT_SERVER_NAME = ""  # 留空则使用DEFAULT_QUIC_HOST
DEFAULT_ACCEPT_TIMEOUT_MS = 0
DEFAULT_SERVE_TIMEOUT_MS = 600000
DEFAULT_SERVER_READY_TIMEOUT_S = 15.0
DEFAULT_BOARD_DIR = "/oemdata/gx/xquic_file"
DEFAULT_CLIENT_EXECUTABLE = "./netins_udp_mini_api_client"
DEFAULT_REMOTE_PATH = "/upload/1"
DEFAULT_POST_REMOTE_RMNET = "/upload/post-rmnet-kkk.txt"
DEFAULT_POST_REMOTE_USB = "/upload/post-usb-kkk.txt"
DEFAULT_POST_REMOTE_AGG = "/upload/post-agg-kkk.txt"
DEFAULT_BOARD_RESOURCE_DIR = "/oemdata/test_result/gx_resource_monitor"
DEFAULT_SERVER_RESOURCE_DIR = "/tmp/gx_server_memory"
DEFAULT_BOARD_RESOURCE_RETENTION_DAYS = 0
DEFAULT_SERVER_RESOURCE_RETENTION_DAYS = 0
DEFAULT_OUTPUT_ROOT = ""  # 留空：本文件同目录下的log/<月日_时分秒_算法名称组合>/

# 压测参数
DEFAULT_ITERATIONS = 1  # 每个算法各跑多少个正式完整周期；正式对比建议覆盖均衡周期
DEFAULT_RUN_GET = 1
DEFAULT_RUN_POST = 1
DEFAULT_GET_SCHEDULER = "adaptive"
DEFAULT_GET_CC = "bbr"
DEFAULT_POST_SCHEDULER = "minrtt"
DEFAULT_POST_CC = "bbr"
DEFAULT_MODE_ORDER = "rmnet,agg,usb,"
DEFAULT_RETRIES = 1
DEFAULT_COOLDOWN_S = 2.0
DEFAULT_VERIFY_SHA256 = False

# 主板 API Demo 进程资源监控
DEFAULT_BOARD_RESOURCE_MONITOR = "/oemdata/monitor_board_resource.sh"
DEFAULT_BOARD_RESOURCE_INTERVAL_S = 0.5

# 云服务器资源监控
DEFAULT_SERVER_RESOURCE_MONITOR = (
    posixpath.join(DEFAULT_SERVER_DIR, "monitor_server_memory.sh")
)
DEFAULT_SERVER_RESOURCE_INTERVAL_S = 0.5

# =============================================================================
# 用户常用配置区结束
# =============================================================================
MODE_INTERFACES = {
    "rmnet": ("rmnet_data0",),
    "usb": ("usb0",),
    "agg": ("rmnet_data0", "usb0"),
}
TRANSFER_RECORD_RE = re.compile(r"transfer\s+iteration=", re.IGNORECASE)
TRANSFER_FIELD_RE = re.compile(
    r"\b(?P<key>direction|bytes|fragments|elapsed_us|throughput_mbps|metric)="
    r"(?P<value>[^\s\r\n]+)",
    re.IGNORECASE,
)
REQUIRED_TRANSFER_FIELDS = (
    "bytes",
    "fragments",
    "elapsed_us",
    "throughput_mbps",
)
EXPECTED_B_METRIC = {
    "get": "first_body_to_last_body",
    "post": "app_submit_not_remote_receive",
}


def extract_transfer_records(stdout: str) -> List[Dict[str, str]]:
    """Extract unordered fields from each client transfer-summary line.

    A record is identified by the client-only ``transfer iteration=`` marker.
    Fields may appear in any order after the marker.  Final success requires
    bytes, fragments, elapsed_us and throughput_mbps; direction and metric
    remain mandatory provenance guards.
    """
    records: List[Dict[str, str]] = []
    for line in stdout.splitlines():
        markers = list(TRANSFER_RECORD_RE.finditer(line))
        if not markers:
            continue
        for index, marker in enumerate(markers):
            record_end = (
                markers[index + 1].start()
                if index + 1 < len(markers)
                else len(line)
            )
            record_text = line[marker.start():record_end]
            fields = {
                match.group("key").lower(): match.group("value")
                for match in TRANSFER_FIELD_RE.finditer(record_text)
            }
            records.append(fields)
    return records


def transfer_numeric_fields_are_valid(record: Dict[str, str]) -> bool:
    return bool(
        re.fullmatch(r"\d+", record.get("bytes", ""))
        and re.fullmatch(r"\d+", record.get("elapsed_us", ""))
        and re.fullmatch(
            r"\d+(?:\.\d+)?", record.get("throughput_mbps", "")
        )
    )
GET_FINAL_RE = re.compile(
    r"(?:\[stats\]\s*)?all download streams finished:\s*"
    r"(?P<bytes>\d+)\s*bytes,\s*"
    r"time=(?P<time_ms>\d+(?:\.\d+)?)\s*ms,\s*"
    r"speed=(?P<mbps>\d+(?:\.\d+)?)\s*Mbps",
    re.IGNORECASE,
)
POST_FINAL_RE = re.compile(
    r"(?:\[stats\]\s*)?all streams sent:\s*"
    r"(?P<bytes>\d+)\s*bytes,\s*"
    r"time=(?P<time_ms>\d+(?:\.\d+)?)\s*ms,\s*"
    r"speed=(?P<mbps>\d+(?:\.\d+)?)\s*Mbps",
    re.IGNORECASE,
)
WALL_RE = re.compile(r"__MINI_WALL_SECONDS__=(\d+(?:\.\d+)?)")
CLIENT_RC_RE = re.compile(r"__MINI_CLIENT_RC__=(\d+)")
LOCAL_SIZE_RE = re.compile(r"__MINI_LOCAL_SIZE__=(\d+)")
LOCAL_SHA_RE = re.compile(r"__MINI_LOCAL_SHA256__=([0-9a-fA-F]{64})")
REMOTE_SIZE_RE = re.compile(r"__MINI_REMOTE_SIZE__=(\d+)")
REMOTE_SHA_RE = re.compile(r"__MINI_REMOTE_SHA256__=([0-9a-fA-F]{64})")
SERVER_PID_RE = re.compile(r"__MINI_SERVER_PID__=(\d+)")
RESOURCE_STATUS_RE = re.compile(r"__MINI_RESOURCE_STATUS__=([A-Z_]+)")


@dataclass
class CommandResult:
    # output is stdout only.  Speed/protocol markers must never be parsed from
    # stderr, where API/zlog diagnostics are written.
    output: str
    exit_code: int
    controller_elapsed_s: float
    stderr: str = ""
    timed_out: bool = False
    truncated: bool = False

    @property
    def diagnostic_output(self) -> str:
        if not self.stderr:
            return self.output
        if not self.output:
            return self.stderr
        return self.output + "\n" + self.stderr


class SSHRemote:
    """Small Paramiko runner with separate output and a hard wall-clock limit."""

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
        fingerprint = ":".join(f"{value:02x}" for value in key.get_fingerprint())
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
        max_capture_bytes: int = 64 * 1024 * 1024,
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
        sink = None
        stderr_sink = None
        stable_after_exit = None
        try:
            sink = output_path.open("wb") if output_path is not None else None
            stderr_sink = (
                stderr_path.open("wb") if stderr_path is not None else None
            )
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
                    if made_progress:
                        stable_after_exit = time.monotonic()
                    elif stable_after_exit is None:
                        stable_after_exit = time.monotonic()
                    elif time.monotonic() - stable_after_exit >= 0.25:
                        exit_code = channel.recv_exit_status()
                        break

                if time.monotonic() >= deadline:
                    channel.close()
                    elapsed = time.monotonic() - started
                    return CommandResult(
                        output=captured.decode("utf-8", errors="replace"),
                        exit_code=124,
                        controller_elapsed_s=elapsed,
                        stderr=stderr_captured.decode("utf-8", errors="replace"),
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

        elapsed = time.monotonic() - started
        return CommandResult(
            output=captured.decode("utf-8", errors="replace"),
            exit_code=exit_code,
            controller_elapsed_s=elapsed,
            stderr=stderr_captured.decode("utf-8", errors="replace"),
            truncated=truncated or stderr_truncated,
        )

    def download(
        self,
        remote_path: str,
        local_path: Path,
        socket_timeout_s: float = 120,
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

    def close(self) -> None:
        client = self.client
        self.client = None
        if client is not None:
            try:
                client.close()
            except Exception:
                pass




def q(value: str) -> str:
    return shlex.quote(str(value))






def connect_ssh_with_optional_prompt(remote: SSHRemote, password_label: str) -> None:
    try:
        remote.connect()
        return
    except Exception as first_error:
        if remote.password or remote.key_filename or not sys.stdin.isatty():
            raise
        LOG.warning("%s首次SSH认证失败: %s", remote.label, first_error)
    remote.password = getpass.getpass(f"{password_label} SSH密码: ")
    remote.connect()


def validate_order(value: str) -> List[str]:
    modes = [item.strip().lower() for item in value.split(",") if item.strip()]
    if not modes or any(mode not in MODE_INTERFACES for mode in modes):
        raise argparse.ArgumentTypeError(
            "order只能包含 rmnet,usb,agg，例如 rmnet,usb,agg"
        )
    if len(set(modes)) != len(modes):
        raise argparse.ArgumentTypeError("order中不能重复模式")
    if set(modes) != set(MODE_INTERFACES):
        raise argparse.ArgumentTypeError("order必须恰好包含 rmnet、usb、agg")
    return modes


def validate_remote_path(path: str) -> str:
    pieces = [piece for piece in path.split("/") if piece]
    normalized = posixpath.normpath(path)
    if (
        not path.startswith("/")
        or normalized == "/"
        or normalized != path
        or any(piece in (".", "..") for piece in pieces)
    ):
        raise argparse.ArgumentTypeError(
            "remote-path必须是规范化的绝对文件URL路径，且不能是根目录"
        )
    return path


def server_preflight(
    server: SSHRemote,
    server_dir: str,
    server_executable: str,
    remote_path: str,
    require_get_source: bool = True,
) -> Tuple[str, int]:
    source_path = posixpath.join(server_dir, remote_path.lstrip("/"))
    source_check = ""
    if require_get_source:
        source_check = (
            f" && test -r {q(source_path)}"
            f" && printf '__MINI_SOURCE_SIZE__=%s\\n' "
            f"\"$(wc -c < {q(source_path)})\""
        )
    command = (
        f"cd -- {q(server_dir)}"
        f" && test -x {q(server_executable)}"
        " && test -r server.crt"
        " && test -r server.key"
        " && command -v nohup >/dev/null"
        " && command -v env >/dev/null"
        " && command -v awk >/dev/null"
        " && command -v tr >/dev/null"
        " && command -v grep >/dev/null"
        " && command -v wc >/dev/null"
        " && command -v ss >/dev/null"
        + source_check
    )
    result = server.run(command, timeout_s=20)
    if result.exit_code != 0:
        raise RuntimeError(f"云端服务端预检失败:\n{result.diagnostic_output}")
    if not require_get_source:
        return source_path, 0
    size_match = re.search(r"__MINI_SOURCE_SIZE__=(\d+)", result.output)
    if not size_match:
        raise RuntimeError("云端服务端预检未返回源文件大小")
    source_size = int(size_match.group(1))
    if source_size <= 0:
        raise RuntimeError("云端GET源文件为空，拒绝进行无效吞吐测试")
    LOG.info("云端GET源文件: %s bytes=%s", source_path, source_size)
    return source_path, source_size


def board_preflight(
    board,
    args,
    source_size: int,
    session: str,
    verify_sha256: bool,
    require_post_source: bool = True,
) -> None:
    write_probe = posixpath.join(args.board_dir, f".mini_bench_{session}")
    checks = [
        f"cd -- {q(args.board_dir)}",
        f"test -x {q(args.client_executable)}",
        f"test -w {q(args.board_dir)}",
        f": > {q(write_probe)}",
        f"rm -f -- {q(write_probe)}",
        "command -v env >/dev/null",
        "command -v tr >/dev/null",
        "command -v grep >/dev/null",
        "command -v awk >/dev/null",
        "command -v df >/dev/null",
        "command -v ip >/dev/null",
        "command -v wc >/dev/null",
        "test -r /proc/uptime",
    ]
    if require_post_source:
        checks.insert(3, f"test -r {q(args.post_local)}")
    for interface in ("rmnet_data0", "usb0"):
        variable = interface.replace("-", "_")
        checks.append(
            f"ip -o link show dev {q(interface)} "
            "| grep -Eq '<([^,>]+,)*UP(,|>)'"
        )
        checks.append(
            f"src_{variable}=$(ip -o -4 addr show dev {q(interface)} "
            "| awk '{print $4; exit}')"
        )
        checks.append(f"test -n \"$src_{variable}\"")
        checks.append(
            f"ip -4 route get {q(args.quic_host)} "
            f"from \"${{src_{variable}%/*}}\" "
            f"oif {q(interface)} >/dev/null"
        )
    if verify_sha256:
        checks.append("command -v sha256sum >/dev/null")
    if isinstance(board, SSHRemote) and not args.disable_board_resource_monitor:
        checks.extend(
            [
                f"test -x {q(args.board_resource_monitor)}",
                "command -v bash >/dev/null",
                "command -v sleep >/dev/null",
                "command -v date >/dev/null",
                "command -v find >/dev/null",
                "command -v getconf >/dev/null",
                f"mkdir -p {q(args.board_resource_dir)}",
                f"test -w {q(args.board_resource_dir)}",
            ]
        )
    # Refuse to benchmark while an older marked API client/monitor is still
    # running. Killing it automatically could interrupt another controller.
    checks.append(
        "conflict_pids=''; "
        "for env_file in /proc/[0-9]*/environ; do "
        "[ -r \"$env_file\" ] || continue; "
        "if tr '\\000' '\\n' < \"$env_file\" 2>/dev/null "
        "| grep -Eq '^MINI_API_BENCH_(CLIENT|MONITOR)_SESSION=api_'; then "
        "pid=${env_file#/proc/}; pid=${pid%/environ}; conflict_pids=\"$conflict_pids $pid\"; "
        "fi; done; "
        "if [ -n \"$conflict_pids\" ]; then "
        "printf 'existing benchmark process(es):%s\\n' \"$conflict_pids\" >&2; exit 8; fi"
    )
    checks.append(
        f"df -Pk {q(args.board_dir)} | "
        "awk 'NR==2 {printf \"__MINI_FREE_BYTES__=%.0f\\n\", $4 * 1024}'"
    )
    result = board.run(" && ".join(checks), timeout_s=30)
    if result.exit_code != 0:
        raise RuntimeError(f"板卡预检失败:\n{result.diagnostic_output}")
    free_match = re.search(r"__MINI_FREE_BYTES__=(\d+)", result.output)
    if not free_match:
        raise RuntimeError("板卡预检未能读取工作目录剩余空间")
    free_bytes = int(free_match.group(1))
    planned_attempts = getattr(args, "planned_get_attempts", len(args.order) * args.iterations * (args.retries + 1))
    retained_copies = planned_attempts if args.keep_downloads else 1
    safety_margin = max(16 * 1024 * 1024, source_size // 10)
    required_bytes = source_size * retained_copies + safety_margin
    if free_bytes < required_bytes:
        raise RuntimeError(
            "板卡空间不足: "
            f"free={free_bytes} required={required_bytes} "
            f"keep_downloads={args.keep_downloads}"
        )
    LOG.info(
        "板卡客户端、双接口路由、资源监控和写入空间预检通过: "
        "free=%s required=%s",
        free_bytes,
        required_bytes,
    )



def cleanup_stale_board_resource_files(board, args) -> None:
    """Clean abandoned benchmark artifacts from earlier sessions.

    Current-session artifacts are removed immediately after local collection and
    again in the final session cleanup. This startup pass only handles leftovers
    from older interrupted runs.
    """
    if not isinstance(board, SSHRemote) or args.disable_board_resource_monitor:
        return
    days = int(args.board_resource_retention_days)
    remote_dir = q(args.board_resource_dir)
    board_dir = q(args.board_dir)
    command = (
        f"mkdir -p {remote_dir} && "
        f"find {remote_dir} -maxdepth 1 -type f "
        "\\( -name 'api_*_resource.csv' -o "
        "-name 'api_*_resource_monitor.log' -o "
        "-name 'mini_*_resource.csv' -o "
        "-name 'mini_*_resource_monitor.log' \\) "
        f"-mtime +{days} -delete; "
        "find /tmp -maxdepth 1 -type f "
        "\\( -name 'api_*_resource.stop' -o "
        "-name 'api_*_resource_monitor.log' -o "
        "-name 'api_*_client.start' -o "
        "-name 'mini_*_resource.stop' -o "
        "-name 'mini_*_resource_monitor.log' \\) "
        f"-mtime +{days} -delete; "
        f"find {board_dir} -maxdepth 1 -type f "
        "\\( -name 'get-rmnet-*.bin' -o -name 'get-usb-*.bin' "
        "-o -name 'get-agg-*.bin' \\) "
        f"-mtime +{days} -delete; "
        f"rm -f -- {q(posixpath.join(args.board_dir, 'clog.log'))}"
    )
    result = board.run(command, timeout_s=30)
    if result.exit_code != 0 or result.timed_out:
        LOG.warning(
            "清理主板历史资源文件未确认: exit=%s output=%s",
            result.exit_code,
            result.diagnostic_output.strip(),
        )
    else:
        LOG.info(
            "主板历史压测文件启动清理完成: retention_days=%s dir=%s",
            days,
            args.board_resource_dir,
        )

def get_server_sha256(server: SSHRemote, source_path: str) -> str:
    result = server.run(
        f"sha256sum {q(source_path)} | awk '{{print $1}}'", timeout_s=120
    )
    match = re.search(r"\b([0-9a-fA-F]{64})\b", result.output)
    if result.exit_code != 0 or not match:
        raise RuntimeError(f"无法计算云端源文件SHA-256:\n{result.diagnostic_output}")
    return match.group(1).lower()







def get_board_post_source_info(
    board: SSHRemote,
    post_local_path: str,
    verify_sha256: bool,
) -> Tuple[int, str]:
    sha_part = (
        f"printf '__MINI_POST_SOURCE_SHA256__=%s\\n' "
        f"\"$(sha256sum {q(post_local_path)} | awk '{{print $1}}')\"; "
        if verify_sha256
        else ""
    )
    command = (
        f"test -r {q(post_local_path)} && "
        f"printf '__MINI_POST_SOURCE_SIZE__=%s\\n' \"$(wc -c < {q(post_local_path)})\""
        + (" && " + sha_part.rstrip("; ") if sha_part else "")
    )
    result = board.run(command, timeout_s=120)
    size_match = re.search(r"__MINI_POST_SOURCE_SIZE__=(\d+)", result.output)
    sha_match = re.search(
        r"__MINI_POST_SOURCE_SHA256__=([0-9a-fA-F]{64})",
        result.output,
    )
    if result.exit_code != 0 or not size_match:
        raise RuntimeError(
            f"无法读取主板POST源文件: {post_local_path}: "
            f"{result.diagnostic_output.strip()}"
        )
    size = int(size_match.group(1))
    if size <= 0:
        raise RuntimeError(f"主板POST源文件为空: {post_local_path}")
    sha = sha_match.group(1).lower() if sha_match else ""
    if verify_sha256 and not sha:
        raise RuntimeError("无法计算主板POST源文件SHA-256")
    LOG.info("主板POST源文件: %s bytes=%s", post_local_path, size)
    return size, sha


def post_remote_path_for_mode(args, mode: str) -> str:
    return {
        "rmnet": args.post_remote_rmnet,
        "usb": args.post_remote_usb,
        "agg": args.post_remote_agg,
    }[mode]


def server_path_for_remote(server_dir: str, remote_path: str) -> str:
    return posixpath.join(server_dir, remote_path.lstrip("/"))


def validate_server_post_targets(server: SSHRemote, args) -> None:
    remote_paths = [
        args.post_remote_rmnet,
        args.post_remote_usb,
        args.post_remote_agg,
    ]
    target_paths = [server_path_for_remote(args.server_dir, item) for item in remote_paths]
    parent_paths = sorted({posixpath.dirname(item) for item in target_paths})
    command = " && ".join(
        [f"test -d {q(parent)} && test -w {q(parent)}" for parent in parent_paths]
        + [f"rm -f -- {q(target)} && test ! -e {q(target)}" for target in target_paths]
    )
    result = server.run(command, timeout_s=30)
    if result.exit_code != 0 or result.timed_out:
        raise RuntimeError(
            "云服务器POST目标目录不可写或旧上传文件无法清理: "
            + result.diagnostic_output.strip()
        )


def prepare_server_post_target(server: SSHRemote, args, remote_path: str) -> bool:
    target = server_path_for_remote(args.server_dir, remote_path)
    return cleanup_remote_files_verified(server, [target], "云服务器POST目标")


def inspect_and_cleanup_server_post(
    server: SSHRemote,
    args,
    remote_path: str,
    verify_sha256: bool,
) -> Tuple[str, bool]:
    target = server_path_for_remote(args.server_dir, remote_path)
    sha_command = (
        f"remote_sha=$(sha256sum {q(target)} | awk '{{print $1}}'); "
        "printf '__MINI_REMOTE_SHA256__=%s\\n' \"$remote_sha\"; "
        if verify_sha256
        else ""
    )
    command = (
        f"if [ ! -f {q(target)} ]; then "
        "printf '__MINI_REMOTE_FILE_MISSING__\\n'; exit 4; fi; "
        f"remote_size=$(wc -c < {q(target)}); "
        "printf '__MINI_REMOTE_SIZE__=%s\\n' \"$remote_size\"; "
        + sha_command
        + f"rm -f -- {q(target)}; test ! -e {q(target)}"
    )
    try:
        result = server.run(command, timeout_s=max(120.0, args.command_timeout_s))
    except Exception as exc:
        cleanup_ok = cleanup_remote_files_verified(
            server,
            [target],
            "云服务器POST目标",
        )
        return f"server POST inspection exception: {type(exc).__name__}: {exc}", cleanup_ok
    cleanup_ok = result.exit_code == 0 and not result.timed_out
    if not cleanup_ok:
        cleanup_ok = cleanup_remote_files_verified(
            server,
            [target],
            "云服务器POST目标",
        )
    error = ""
    if result.exit_code != 0 or result.timed_out:
        error = "server POST file verify/cleanup failed"
    return result.output + ("\n" + error if error else ""), cleanup_ok


def stop_server(
    server: SSHRemote,
    pid: Optional[int],
    executable: str,
    remote_pidfile: str,
    session_token: str,
) -> bool:
    """Stop only the server process proven to belong to this session."""
    if not pid or not server.is_connected():
        return False
    executable_name = posixpath.basename(executable)
    token_line = "MINI_API_BENCH_SESSION=" + session_token
    ownership_check = (
        f"cmdline=$(tr '\\000' ' ' < /proc/{pid}/cmdline 2>/dev/null || true); "
        f"current_start=$(awk '{{print $22}}' /proc/{pid}/stat 2>/dev/null || true); "
        f"read tracked_pid tracked_start < {q(remote_pidfile)} 2>/dev/null || true; "
        f"token_ok=0; tr '\\000' '\\n' < /proc/{pid}/environ 2>/dev/null "
        f"| grep -Fqx {q(token_line)} && token_ok=1; "
        f"case \"$cmdline\" in *{q(executable_name)}*) name_ok=1 ;; *) name_ok=0 ;; esac; "
        f"[ \"$tracked_pid\" = {pid} ] && [ -n \"$tracked_start\" ] && "
        "[ \"$current_start\" = \"$tracked_start\" ] && "
        "[ \"$token_ok\" -eq 1 ] && [ \"$name_ok\" -eq 1 ]"
    )
    command = (
        f"if kill -0 {pid} 2>/dev/null; then "
        f"if {ownership_check}; then "
        f"kill -TERM {pid} 2>/dev/null || true; "
        f"i=0; while kill -0 {pid} 2>/dev/null && [ \"$i\" -lt 25 ]; "
        "do sleep 0.2; i=$((i+1)); done; "
        f"if kill -0 {pid} 2>/dev/null; then "
        f"if {ownership_check}; then kill -KILL {pid} 2>/dev/null || true; sleep 0.2; "
        "else printf '__MINI_PID_REUSED__\\n'; exit 4; fi; fi; "
        f"if kill -0 {pid} 2>/dev/null; then printf '__MINI_STOP_FAILED__\\n'; exit 5; fi; "
        f"rm -f -- {q(remote_pidfile)}; printf '__MINI_SERVER_STOPPED__\\n'; "
        "else printf '__MINI_PID_NOT_OWNED__=%s\\n' \"$cmdline\"; exit 3; fi; "
        f"else rm -f -- {q(remote_pidfile)}; printf '__MINI_SERVER_ALREADY_EXITED__\\n'; fi"
    )
    try:
        result = server.run(command, timeout_s=15)
    except Exception as exc:
        LOG.warning("清理云端服务端PID %s失败: %s", pid, exc)
        return False
    if result.exit_code == 0 and not result.timed_out:
        return True
    if "__MINI_PID_NOT_OWNED__" in result.diagnostic_output:
        LOG.warning("拒绝清理PID %s：进程身份与本次会话不匹配", pid)
    else:
        LOG.warning(
            "云端服务端PID %s清理未确认: exit=%s timed_out=%s output=%s",
            pid,
            result.exit_code,
            result.timed_out,
            result.diagnostic_output.strip(),
        )
    return False



def empty_resource_result(
    status: str,
    resource_csv: str = "",
) -> Dict[str, object]:
    return {
        "resource_status": status,
        "resource_csv": resource_csv,
        "client_process_cpu_avg_pct": None,
        "client_process_cpu_max_pct": None,
        "client_process_rss_avg_kb": None,
        "client_process_rss_max_kb": None,
        "client_process_rss_delta_kb": None,
        "client_process_hwm_max_kb": None,
        "client_process_vmsize_max_kb": None,
        "resource_samples": 0,
    }


def parse_resource_csv(path: Path) -> Dict[str, object]:
    required_columns = {
        "client_pid",
        "client_process_cpu_pct",
        "client_rss_kb",
        "client_hwm_kb",
        "client_vmsize_kb",
    }
    try:
        with path.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            if not reader.fieldnames or not required_columns.issubset(
                set(reader.fieldnames)
            ):
                return empty_resource_result("PARSE_ERROR", str(path))

            client_pids = []
            cpu_values: List[float] = []
            rss_values: List[float] = []
            hwm_values: List[float] = []
            vmsize_values: List[float] = []
            for row in reader:
                client_pid = int((row.get("client_pid") or "").strip())
                cpu_pct = float(
                    (row.get("client_process_cpu_pct") or "").strip()
                )
                rss_kb = float((row.get("client_rss_kb") or "").strip())
                hwm_kb = float((row.get("client_hwm_kb") or "").strip())
                vmsize_kb = float(
                    (row.get("client_vmsize_kb") or "").strip()
                )
                values = (cpu_pct, rss_kb, hwm_kb, vmsize_kb)
                if (
                    client_pid <= 0
                    or not all(math.isfinite(value) for value in values)
                    or cpu_pct < 0.0
                    or min(rss_kb, hwm_kb, vmsize_kb) < 0.0
                ):
                    return empty_resource_result("PARSE_ERROR", str(path))
                client_pids.append(client_pid)
                cpu_values.append(cpu_pct)
                rss_values.append(rss_kb)
                hwm_values.append(hwm_kb)
                vmsize_values.append(vmsize_kb)
    except (OSError, ValueError, TypeError, csv.Error):
        return empty_resource_result("PARSE_ERROR", str(path))

    if not cpu_values:
        return empty_resource_result("EMPTY", str(path))
    if len(set(client_pids)) != 1:
        return empty_resource_result("PARSE_ERROR", str(path))

    return {
        "resource_status": "OK",
        "resource_csv": str(path),
        "client_process_cpu_avg_pct": statistics.mean(cpu_values),
        "client_process_cpu_max_pct": max(cpu_values),
        "client_process_rss_avg_kb": statistics.mean(rss_values),
        "client_process_rss_max_kb": max(rss_values),
        "client_process_rss_delta_kb": rss_values[-1] - rss_values[0],
        "client_process_hwm_max_kb": max(hwm_values),
        "client_process_vmsize_max_kb": max(vmsize_values),
        "resource_samples": len(cpu_values),
    }


def extract_resource_status(output: str, enabled: bool) -> str:
    if not enabled:
        return "DISABLED"
    matches = RESOURCE_STATUS_RE.findall(output)
    return matches[-1] if matches else "MISSING_STATUS"


def cleanup_remote_files_verified(
    remote: SSHRemote,
    paths: Sequence[str],
    label: str,
    retries: int = 3,
) -> bool:
    """Delete exact remote files and verify every path is absent."""
    existing = [str(path) for path in paths if path]
    if not existing:
        return True
    quoted_paths = " ".join(q(path) for path in existing)
    verify_parts = " && ".join(f"test ! -e {q(path)}" for path in existing)
    command = f"rm -f -- {quoted_paths} && {verify_parts}"
    total_attempts = max(1, int(retries))
    for attempt in range(1, total_attempts + 1):
        try:
            result = remote.run(command, timeout_s=20)
        except Exception as exc:
            LOG.warning(
                "%s远端文件清理第%s/%s次异常: %s",
                label,
                attempt,
                total_attempts,
                exc,
            )
        else:
            if result.exit_code == 0 and not result.timed_out:
                return True
            LOG.warning(
                "%s远端文件清理第%s/%s次未确认: exit=%s output=%s",
                label,
                attempt,
                total_attempts,
                result.exit_code,
                result.diagnostic_output.strip(),
            )
        if attempt < total_attempts:
            time.sleep(0.5)
    return False


def download_remote_with_retry(
    remote: SSHRemote,
    remote_path: str,
    local_path: Path,
    label: str,
    retries: int = 3,
) -> Tuple[bool, Optional[Exception]]:
    """Download with reconnect/retry; never delete the remote source here."""
    last_error: Optional[Exception] = None
    total_attempts = max(1, int(retries))
    for attempt in range(1, total_attempts + 1):
        try:
            if not remote.is_connected():
                remote.connect()
            remote.download(remote_path, local_path)
            return True, None
        except Exception as exc:
            last_error = exc
            LOG.warning(
                "%s下载第%s/%s次失败: %s: %s",
                label,
                attempt,
                total_attempts,
                remote_path,
                exc,
            )
            if attempt < total_attempts:
                try:
                    remote.close()
                    remote.connect()
                except Exception:
                    pass
                time.sleep(0.5)
    return False, last_error



def cleanup_remote_resource_files(
    board: SSHRemote,
    paths: Sequence[str],
) -> bool:
    return cleanup_remote_files_verified(board, paths, "主板")



def transfer_algorithms_for_direction(args, direction: str) -> Tuple[str, str]:
    if direction == "get":
        return (
            args.scheduler or DEFAULT_GET_SCHEDULER,
            args.cc or DEFAULT_GET_CC,
        )
    return (
        args.scheduler or DEFAULT_POST_SCHEDULER,
        args.cc or DEFAULT_POST_CC,
    )


def build_client_command(
    args,
    mode: str,
    direction: str,
    board_path: str,
    remote_path: str,
    attempt_token: str,
    resource_enabled: bool,
    remote_resource_file: str = "",
    remote_stop_file: str = "",
    remote_monitor_log: str = "",
    *,
    scheduler: Optional[str] = None,
    cc: Optional[str] = None,
) -> str:
    if direction not in {"get", "post"}:
        raise ValueError(f"unsupported direction: {direction}")
    default_scheduler, default_cc = transfer_algorithms_for_direction(args, direction)
    scheduler = scheduler or default_scheduler
    cc = cc or default_cc

    argv = [
        args.client_executable,
        "--get" if direction == "get" else "--post",
        "--host",
        args.quic_host,
        "--port",
        str(args.quic_port),
        "--server-name",
        args.server_name,
        "--local",
        board_path,
        "--remote",
        remote_path,
    ]
    for interface in MODE_INTERFACES[mode]:
        argv.extend(["--interface", interface])
    argv.extend(
        [
            "--scheduler",
            scheduler,
            "--cc",
            cc,
            "--timeout-ms",
            str(args.client_timeout_ms),
        ]
    )
    client_command = " ".join(q(item) for item in argv)
    gate_file = f"/tmp/{attempt_token}_client.start"

    resource_setup = "monitor_state=DISABLED; monitor_pid=\"\"; "
    if resource_enabled:
        resource_setup = (
            f"resource_file={q(remote_resource_file)}; "
            f"stop_file={q(remote_stop_file)}; "
            f"monitor_log={q(remote_monitor_log)}; "
            "monitor_state=OK; monitor_pid=\"\"; "
            "rm -f -- \"$resource_file\" \"$stop_file\" \"$monitor_log\"; "
        )

    start_monitor = ""
    if resource_enabled:
        start_monitor = (
            f"MINI_API_BENCH_MONITOR_SESSION={q(attempt_token)} "
            f"{q(args.board_resource_monitor)} "
            "\"$resource_file\" \"$stop_file\" "
            f"{q(str(args.board_resource_interval_s))} \"$client_pid\" "
            "> \"$monitor_log\" 2>&1 & "
            "monitor_pid=$!; sleep 0.05; "
            "if ! kill -0 \"$monitor_pid\" 2>/dev/null; then "
            "monitor_state=START_FAILED; fi; "
        )

    if direction == "get":
        prepare_path = f"rm -f -- {q(board_path)} clog.log \"$gate_file\""
    else:
        prepare_path = (
            f"rm -f -- clog.log \"$gate_file\"; "
            f"if [ ! -r {q(board_path)} ]; then client_rc=92; fi"
        )

    script = (
        "set -uo pipefail\n"
        "export LC_ALL=C\n"
        "client_pid=\"\"\n"
        "client_rc=125\n"
        + resource_setup
        + f"gate_file={q(gate_file)}\n"
        + "cleanup_client() {\n"
        + "  rm -f -- \"$gate_file\"\n"
        + "  if [ -n \"${client_pid:-}\" ] && kill -0 \"$client_pid\" 2>/dev/null; then\n"
        + "    kill -TERM \"$client_pid\" 2>/dev/null || true\n"
        + "    sleep 0.2\n"
        + "    kill -KILL \"$client_pid\" 2>/dev/null || true\n"
        + "    wait \"$client_pid\" 2>/dev/null || true\n"
        + "  fi\n"
        + "  client_pid=\"\"\n"
        + "}\n"
        + "stop_resource_monitor() {\n"
        + "  monitor_rc=0\n"
        + "  if [ -n \"${monitor_pid:-}\" ]; then\n"
        + "    touch \"${stop_file:-/tmp/nonexistent}\" 2>/dev/null || true\n"
        + "    wait \"$monitor_pid\" 2>/dev/null || monitor_rc=$?\n"
        + "    monitor_pid=\"\"\n"
        + "  fi\n"
        + "  if [ -n \"${stop_file:-}\" ]; then rm -f -- \"$stop_file\"; fi\n"
        + "  if [ \"$monitor_rc\" -ne 0 ] && [ \"$monitor_state\" = OK ]; then\n"
        + "    monitor_state=RUNTIME_FAILED\n"
        + "  fi\n"
        + "}\n"
        + "cleanup_all() { cleanup_client; stop_resource_monitor; }\n"
        + "trap cleanup_all EXIT\n"
        + "trap 'cleanup_all; exit 129' HUP\n"
        + "trap 'cleanup_all; exit 130' INT\n"
        + "trap 'cleanup_all; exit 143' TERM\n"
        + f"cd -- {q(args.board_dir)} || client_rc=90\n"
        + "if [ \"$client_rc\" -ne 90 ]; then\n"
        + "  " + prepare_path + "\n"
        + "  if [ \"$client_rc\" -ne 92 ]; then\n"
        + "    (\n"
        + "      while [ ! -e \"$gate_file\" ]; do sleep 0.01; done\n"
        + f"      exec env MINI_API_BENCH_CLIENT_SESSION={q(attempt_token)} {client_command}\n"
        + "    ) &\n"
        + "    client_pid=$!\n"
        + "    " + start_monitor + "\n"
        + "    wall_start=$(awk '{print $1}' /proc/uptime)\n"
        + "    if ! touch \"$gate_file\"; then\n"
        + "      client_rc=91\n"
        + "      cleanup_client\n"
        + "    else\n"
        + "      client_rc=0\n"
        + "      wait \"$client_pid\" || client_rc=$?\n"
        + "      client_pid=\"\"\n"
        + "    fi\n"
        + "    wall_end=$(awk '{print $1}' /proc/uptime)\n"
        + "    wall_elapsed=$(awk -v start=\"$wall_start\" -v end=\"$wall_end\" 'BEGIN { printf \"%.3f\", end - start }')\n"
        + "    printf '__MINI_WALL_SECONDS__=%s\\n' \"$wall_elapsed\"\n"
        + "  fi\n"
        + f"  if [ -f {q(board_path)} ]; then\n"
        + f"    local_size=$(wc -c < {q(board_path)})\n"
        + "    printf '__MINI_LOCAL_SIZE__=%s\\n' \"$local_size\"\n"
        + "  fi\n"
        + "  rm -f -- clog.log\n"
        + "fi\n"
        + "stop_resource_monitor\n"
        + "trap - EXIT HUP INT TERM\n"
        + "rm -f -- \"$gate_file\"\n"
        + "printf '__MINI_RESOURCE_STATUS__=%s\\n' \"$monitor_state\"\n"
        + "printf '__MINI_CLIENT_RC__=%s\\n' \"$client_rc\"\n"
        + "[ \"$client_rc\" -eq 0 ]\n"
    )
    return f"bash -c {q(script)}"


def cleanup_board_attempt(
    board,
    attempt_token: str,
    board_dir: str,
    board_output: str,
    keep_download: bool,
    resource_stop_file: str = "",
) -> bool:
    """Stop processes owned by one attempt without repeatedly scanning /proc.

    The previous implementation rescanned every /proc/*/environ entry as many
    as 23 times. On the embedded board this could exceed the 15-second SSH
    timeout even after the API client had already finished. We now perform one
    ownership scan, then wait on that fixed PID set directly.
    """
    client_token_line = "MINI_API_BENCH_CLIENT_SESSION=" + attempt_token
    monitor_token_line = "MINI_API_BENCH_MONITOR_SESSION=" + attempt_token
    collect_pids = (
        "collect_mini_pids() { "
        "for env_file in /proc/[0-9]*/environ; do "
        "[ -r \"$env_file\" ] || continue; "
        f"if tr '\\000' '\\n' < \"$env_file\" 2>/dev/null "
        f"| grep -Eq {q('^(' + re.escape(client_token_line) + '|' + re.escape(monitor_token_line) + ')$')}; then "
        "pid=${env_file#/proc/}; pid=${pid%/environ}; "
        "printf '%s ' \"$pid\"; fi; done; }; "
    )
    # A PID is considered owned only while it still carries this attempt's
    # environment marker. Zombies are treated as already terminated.
    owned_pid_function = (
        "owned_mini_pid() { pid=$1; "
        "[ -r \"/proc/$pid/stat\" ] || return 1; "
        "state=$(awk '{print $3}' \"/proc/$pid/stat\" 2>/dev/null || true); "
        "[ \"$state\" != Z ] || return 1; "
        "[ -r \"/proc/$pid/environ\" ] || return 1; "
        f"tr '\\000' '\\n' < \"/proc/$pid/environ\" 2>/dev/null "
        f"| grep -Eq {q('^(' + re.escape(client_token_line) + '|' + re.escape(monitor_token_line) + ')$')}; "
        "}; "
    )
    remove_output = (
        ""
        if keep_download
        else (
            f"if ! cd -- {q(board_dir)} "
            f"|| ! rm -f -- {q(board_output)}; then "
            "printf '__MINI_OUTPUT_CLEANUP_FAILED__\\n'; exit 6; fi; "
        )
    )
    stop_monitor = (
        f"touch {q(resource_stop_file)} 2>/dev/null || true; "
        if resource_stop_file
        else ""
    )
    remove_stop = (
        f"rm -f -- {q(resource_stop_file)}; "
        if resource_stop_file
        else ""
    )
    command = (
        "set -uo pipefail; export LC_ALL=C; "
        + stop_monitor
        + collect_pids
        + owned_pid_function
        + "pids=$(collect_mini_pids); "
        + "for pid in $pids; do kill -TERM \"$pid\" 2>/dev/null || true; done; "
        + "i=0; while [ \"$i\" -lt 30 ]; do "
        + "remaining=''; for pid in $pids; do "
        + "owned_mini_pid \"$pid\" && remaining=\"$remaining $pid\" || true; done; "
        + "[ -n \"$remaining\" ] || break; sleep 0.1; i=$((i+1)); done; "
        + "for pid in $remaining; do "
        + "owned_mini_pid \"$pid\" && kill -KILL \"$pid\" 2>/dev/null || true; done; "
        + "i=0; while [ \"$i\" -lt 20 ]; do "
        + "still_alive=''; for pid in $remaining; do "
        + "owned_mini_pid \"$pid\" && still_alive=\"$still_alive $pid\" || true; done; "
        + "[ -n \"$still_alive\" ] || break; sleep 0.1; i=$((i+1)); done; "
        + remove_output
        + remove_stop
        + "if [ -n \"${still_alive:-}\" ]; then "
        + "printf '__MINI_CLIENT_CLEANUP_FAILED__=%s\\n' \"$still_alive\"; exit 5; fi; "
        + "printf '__MINI_CLIENT_CLEANUP_OK__\\n'"
    )
    result = board.run(command, timeout_s=30)
    if (
        result.exit_code != 0
        or result.timed_out
        or "__MINI_CLIENT_CLEANUP_OK__" not in result.output
    ):
        LOG.warning(
            "板卡测试进程/文件清理未确认: exit=%s timed_out=%s output=%s",
            result.exit_code,
            result.timed_out,
            result.diagnostic_output.strip(),
        )
        return False
    return True


def parse_client_result(
    command_result: CommandResult,
    direction: str,
    expected_size: int,
    expected_sha: str,
    verify_sha256: bool,
) -> Tuple[bool, Dict[str, object], str]:
    direction = direction.lower()

    # Only accept the current B metric emitted by the API Demo.  Do not fall
    # back to legacy aggregate lines, because doing so could silently mix A and
    # B timing definitions across benchmark runs.
    expected_metric = EXPECTED_B_METRIC[direction]
    transfer_records = extract_transfer_records(command_result.output)
    direction_transfers = [
        record
        for record in transfer_records
        if record.get("direction", "").lower() == direction
    ]
    b_transfers = [
        record
        for record in direction_transfers
        if record.get("metric", "").lower() == expected_metric
    ]
    complete_b_transfers = [
        record for record in b_transfers if transfer_numeric_fields_are_valid(record)
    ]
    transfer = complete_b_transfers[-1] if complete_b_transfers else None

    wall_matches = WALL_RE.findall(command_result.output)
    rc_matches = CLIENT_RC_RE.findall(command_result.output)
    local_size_matches = LOCAL_SIZE_RE.findall(command_result.output)
    local_sha_matches = LOCAL_SHA_RE.findall(command_result.output)
    remote_size_matches = REMOTE_SIZE_RE.findall(command_result.output)
    remote_sha_matches = REMOTE_SHA_RE.findall(command_result.output)

    exit_code = int(rc_matches[-1]) if rc_matches else command_result.exit_code
    if transfer is not None:
        total_bytes = int(transfer["bytes"])
        elapsed_us = int(transfer["elapsed_us"])
        transfer_elapsed_s = elapsed_us / 1_000_000.0
        throughput_mbps = float(transfer["throughput_mbps"])
        fragments_completed = 0
        fragments_planned = 0
        fragments = re.fullmatch(r"(\d+)/(\d+)", transfer.get("fragments", ""))
        if fragments is not None:
            fragments_completed = int(fragments.group(1))
            fragments_planned = int(fragments.group(2))
        summary_source = "transfer_iteration_b"
    else:
        total_bytes = 0
        fragments_completed = 0
        fragments_planned = 0
        transfer_elapsed_s = 0.0
        throughput_mbps = 0.0
        summary_source = "missing"

    parsed = {
        "exit_code": exit_code,
        "total_bytes": total_bytes,
        "fragments_completed": fragments_completed,
        "fragments_planned": fragments_planned,
        "transfer_elapsed_s": transfer_elapsed_s,
        "throughput_mbps": throughput_mbps,
        "metric": expected_metric if transfer is not None else "",
        "summary_source": summary_source,
        "remote_wall_elapsed_s": float(wall_matches[-1]) if wall_matches else 0.0,
        "wall_throughput_mbps": 0.0,
        "local_size": int(local_size_matches[-1]) if local_size_matches else 0,
        "local_sha256": local_sha_matches[-1].lower() if local_sha_matches else "",
        "remote_size": int(remote_size_matches[-1]) if remote_size_matches else 0,
        "remote_sha256": remote_sha_matches[-1].lower() if remote_sha_matches else "",
    }
    if parsed["remote_wall_elapsed_s"] > 0 and total_bytes > 0:
        parsed["wall_throughput_mbps"] = (
            total_bytes * 8.0 / parsed["remote_wall_elapsed_s"] / 1_000_000.0
        )

    errors: List[str] = []
    if command_result.timed_out:
        errors.append("controller timeout")
    if exit_code != 0:
        errors.append(f"client exit={exit_code}")
    if summary_source == "missing":
        if b_transfers:
            missing_or_invalid = sorted(
                {
                    field
                    for record in b_transfers
                    for field in REQUIRED_TRANSFER_FIELDS
                    if not record.get(field)
                }
            )
            detail = ",".join(missing_or_invalid) or "numeric value"
            errors.append(
                f"incomplete {direction.upper()} B transfer summary: "
                f"missing/invalid {detail}"
            )
        elif direction_transfers:
            actual_metrics = sorted(
                {
                    record.get("metric", "<missing>")
                    for record in direction_transfers
                }
            )
            errors.append(
                f"unexpected {direction.upper()} metric {actual_metrics}; "
                f"expected B metric {expected_metric}"
            )
        else:
            errors.append(
                f"missing {direction.upper()} B transfer summary "
                f"metric={expected_metric}"
            )
    elif (
        total_bytes <= 0
        or transfer_elapsed_s <= 0
        or throughput_mbps <= 0
    ):
        errors.append("invalid zero-valued transfer summary")
    else:
        calculated_mbps = total_bytes * 8.0 / (transfer_elapsed_s * 1_000_000.0)
        tolerance_mbps = max(0.01, calculated_mbps * 0.0001)
        if abs(calculated_mbps - throughput_mbps) > tolerance_mbps:
            errors.append(
                "transfer throughput formula mismatch: "
                f"reported={throughput_mbps:.6f} "
                f"calculated={calculated_mbps:.6f}"
            )

    if transfer is not None:
        if fragments_completed <= 0 or fragments_planned <= 0:
            errors.append("missing or invalid fragments")
        elif fragments_completed != fragments_planned:
            errors.append(
                "incomplete fragments "
                f"{fragments_completed}/{fragments_planned}"
            )

    if not wall_matches:
        errors.append("missing wall-clock timing")
    elif parsed["remote_wall_elapsed_s"] <= 0:
        errors.append("invalid wall-clock timing")

    if not local_size_matches:
        errors.append("missing local file size")
    if summary_source != "missing" and parsed["local_size"] != total_bytes:
        errors.append(
            f"local size {parsed['local_size']} != bytes {total_bytes}"
        )
    if summary_source != "missing" and total_bytes != expected_size:
        errors.append(
            f"{direction} bytes {total_bytes} != source {expected_size}"
        )

    if direction == "get":
        if verify_sha256:
            if not parsed["local_sha256"]:
                errors.append("missing local sha256")
            elif parsed["local_sha256"] != expected_sha:
                errors.append("GET sha256 mismatch")
    else:
        if not remote_size_matches:
            errors.append("missing uploaded remote file size")
        elif parsed["remote_size"] != expected_size:
            errors.append(
                f"uploaded remote size {parsed['remote_size']} != source {expected_size}"
            )
        if verify_sha256:
            if not parsed["remote_sha256"]:
                errors.append("missing uploaded remote sha256")
            elif parsed["remote_sha256"] != expected_sha:
                errors.append("POST sha256 mismatch")

    if "transfer failed" in command_result.diagnostic_output.lower():
        errors.append("client reported transfer failed")
    return not errors, parsed, "; ".join(errors)


# ---------------------------------------------------------------------------
# GX-style API Demo benchmark entry point
# ---------------------------------------------------------------------------


DISPLAY_MODE_NAMES = {
    "rmnet": "5G1",
    "usb": "5G2",
    "agg": "Agg",
}
SPEED_FIELDS = [
    "iter",
    "filesize",
    "time_ms",
    "mbps",
    "mode",
    "start_time",
    "end_time",
]
SERVER_MONITOR_PID_RE = re.compile(r"__API_SERVER_MONITOR_PID__=(\d+)")
SERVER_MONITOR_START_RE = re.compile(r"__API_SERVER_MONITOR_START__=(\d+)")


def configure_console_logging() -> None:
    formatter = logging.Formatter(
        "%(asctime)s %(levelname)s %(message)s",
        "%Y-%m-%d %H:%M:%S",
    )
    LOG.setLevel(logging.INFO)
    LOG.handlers.clear()
    handler = logging.StreamHandler(sys.stdout)
    handler.setFormatter(formatter)
    LOG.addHandler(handler)


def parse_gx_args(argv: Optional[Sequence[str]] = None):
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description=(
            "通过SSH控制板卡和云端服务端，使用Mini API Demo执行"
            "5G1/5G2/聚合POST与GET，并按旧GX压测格式保存速率和资源数据"
        )
    )

    parser.add_argument("--board-ssh-host", default=DEFAULT_BOARD_SSH_HOST)
    parser.add_argument("--board-ssh-port", type=int, default=DEFAULT_BOARD_SSH_PORT)
    parser.add_argument("--board-ssh-user", default=DEFAULT_BOARD_SSH_USER)
    parser.add_argument(
        "--board-ssh-password",
        default=DEFAULT_BOARD_SSH_PASSWORD,
    )
    parser.add_argument("--board-ssh-key", default=DEFAULT_BOARD_SSH_KEY)

    parser.add_argument("--server-ssh-host", default=DEFAULT_SERVER_SSH_HOST)
    parser.add_argument("--server-ssh-port", type=int, default=DEFAULT_SERVER_SSH_PORT)
    parser.add_argument("--server-ssh-user", default=DEFAULT_SERVER_SSH_USER)
    parser.add_argument(
        "--server-ssh-password",
        default=DEFAULT_SERVER_SSH_PASSWORD,
    )
    parser.add_argument("--server-ssh-key", default=DEFAULT_SERVER_SSH_KEY)
    parser.add_argument("--strict-host-key", action="store_true")

    parser.add_argument("--server-dir", default=DEFAULT_SERVER_DIR)
    parser.add_argument(
        "--server-executable",
        default=DEFAULT_SERVER_EXECUTABLE,
    )
    parser.add_argument("--server-bind", default=DEFAULT_SERVER_BIND)
    parser.add_argument("--quic-host", default=DEFAULT_QUIC_HOST)
    parser.add_argument("--quic-port", type=int, default=DEFAULT_QUIC_PORT)
    parser.add_argument("--server-name", default=DEFAULT_SERVER_NAME)
    parser.add_argument("--accept-timeout-ms", type=int, default=DEFAULT_ACCEPT_TIMEOUT_MS)
    parser.add_argument("--serve-timeout-ms", type=int, default=DEFAULT_SERVE_TIMEOUT_MS)
    parser.add_argument("--server-ready-timeout-s", type=float, default=DEFAULT_SERVER_READY_TIMEOUT_S)

    parser.add_argument("--board-dir", default=DEFAULT_BOARD_DIR)
    parser.add_argument(
        "--client-executable",
        default=DEFAULT_CLIENT_EXECUTABLE,
    )
    parser.add_argument(
        "--remote-path",
        type=validate_remote_path,
        default=DEFAULT_REMOTE_PATH,
    )
    parser.add_argument(
        "--post-local",
        default=DEFAULT_POST_LOCAL,
        help="主板上的POST源文件绝对路径",
    )
    parser.add_argument(
        "--post-remote-rmnet",
        type=validate_remote_path,
        default=DEFAULT_POST_REMOTE_RMNET,
    )
    parser.add_argument(
        "--post-remote-usb",
        type=validate_remote_path,
        default=DEFAULT_POST_REMOTE_USB,
    )
    parser.add_argument(
        "--post-remote-agg",
        type=validate_remote_path,
        default=DEFAULT_POST_REMOTE_AGG,
    )
    scheduler_group = parser.add_mutually_exclusive_group()
    scheduler_group.add_argument("--scheduler", default=None)
    scheduler_group.add_argument("--schedulers", type=validate_schedulers, default=None)
    parser.add_argument("--execution-mode", choices=("round-robin", "sequential"), default=DEFAULT_EXECUTION_MODE)
    parser.add_argument("--sequence", choices=("balanced", "fixed"), default=DEFAULT_SEQUENCE)
    parser.add_argument("--dry-run", action="store_true", default=DEFAULT_DRY_RUN)
    parser.add_argument("--warmup-cycles", type=int, default=DEFAULT_WARMUP_CYCLES)
    parser.add_argument("--algorithm-cooldown-s", type=float, default=DEFAULT_ALGORITHM_COOLDOWN_S)
    parser.set_defaults(strict_host_key=DEFAULT_STRICT_HOST_KEY)
    parser.add_argument("--cc", default=None)
    parser.add_argument(
        "--client-timeout-ms",
        type=int,
        default=DEFAULT_CLIENT_TIMEOUT_MS,
    )
    parser.add_argument(
        "--command-timeout-s",
        type=float,
        default=DEFAULT_COMMAND_TIMEOUT_S,
    )

    parser.add_argument("--iterations", type=int, default=DEFAULT_ITERATIONS)
    parser.add_argument(
        "--direction",
        choices=("both", "get", "post"),
        default=None,
        help="测试方向：both、get或post；未指定时使用代码顶部的DEFAULT_RUN_GET/POST",
    )
    parser.add_argument(
        "--order",
        type=validate_order,
        default=validate_order(DEFAULT_MODE_ORDER),
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=DEFAULT_RETRIES,
        help="extra retries after the first attempt (default: 1; 2 total attempts)",
    )
    parser.add_argument("--cooldown-s", type=float, default=DEFAULT_COOLDOWN_S)
    sha_group = parser.add_mutually_exclusive_group()
    sha_group.add_argument(
        "--verify-sha256",
        dest="verify_sha256",
        action="store_true",
        help="开启GET/POST文件SHA-256完整性校验",
    )
    sha_group.add_argument(
        "--no-verify-sha256",
        dest="verify_sha256",
        action="store_false",
        help="关闭GET/POST文件SHA-256完整性校验",
    )
    parser.set_defaults(verify_sha256=DEFAULT_VERIFY_SHA256)

    parser.add_argument(
        "--board-resource-monitor",
        default=DEFAULT_BOARD_RESOURCE_MONITOR,
    )
    parser.add_argument(
        "--board-resource-dir",
        default=DEFAULT_BOARD_RESOURCE_DIR,
    )
    parser.add_argument(
        "--board-resource-interval-s",
        type=float,
        default=DEFAULT_BOARD_RESOURCE_INTERVAL_S,
    )
    parser.add_argument(
        "--board-resource-retention-days",
        type=int,
        default=DEFAULT_BOARD_RESOURCE_RETENTION_DAYS,
    )

    parser.add_argument(
        "--server-resource-monitor",
        default=DEFAULT_SERVER_RESOURCE_MONITOR,
    )
    parser.add_argument(
        "--server-resource-dir",
        default=DEFAULT_SERVER_RESOURCE_DIR,
    )
    parser.add_argument(
        "--server-resource-interval-s",
        type=float,
        default=DEFAULT_SERVER_RESOURCE_INTERVAL_S,
    )
    parser.add_argument(
        "--server-resource-retention-days",
        type=int,
        default=DEFAULT_SERVER_RESOURCE_RETENTION_DAYS,
    )

    parser.add_argument(
        "--output-root",
        default=DEFAULT_OUTPUT_ROOT or str(script_dir / "log"),
        help="本机结果目录，默认是脚本目录下的log",
    )

    args = parser.parse_args(argv)
    try:
        if args.scheduler is not None:
            args.scheduler = validate_schedulers([args.scheduler])[0]
            args.schedulers = []
        elif args.schedulers is None:
            args.schedulers = validate_schedulers(DEFAULT_SCHEDULERS)
        if args.cc is not None and args.cc not in SUPPORTED_CC:
            parser.error("不支持的拥塞控制算法")
        for name in (DEFAULT_GET_SCHEDULER, DEFAULT_POST_SCHEDULER):
            validate_schedulers([name])
        if any(cc not in SUPPORTED_CC for cc in (DEFAULT_CC, DEFAULT_GET_CC,
                                                  DEFAULT_POST_CC)):
            parser.error("代码配置中的拥塞控制算法无效")
    except (argparse.ArgumentTypeError, TypeError) as exc:
        parser.error(str(exc))
    if args.execution_mode not in ("round-robin", "sequential"):
        parser.error("DEFAULT_EXECUTION_MODE配置无效")
    if args.sequence not in ("balanced", "fixed"):
        parser.error("DEFAULT_SEQUENCE配置无效")
    if args.warmup_cycles < 0:
        parser.error("预热周期数不能小于0")
    if not math.isfinite(args.algorithm_cooldown_s) or args.algorithm_cooldown_s < 0:
        parser.error("算法切换冷却时间必须为非负有限数")
    if args.direction is None:
        if DEFAULT_RUN_GET and DEFAULT_RUN_POST:
            args.direction = "both"
        elif DEFAULT_RUN_GET:
            args.direction = "get"
        elif DEFAULT_RUN_POST:
            args.direction = "post"
        else:
            parser.error("DEFAULT_RUN_GET和DEFAULT_RUN_POST不能同时为False")
    if args.iterations < 1:
        parser.error("--iterations必须大于0")
    if args.retries < 0:
        parser.error("--retries不能小于0")
    if not 1 <= args.quic_port <= 65535:
        parser.error("--quic-port无效")
    if not 1 <= args.board_ssh_port <= 65535:
        parser.error("--board-ssh-port无效")
    if not 1 <= args.server_ssh_port <= 65535:
        parser.error("--server-ssh-port无效")
    if args.client_timeout_ms <= 0 or args.command_timeout_s <= 0:
        parser.error("客户端和控制端超时必须大于0")
    if args.command_timeout_s <= args.client_timeout_ms / 1000.0:
        parser.error("--command-timeout-s必须大于客户端timeout")
    for value_name in (
        "server_ready_timeout_s",
        "cooldown_s",
        "board_resource_interval_s",
        "server_resource_interval_s",
    ):
        value = getattr(args, value_name)
        if not math.isfinite(value):
            parser.error(f"--{value_name.replace('_', '-')}必须是有限数值")
    if args.server_ready_timeout_s <= 0:
        parser.error("--server-ready-timeout-s必须大于0")
    if args.cooldown_s < 0:
        parser.error("--cooldown-s不能小于0")
    if args.board_resource_interval_s <= 0:
        parser.error("--board-resource-interval-s必须大于0")
    if args.server_resource_interval_s <= 0:
        parser.error("--server-resource-interval-s必须大于0")
    if args.board_resource_retention_days < 0:
        parser.error("--board-resource-retention-days不能小于0")
    if args.server_resource_retention_days < 0:
        parser.error("--server-resource-retention-days不能小于0")
    if args.accept_timeout_ms < 0 or args.serve_timeout_ms <= 0:
        parser.error("服务端timeout参数无效")
    active_directions = ("get", "post") if args.direction == "both" else (args.direction,)
    for direction in active_directions:
        scheduler, cc = transfer_algorithms_for_direction(args, direction)
        if not scheduler or not cc:
            parser.error(f"{direction.upper()}的scheduler和cc不能为空")
    if not args.server_name:
        args.server_name = args.quic_host

    if args.direction in {"both", "post"}:
        post_remote_paths = (
            args.post_remote_rmnet,
            args.post_remote_usb,
            args.post_remote_agg,
        )
        if len(set(post_remote_paths)) != len(post_remote_paths):
            parser.error("三个POST远端路径必须互不相同")
        if args.direction == "both" and args.remote_path in post_remote_paths:
            parser.error("POST远端路径不能与GET源文件路径相同，以免删除GET源文件")

    # Compatibility fields used by the proven helper functions above.
    # 控制机本地路径相对于脚本目录，不受编辑器工作目录或Windows用户名影响。
    for name in ("output_root", "board_ssh_key", "server_ssh_key"):
        value = getattr(args, name)
        if value:
            local_path = Path(value).expanduser()
            if not local_path.is_absolute():
                local_path = script_dir / local_path
            setattr(args, name, str(local_path.resolve()))
    args.board_transport = "ssh"
    args.disable_board_resource_monitor = False
    args.keep_downloads = False
    return args


def validate_server_scheduler_support(server: SSHRemote, args) -> None:
    """Read usage only; the legacy server may print it with exit code 1."""
    result = server.run(
        f"cd -- {q(args.server_dir)} && {q(args.server_executable)} --help",
        timeout_s=20,
    )
    if (
        result.timed_out
        or result.truncated
        or result.exit_code not in (0, 1)
        or re.search(r"^\s*--scheduler\s+", result.diagnostic_output, re.MULTILINE) is None
    ):
        raise RuntimeError(
            "服务端不支持 --scheduler 或无法读取其用法；"
            "请先在原构建环境编译并部署配套服务端。未开始传输。\n"
            + result.diagnostic_output[-2000:]
        )


def start_server_quiet(
    server: SSHRemote,
    args,
    max_connections: int,
    remote_pidfile: str,
    session_token: str,
    scheduler: str = "adaptive",
) -> int:
    if scheduler not in SUPPORTED_SCHEDULERS:
        raise ValueError(f"不支持的服务端调度算法：{scheduler}")
    state_dir = posixpath.dirname(remote_pidfile)
    startup_file = posixpath.join(state_dir, "server_startup.log")
    port_pattern = f":{int(args.quic_port)}([[:space:]]|$)"
    command = (
        f"cd -- {q(args.server_dir)} || exit 90; "
        f"mkdir -p {q(state_dir)} || exit 91; "
        f"chmod 700 {q(state_dir)} || exit 92; "
        "if ss -H -lunp 2>/dev/null "
        f"| grep -Eq {q(port_pattern)}; then "
        "printf '__MINI_SERVER_PORT_IN_USE__\\n' >&2; exit 93; fi; "
        f"MINI_API_BENCH_SESSION={q(session_token)} "
        f"nohup {q(args.server_executable)} "
        f"--bind {q(args.server_bind)} "
        f"--port {int(args.quic_port)} "
        "--cert \"$PWD/server.crt\" --key \"$PWD/server.key\" --root \"$PWD\" "
        f"--accept-timeout-ms {int(args.accept_timeout_ms)} "
        f"--serve-timeout-ms {int(args.serve_timeout_ms)} "
        f"--max-connections {int(max_connections)} "
        f"--scheduler {q(scheduler)} "
        f"> {q(startup_file)} 2>/dev/null < /dev/null & "
        "server_pid=$!; "
        "server_start=$(awk '{print $22}' \"/proc/$server_pid/stat\" 2>/dev/null || true); "
        f"printf '%s %s\\n' \"$server_pid\" \"$server_start\" > {q(remote_pidfile)}; "
        "printf '__MINI_SERVER_PID__=%s\\n' \"$server_pid\""
    )
    result = server.run(command, timeout_s=20)
    match = SERVER_PID_RE.search(result.output)
    if result.exit_code != 0 or result.timed_out or result.truncated or not match:
        if "__MINI_SERVER_PORT_IN_USE__" in result.diagnostic_output:
            raise RuntimeError(
                f"云服务器UDP端口{args.quic_port}已被占用；请先确认并停止旧服务"
            )
        raise RuntimeError(
            f"启动云端服务端失败: {result.diagnostic_output.strip()}"
        )
    pid = int(match.group(1))

    try:
        deadline = time.monotonic() + float(args.server_ready_timeout_s)
        while time.monotonic() < deadline:
            probe_command = (
                f"if ! kill -0 {pid} 2>/dev/null; then "
                "printf '__MINI_SERVER_EXITED__\\n'; exit 2; "
                f"elif ss -H -lunp 2>/dev/null | grep -F {q('pid=' + str(pid) + ',')} "
                f"| grep -Eq {q(':' + str(args.quic_port) + '([[:space:]]|$)')}; then "
                "printf '__MINI_SERVER_READY__\\n'; "
                "else printf '__MINI_SERVER_WAIT__\\n'; fi"
            )
            probe = server.run(probe_command, timeout_s=5)
            if probe.exit_code != 0 or probe.timed_out or probe.truncated:
                raise RuntimeError(
                    "云端API服务端退出或就绪检查失败: "
                    f"{probe.diagnostic_output.strip()}"
                )
            if "__MINI_SERVER_READY__" in probe.output:
                startup = server.run(f"cat -- {q(startup_file)}", timeout_s=5)
                if startup.timed_out or startup.truncated or startup.exit_code not in (0, 1):
                    raise RuntimeError("无法完整读取服务端启动信息，拒绝开始传输")
                match = re.search(
                    r"^listening address=[^\r\n]*\bscheduler=([a-z0-9_]+)[ \t]*\r?\n",
                    startup.output, re.MULTILINE,
                )
                if match is not None:
                    actual_scheduler = match.group(1)
                    if actual_scheduler != scheduler:
                        raise RuntimeError(
                            f"服务端算法不匹配：计划={scheduler}，启动输出={actual_scheduler}"
                        )
                    LOG.info(
                        "云端API服务端已就绪: pid=%s udp=%s scheduler=%s max_connections=%s",
                        pid, args.quic_port, actual_scheduler, max_connections,
                    )
                    return pid
                # 端口可先于启动信息刷新就绪，继续在原有超时内等待。
            time.sleep(0.5)

        raise RuntimeError("等待云端API服务端监听端口及实际算法确认超时")
    except BaseException:
        stop_server(server, pid, args.server_executable, remote_pidfile, session_token)
        raise



def validate_server_resource_monitor(server: SSHRemote, args) -> None:
    command = (
        f"test -x {q(args.server_resource_monitor)}"
        " && command -v bash >/dev/null"
        " && command -v awk >/dev/null"
        " && command -v sleep >/dev/null"
        " && command -v date >/dev/null"
        " && command -v find >/dev/null"
        " && command -v ss >/dev/null"
        " && command -v getconf >/dev/null"
        f" && mkdir -p {q(args.server_resource_dir)}"
        f" && test -w {q(args.server_resource_dir)}"
    )
    result = server.run(command, timeout_s=30)
    if result.exit_code != 0:
        raise RuntimeError(
            "云服务器资源监控预检失败，请确认脚本和目录存在: "
            f"{result.diagnostic_output.strip()}"
        )
    LOG.info(
        "云服务器资源监控预检通过: monitor=%s interval=%ss",
        args.server_resource_monitor,
        args.server_resource_interval_s,
    )


def cleanup_stale_server_resource_files(server: SSHRemote, args) -> None:
    days = int(args.server_resource_retention_days)
    command = (
        "set -e; "
        f"mkdir -p {q(args.server_resource_dir)}; "
        f"find {q(args.server_resource_dir)} -maxdepth 1 -type f "
        "-name 'api_*_server_memory.csv' "
        f"-mtime +{days} -delete; "
        "find /tmp -maxdepth 1 -type f "
        "\\( -name 'api_*_server_memory.stop' -o "
        "-name 'api_*_server_memory.status' \\) "
        f"-mtime +{days} -delete"
    )
    result = server.run(command, timeout_s=30)
    if result.exit_code != 0 or result.timed_out:
        LOG.warning(
            "云服务器历史资源文件清理未确认: exit=%s output=%s",
            result.exit_code,
            result.diagnostic_output.strip(),
        )
    else:
        LOG.info(
            "云服务器历史资源文件启动清理完成: retention_days=%s dir=%s",
            days,
            args.server_resource_dir,
        )


def start_server_resource_monitor(
    server: SSHRemote,
    args,
    run_tag: str,
    remote_pidfile: str,
) -> Dict[str, object]:
    remote_csv = posixpath.join(
        args.server_resource_dir,
        f"{run_tag}_server_memory.csv",
    )
    remote_stop = f"/tmp/{run_tag}_server_memory.stop"
    remote_status = f"/tmp/{run_tag}_server_memory.status"
    token_line = f"MINI_API_BENCH_SERVER_MONITOR_SESSION={run_tag}"
    process_name = posixpath.basename(args.server_executable)
    wrapper = (
        f"{q(args.server_resource_monitor)} {q(remote_csv)} {q(remote_stop)} "
        f"{q(str(args.server_resource_interval_s))} {q(process_name)} "
        f"{q(remote_pidfile)} 0; rc=$?; "
        f"printf '%s\\n' \"$rc\" > {q(remote_status)}; exit \"$rc\""
    )
    command = (
        f"rm -f -- {q(remote_csv)} {q(remote_stop)} {q(remote_status)}; "
        f"mkdir -p {q(args.server_resource_dir)}; "
        f"nohup env {q(token_line)} bash -c {q(wrapper)} "
        "> /dev/null 2>&1 < /dev/null & "
        "monitor_pid=$!; "
        "monitor_start=$(awk '{print $22}' \"/proc/$monitor_pid/stat\" 2>/dev/null || true); "
        "sleep 0.05; "
        "if kill -0 \"$monitor_pid\" 2>/dev/null && [ -n \"$monitor_start\" ]; then "
        "printf '__API_SERVER_MONITOR_PID__=%s\\n' \"$monitor_pid\"; "
        "printf '__API_SERVER_MONITOR_START__=%s\\n' \"$monitor_start\"; "
        "else exit 3; fi"
    )
    try:
        result = server.run(command, timeout_s=10)
    except Exception as exc:
        LOG.warning("启动云服务器资源监控异常: %s", exc)
        return {
            "status": "START_FAILED",
            "pid": None,
            "start": None,
            "token": token_line,
            "remote_csv": remote_csv,
            "remote_stop": remote_stop,
            "remote_status": remote_status,
        }
    pid_match = SERVER_MONITOR_PID_RE.search(result.output)
    start_match = SERVER_MONITOR_START_RE.search(result.output)
    if result.exit_code != 0 or not pid_match or not start_match:
        LOG.warning(
            "启动云服务器资源监控失败: exit=%s timed_out=%s output=%s",
            result.exit_code,
            result.timed_out,
            result.diagnostic_output.strip(),
        )
        return {
            "status": "START_FAILED",
            "pid": None,
            "start": None,
            "token": token_line,
            "remote_csv": remote_csv,
            "remote_stop": remote_stop,
            "remote_status": remote_status,
        }
    return {
        "status": "OK",
        "pid": int(pid_match.group(1)),
        "start": start_match.group(1),
        "token": token_line,
        "remote_csv": remote_csv,
        "remote_stop": remote_stop,
        "remote_status": remote_status,
    }



def stop_server_resource_monitor(
    server: SSHRemote,
    state: Dict[str, object],
) -> str:
    monitor_pid = state.get("pid")
    expected_start = str(state.get("start") or "")
    token_line = str(state.get("token") or "")
    remote_stop = str(state.get("remote_stop") or "")
    remote_status = str(state.get("remote_status") or "")
    if not monitor_pid:
        return str(state.get("status") or "START_FAILED")

    ownership_check = (
        f"current_start=$(awk '{{print $22}}' /proc/{int(monitor_pid)}/stat "
        "2>/dev/null || true); "
        f"token_ok=0; tr '\\000' '\\n' < /proc/{int(monitor_pid)}/environ "
        f"2>/dev/null | grep -Fqx {q(token_line)} && token_ok=1; "
        f"[ \"$current_start\" = {q(expected_start)} ] && [ \"$token_ok\" -eq 1 ]"
    )
    command = (
        f"touch {q(remote_stop)}; "
        "i=0; "
        f"while kill -0 {int(monitor_pid)} 2>/dev/null && [ \"$i\" -lt 50 ]; "
        "do sleep 0.1; i=$((i+1)); done; "
        f"if kill -0 {int(monitor_pid)} 2>/dev/null; then "
        f"if {ownership_check}; then kill -TERM {int(monitor_pid)} 2>/dev/null || true; "
        "else printf '__API_MONITOR_PID_REUSED__\\n'; exit 7; fi; sleep 0.2; fi; "
        f"if kill -0 {int(monitor_pid)} 2>/dev/null; then "
        f"if {ownership_check}; then kill -KILL {int(monitor_pid)} 2>/dev/null || true; "
        "else printf '__API_MONITOR_PID_REUSED__\\n'; exit 7; fi; sleep 0.1; fi; "
        f"rm -f -- {q(remote_stop)}; "
        f"if kill -0 {int(monitor_pid)} 2>/dev/null; then exit 5; fi; "
        f"if [ ! -r {q(remote_status)} ]; then exit 6; fi; "
        f"read monitor_rc < {q(remote_status)}; "
        "case \"$monitor_rc\" in ''|*[!0-9]*) exit 6 ;; esac; "
        "[ \"$monitor_rc\" -eq 0 ]"
    )
    try:
        result = server.run(command, timeout_s=10)
    except Exception as exc:
        LOG.warning("停止云服务器资源监控异常: %s", exc)
        return "STOP_FAILED"
    if result.exit_code == 0 and not result.timed_out:
        return "OK"
    if "__API_MONITOR_PID_REUSED__" in result.diagnostic_output:
        LOG.warning("拒绝停止已被复用的服务器监控PID: %s", monitor_pid)
        return "PID_REUSED"
    return "RUNTIME_FAILED" if result.exit_code == 1 else "STOP_FAILED"



def empty_server_resource_result(
    status: str,
    csv_path: str = "",
    *,
    csv_parse_status: str = "NOT_PARSED",
    monitor_stop_status: str = "NOT_RUN",
) -> Dict[str, object]:
    return {
        "server_memory_status": status,
        "server_csv_parse_status": csv_parse_status,
        "server_monitor_stop_status": monitor_stop_status,
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


def parse_and_append_server_resource(
    source_csv: Path,
    combined_csv: Path,
    run_tag: str,
    mode: str,
    direction: str,
    round_index: int,
    iteration: int,
    attempt: int,
    monitor_stop_status: str,
) -> Dict[str, object]:
    required = {
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
    }
    try:
        with source_csv.open("r", encoding="utf-8", newline="") as handle:
            reader = csv.DictReader(handle)
            if not reader.fieldnames or not required.issubset(reader.fieldnames):
                return empty_server_resource_result(
                    "PARSE_ERROR",
                    str(source_csv),
                    csv_parse_status="PARSE_ERROR",
                    monitor_stop_status=monitor_stop_status,
                )
            rows = list(reader)
    except (OSError, csv.Error):
        return empty_server_resource_result(
            "PARSE_ERROR",
            str(source_csv),
            csv_parse_status="PARSE_ERROR",
            monitor_stop_status=monitor_stop_status,
        )
    if not rows:
        return empty_server_resource_result(
            "EMPTY",
            str(source_csv),
            csv_parse_status="EMPTY",
            monitor_stop_status=monitor_stop_status,
        )

    system_cpu: List[float] = []
    process_cpu: List[float] = []
    system_mem: List[float] = []
    system_total: List[float] = []
    process_rss: List[float] = []
    process_hwm: List[float] = []
    process_vmsize: List[float] = []
    server_pids = []
    normalized_rows: List[Dict[str, object]] = []
    try:
        for row in rows:
            sys_cpu = float(row["system_cpu_pct"])
            proc_cpu = float(row["server_process_cpu_pct"])
            sys_mem = float(row["system_used_kb"])
            sys_total = float(row["mem_total_kb"])
            rss = float(row["server_rss_kb"])
            hwm = float(row["server_hwm_kb"])
            vmsize = float(row["server_vmsize_kb"])
            server_pid = int(row["server_pid"])
            values = (sys_cpu, proc_cpu, sys_mem, sys_total, rss, hwm, vmsize)
            if not all(math.isfinite(value) for value in values):
                raise ValueError("non-finite server resource value")
            if not 0 <= sys_cpu <= 100 or proc_cpu < 0:
                raise ValueError("invalid CPU percentage")
            if min(sys_mem, sys_total, rss, hwm, vmsize) < 0 or sys_total <= 0:
                raise ValueError("invalid memory value")
            if server_pid <= 0:
                raise ValueError("invalid server PID")
            system_cpu.append(sys_cpu)
            process_cpu.append(proc_cpu)
            system_mem.append(sys_mem)
            system_total.append(sys_total)
            process_rss.append(rss)
            process_hwm.append(hwm)
            process_vmsize.append(vmsize)
            server_pids.append(server_pid)
            normalized_rows.append(
                {
                    **row,
                    "run_tag": run_tag,
                    "mode": DISPLAY_MODE_NAMES[mode],
                    "direction": direction,
                    "round": round_index,
                    "iteration": iteration,
                    "attempt": attempt,
                    "csv_parse_status": "OK",
                    "monitor_stop_status": monitor_stop_status,
                    "aggregation_eligible": "1",
                }
            )
    except (KeyError, TypeError, ValueError):
        return empty_server_resource_result(
            "PARSE_ERROR",
            str(source_csv),
            csv_parse_status="PARSE_ERROR",
            monitor_stop_status=monitor_stop_status,
        )
    if len(set(server_pids)) != 1:
        return empty_server_resource_result(
            "PARSE_ERROR",
            str(source_csv),
            csv_parse_status="PARSE_ERROR",
            monitor_stop_status=monitor_stop_status,
        )

    combined_csv.parent.mkdir(parents=True, exist_ok=True)
    combined_fields = [
        "run_tag", "mode", "direction", "round", "iteration", "attempt",
        "csv_parse_status", "monitor_stop_status", "aggregation_eligible",
        "elapsed_s", "timestamp_utc", "system_cpu_pct", "system_used_kb",
        "system_used_pct", "mem_total_kb", "mem_available_kb", "cached_kb",
        "buffers_kb", "server_pid", "server_process_cpu_pct", "server_rss_kb",
        "server_hwm_kb", "server_vmsize_kb",
    ]
    write_header = not combined_csv.exists() or combined_csv.stat().st_size == 0
    combined_encoding = "utf-8-sig" if write_header else "utf-8"
    with combined_csv.open("a", encoding=combined_encoding, newline="") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=combined_fields,
            extrasaction="ignore",
        )
        if write_header:
            writer.writeheader()
        writer.writerows(normalized_rows)

    # A syntactically and numerically valid CSV remains usable even when the
    # monitor could not be stopped cleanly.  Preserve the stop status as an
    # independent warning instead of silently discarding valid measurements.
    aggregation_eligible = True
    return {
        "server_memory_status": monitor_stop_status,
        "server_csv_parse_status": "OK",
        "server_monitor_stop_status": monitor_stop_status,
        "server_memory_aggregation_eligible": aggregation_eligible,
        "server_memory_csv": str(combined_csv),
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


def collect_server_resource(
    server: SSHRemote,
    state: Dict[str, object],
    stop_status: str,
    combined_csv: Path,
    temp_dir: Path,
    run_tag: str,
    mode: str,
    direction: str,
    round_index: int,
    iteration: int,
    attempt: int,
) -> Dict[str, object]:
    remote_csv = str(state.get("remote_csv") or "")
    remote_stop = str(state.get("remote_stop") or "")
    remote_status = str(state.get("remote_status") or "")
    if str(state.get("status")) != "OK":
        cleanup_remote_files_verified(
            server,
            [path for path in (remote_csv, remote_stop, remote_status) if path],
            "云服务器",
        )
        return empty_server_resource_result(
            "START_FAILED",
            csv_parse_status="NOT_AVAILABLE",
            monitor_stop_status=stop_status,
        )
    temp_dir.mkdir(parents=True, exist_ok=True)
    local_temp = temp_dir / f"{run_tag}_server_memory.csv"
    download_ok, _ = download_remote_with_retry(
        server,
        remote_csv,
        local_temp,
        "云服务器资源CSV",
    )
    if not download_ok:
        cleanup_remote_files_verified(server, [remote_stop], "云服务器")
        # Keep CSV/status remotely for a later recovery attempt.
        return empty_server_resource_result(
            "DOWNLOAD_ERROR",
            csv_parse_status="NOT_AVAILABLE",
            monitor_stop_status=stop_status,
        )

    parsed = parse_and_append_server_resource(
        local_temp,
        combined_csv,
        run_tag,
        mode,
        direction,
        round_index,
        iteration,
        attempt,
        stop_status,
    )
    csv_parse_ok = parsed["server_csv_parse_status"] == "OK"
    monitor_stop_ok = parsed["server_monitor_stop_status"] == "OK"
    if csv_parse_ok and monitor_stop_ok:
        local_temp.unlink(missing_ok=True)
    elif not csv_parse_ok:
        failed_dir = combined_csv.parent / "server_resource_unparsed" / combined_csv.stem
        failed_dir.mkdir(parents=True, exist_ok=True)
        failed_path = failed_dir / local_temp.name
        os.replace(local_temp, failed_path)
        parsed["server_memory_csv"] = str(failed_path)
        LOG.warning("云服务器资源CSV解析异常，本地原始副本已保留: %s", failed_path)
    else:
        failed_dir = (
            combined_csv.parent / "server_resource_monitor_failed" / combined_csv.stem
        )
        failed_dir.mkdir(parents=True, exist_ok=True)
        failed_path = failed_dir / local_temp.name
        os.replace(local_temp, failed_path)
        parsed["server_memory_csv"] = str(failed_path)
        LOG.warning(
            "云服务器资源CSV解析成功，但监控停止状态异常(%s)，"
            "原始副本已保留，本轮有效样本仍计入资源平均值: %s",
            parsed["server_monitor_stop_status"],
            failed_path,
        )

    if not cleanup_remote_files_verified(
        server,
        [remote_csv, remote_stop, remote_status],
        "云服务器",
    ):
        LOG.warning(
            "云服务器资源CSV已保存到本地，但远端本次文件删除未确认: %s",
            remote_csv,
        )
    return parsed


def collect_board_resource_gx(
    board: SSHRemote,
    monitor_state: str,
    remote_resource_file: str,
    remote_stop_file: str,
    remote_monitor_log: str,
    local_resource_file: Path,
) -> Dict[str, object]:
    download_ok, _ = download_remote_with_retry(
        board,
        remote_resource_file,
        local_resource_file,
        "主板资源CSV",
    )
    if not download_ok:
        # Logs are not retained by design; preserve only the undownloaded CSV.
        cleanup_remote_resource_files(board, [remote_stop_file, remote_monitor_log])
        if monitor_state == "START_FAILED":
            return empty_resource_result("MONITOR_START_FAILED")
        if monitor_state == "RUNTIME_FAILED":
            return empty_resource_result("MONITOR_RUNTIME_FAILED")
        return empty_resource_result("DOWNLOAD_ERROR")

    parsed = parse_resource_csv(local_resource_file)
    if monitor_state == "START_FAILED":
        parsed["resource_status"] = "MONITOR_START_FAILED"
    elif monitor_state == "RUNTIME_FAILED":
        parsed["resource_status"] = "MONITOR_RUNTIME_FAILED"
    elif monitor_state not in {"OK", "MISSING_STATUS"}:
        parsed["resource_status"] = monitor_state

    if not cleanup_remote_resource_files(
        board,
        [remote_resource_file, remote_stop_file, remote_monitor_log],
    ):
        LOG.warning(
            "主板资源CSV已保存到本地，但远端本次文件删除未确认: %s",
            remote_resource_file,
        )
    return parsed



def cleanup_current_session_remote_artifacts(
    board: SSHRemote,
    server: SSHRemote,
    args,
    session: str,
    remote_state_dir: str,
    remove_server_state: bool,
    directions: Sequence[str] = ("get", "post"),
) -> None:
    """Remove controls/downloads; undownloaded resource CSVs are preserved."""
    run_prefix = f"api_{session}_*"

    if board.is_connected():
        board_output_patterns = (
            f"\\( -name {q('get-rmnet-' + session + '-*.bin')} "
            f"-o -name {q('get-usb-' + session + '-*.bin')} "
            f"-o -name {q('get-agg-' + session + '-*.bin')} \\)"
        )
        board_command = (
            "set -e; "
            f"find /tmp -maxdepth 1 -type f -name {q(run_prefix)} -delete; "
            f"find {q(args.board_dir)} -maxdepth 1 -type f {board_output_patterns} -delete; "
            f"rm -f -- {q(posixpath.join(args.board_dir, 'clog.log'))}; "
            f"test -z \"$(find /tmp -maxdepth 1 -type f -name {q(run_prefix)} -print -quit)\" && "
            f"test -z \"$(find {q(args.board_dir)} -maxdepth 1 -type f "
            f"{board_output_patterns} -print -quit)\" && "
            f"test ! -e {q(posixpath.join(args.board_dir, 'clog.log'))}"
        )
        try:
            result = board.run(board_command, timeout_s=30)
            if result.exit_code != 0 or result.timed_out:
                LOG.warning(
                    "主板本次会话最终清理未确认: %s",
                    result.diagnostic_output.strip(),
                )
            else:
                LOG.info("主板本次会话控制文件和GET下载文件已清空")
        except Exception as exc:
            LOG.warning("主板本次会话最终清理异常: %s", exc)

    if server.is_connected():
        post_cleanup = ""
        post_verify = "true"
        if "post" in directions:
            post_targets = [
                server_path_for_remote(args.server_dir, args.post_remote_rmnet),
                server_path_for_remote(args.server_dir, args.post_remote_usb),
                server_path_for_remote(args.server_dir, args.post_remote_agg),
            ]
            post_cleanup = "rm -f -- " + " ".join(q(item) for item in post_targets) + "; "
            post_verify = " && ".join(f"test ! -e {q(item)}" for item in post_targets)
        state_cleanup = (
            f"rm -rf -- {q(remote_state_dir)}; test ! -e {q(remote_state_dir)}"
            if remove_server_state else "true"
        )
        server_command = (
            "set -e; "
            f"find /tmp -maxdepth 1 -type f -name {q(run_prefix)} -delete; "
            + post_cleanup
            + f"test -z \"$(find /tmp -maxdepth 1 -type f -name {q(run_prefix)} -print -quit)\" && "
            + post_verify + " && " + state_cleanup
        )
        try:
            result = server.run(server_command, timeout_s=30)
            if result.exit_code != 0 or result.timed_out:
                LOG.warning(
                    "云服务器本次会话最终清理未确认: %s",
                    result.diagnostic_output.strip(),
                )
            elif remove_server_state:
                LOG.info("云服务器POST文件、控制文件和状态目录已清空")
            else:
                LOG.warning(
                    "API服务端停止未确认，已清理POST文件但保留状态目录: %s",
                    remote_state_dir,
                )
        except Exception as exc:
            LOG.warning("云服务器本次会话最终清理异常: %s", exc)


def write_speed_log(
    log_root: Path,
    display_mode: str,
    direction: str,
    iteration: int,
    speed_row: Optional[Dict[str, object]],
    fallback_start: str,
    fallback_end: str,
) -> Path:
    timestamp = datetime.now().strftime("%H%M%S_%f")
    path = log_root / (
        f"{display_mode}_summary_{direction}_iter{iteration:02d}_{timestamp}.log"
    )
    if speed_row is None:
        row = {
            "iter": iteration,
            "filesize": "",
            "time_ms": "",
            "mbps": "",
            "mode": direction,
            "start_time": fallback_start,
            "end_time": fallback_end,
        }
    else:
        row = dict(speed_row)
        row["filesize"] = str(int(row["filesize"]))
        row["time_ms"] = f"{float(row['time_ms']):.3f}"
        row["mbps"] = f"{float(row['mbps']):.3f}"
    average_row = dict(row)
    average_row["iter"] = "AVERAGE"
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=SPEED_FIELDS)
        writer.writeheader()
        writer.writerow(row)
        writer.writerow(average_row)
    return path


def aggregate_board_run_dicts(runs: Sequence[Dict[str, object]]) -> Dict[str, object]:
    valid = [
        run
        for run in runs
        if run.get("resource_status") == "OK"
        and int(run.get("resource_samples") or 0) > 0
    ]
    empty = {
        "resource_valid": 0,
        "resource_missing": len(runs),
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
    if not valid:
        return empty
    count = len(valid)
    sample_count = sum(int(run["resource_samples"]) for run in valid)
    return {
        "resource_valid": count,
        "resource_missing": len(runs) - count,
        "client_process_cpu_avg_pct": sum(
            float(run["client_process_cpu_avg_pct"])
            * int(run["resource_samples"])
            for run in valid
        ) / sample_count,
        "client_process_cpu_peak_avg_pct": sum(
            float(run["client_process_cpu_max_pct"]) for run in valid
        ) / count,
        "client_process_cpu_max_pct": max(
            float(run["client_process_cpu_max_pct"]) for run in valid
        ),
        "client_process_rss_avg_kb": sum(
            float(run["client_process_rss_avg_kb"])
            * int(run["resource_samples"])
            for run in valid
        ) / sample_count,
        "client_process_rss_max_kb": max(
            float(run["client_process_rss_max_kb"]) for run in valid
        ),
        "client_process_rss_delta_avg_kb": sum(
            float(run["client_process_rss_delta_kb"]) for run in valid
        ) / count,
        "client_process_hwm_max_kb": max(
            float(run["client_process_hwm_max_kb"]) for run in valid
        ),
        "client_process_vmsize_max_kb": max(
            float(run["client_process_vmsize_max_kb"]) for run in valid
        ),
        "resource_samples": sample_count,
    }


def aggregate_server_run_dicts(runs: Sequence[Dict[str, object]]) -> Dict[str, object]:
    valid = [
        run
        for run in runs
        if run.get(
            "server_memory_aggregation_eligible",
            run.get("server_memory_status") == "OK",
        ) is True
        and int(run.get("server_memory_samples") or 0) > 0
    ]
    empty = {
        "server_memory_valid": 0,
        "server_memory_missing": len(runs),
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
    if not valid:
        return empty
    count = len(valid)
    mem_samples = sum(int(run["server_memory_samples"]) for run in valid)
    cpu_samples = sum(int(run["server_cpu_samples"]) for run in valid)
    return {
        "server_memory_valid": count,
        "server_memory_missing": len(runs) - count,
        "server_system_cpu_avg_pct": sum(
            float(run["server_system_cpu_avg_pct"]) * int(run["server_cpu_samples"])
            for run in valid
        ) / cpu_samples,
        "server_system_cpu_peak_avg_pct": sum(
            float(run["server_system_cpu_max_pct"]) for run in valid
        ) / count,
        "server_system_cpu_max_pct": max(
            float(run["server_system_cpu_max_pct"]) for run in valid
        ),
        "server_process_cpu_avg_pct": sum(
            float(run["server_process_cpu_avg_pct"]) * int(run["server_cpu_samples"])
            for run in valid
        ) / cpu_samples,
        "server_process_cpu_peak_avg_pct": sum(
            float(run["server_process_cpu_max_pct"]) for run in valid
        ) / count,
        "server_process_cpu_max_pct": max(
            float(run["server_process_cpu_max_pct"]) for run in valid
        ),
        "server_system_mem_avg_kb": sum(
            float(run["server_system_mem_avg_kb"]) * int(run["server_memory_samples"])
            for run in valid
        ) / mem_samples,
        "server_system_mem_max_kb": max(
            float(run["server_system_mem_max_kb"]) for run in valid
        ),
        "server_system_mem_total_kb": max(
            float(run["server_system_mem_total_kb"]) for run in valid
        ),
        "server_system_mem_delta_avg_kb": sum(
            float(run["server_system_mem_delta_kb"]) for run in valid
        ) / count,
        "server_process_rss_avg_kb": sum(
            float(run["server_process_rss_avg_kb"]) * int(run["server_memory_samples"])
            for run in valid
        ) / mem_samples,
        "server_process_rss_max_kb": max(
            float(run["server_process_rss_max_kb"]) for run in valid
        ),
        "server_process_rss_delta_avg_kb": sum(
            float(run["server_process_rss_delta_kb"]) for run in valid
        ) / count,
        "server_process_hwm_max_kb": max(
            float(run["server_process_hwm_max_kb"]) for run in valid
        ),
        "server_process_vmsize_max_kb": max(
            float(run["server_process_vmsize_max_kb"]) for run in valid
        ),
        "server_memory_samples": mem_samples,
        "server_cpu_samples": cpu_samples,
    }


def write_gx_final_summary(
    log_root: Path,
    session: str,
    records: Sequence[Dict[str, object]],
    planned_iterations: int,
    directions: Sequence[str] = ("get", "post"),
) -> Path:
    fieldnames = [
        "mode", "direction", "planned", "successful", "first_attempt_successes", "first_attempt_success_rate", "retried_successes",
        "failed", "unfinished", "success_rate", "median_mbps", "stdev_mbps", "average_filesize", "average_time_ms",
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
        for direction in (item for item in ("post", "get") if item in directions):
            for internal_mode in ("agg", "usb", "rmnet"):
                mode_records = [
                    record
                    for record in records
                    if record["mode"] == internal_mode
                    and record["direction"] == direction
                ]
                successful = [record for record in mode_records if record["success"]]
                board = aggregate_board_run_dicts(
                    [record["board_resource"] for record in successful]
                )
                server = aggregate_server_run_dicts(
                    [record["server_resource"] for record in successful]
                )
                success_count = len(successful)
                writer.writerow(
                    {
                        "mode": DISPLAY_MODE_NAMES[internal_mode],
                        "direction": direction,
                        "planned": planned_iterations,
                        "successful": success_count,
                        "retried_successes": sum(
                            1 for record in successful if int(record["attempt"]) > 1
                        ),
                        "failed": sum(not r["success"] for r in mode_records),
                        "unfinished": planned_iterations - len(mode_records),
                        "first_attempt_successes": sum(int(r["attempt"]) == 1 for r in successful),
                        "first_attempt_success_rate": f"{sum(int(r['attempt']) == 1 for r in successful) / planned_iterations:.4f}",
                        "median_mbps": statistics.median(float(r["mbps"]) for r in successful) if successful else "",
                        "stdev_mbps": statistics.stdev(float(r["mbps"]) for r in successful) if len(successful) > 1 else "",
                        "success_rate": f"{success_count / planned_iterations:.4f}",
                        "average_filesize": (
                            sum(int(record["filesize"]) for record in successful)
                            / success_count if successful else ""
                        ),
                        "average_time_ms": (
                            sum(float(record["time_ms"]) for record in successful)
                            / success_count if successful else ""
                        ),
                        "average_mbps": (
                            sum(float(record["mbps"]) for record in successful)
                            / success_count if successful else ""
                        ),
                        **board,
                        **server,
                    }
                )
    return path


def write_aggregation_summary(
    log_root: Path,
    session: str,
    records: Sequence[Dict[str, object]],
    planned_iterations: int,
    direction: str,
) -> Path:
    fields = [
        "round", "5G1_mbps", "5G2_mbps", "Agg_mbps",
        "best_single_mbps", "sum_single_mbps", "gain_vs_best_single_pct",
        "aggregation_efficiency_pct", "status", "missing_modes",
    ]
    path = log_root / f"aggregation_summary_{direction}_{session}.csv"
    rows: List[Dict[str, object]] = []
    complete_rows: List[Dict[str, object]] = []
    successful = {
        (int(record["round"]), str(record["mode"])): record
        for record in records
        if record["success"] and record["direction"] == direction
    }
    labels = (("rmnet", "5G1"), ("usb", "5G2"), ("agg", "Agg"))
    for round_index in range(1, planned_iterations + 1):
        samples = {
            mode: successful.get((round_index, mode))
            for mode, _label in labels
        }
        missing = [label for mode, label in labels if samples[mode] is None]
        row: Dict[str, object] = {
            "round": round_index,
            "5G1_mbps": (
                f"{float(samples['rmnet']['mbps']):.3f}"
                if samples["rmnet"] is not None else ""
            ),
            "5G2_mbps": (
                f"{float(samples['usb']['mbps']):.3f}"
                if samples["usb"] is not None else ""
            ),
            "Agg_mbps": (
                f"{float(samples['agg']['mbps']):.3f}"
                if samples["agg"] is not None else ""
            ),
            "best_single_mbps": "",
            "sum_single_mbps": "",
            "gain_vs_best_single_pct": "",
            "aggregation_efficiency_pct": "",
            "status": "INCOMPLETE" if missing else "COMPLETE",
            "missing_modes": ";".join(missing),
        }
        if not missing:
            rmnet_mbps = float(samples["rmnet"]["mbps"])
            usb_mbps = float(samples["usb"]["mbps"])
            agg_mbps = float(samples["agg"]["mbps"])
            best = max(rmnet_mbps, usb_mbps)
            total = rmnet_mbps + usb_mbps
            row.update(
                {
                    "best_single_mbps": f"{best:.3f}",
                    "sum_single_mbps": f"{total:.3f}",
                    "gain_vs_best_single_pct": (
                        f"{((agg_mbps / best - 1.0) * 100.0):.3f}"
                        if best > 0 else ""
                    ),
                    "aggregation_efficiency_pct": (
                        f"{(agg_mbps / total * 100.0):.3f}"
                        if total > 0 else ""
                    ),
                }
            )
            complete_rows.append(row)
        rows.append(row)

    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)
        if complete_rows:
            numeric_fields = [
                "5G1_mbps", "5G2_mbps", "Agg_mbps",
                "best_single_mbps", "sum_single_mbps",
                "gain_vs_best_single_pct", "aggregation_efficiency_pct",
            ]
            average_row: Dict[str, object] = {
                "round": "AVERAGE",
                "status": "COMPLETE_ROUNDS_ONLY",
                "missing_modes": "",
            }
            for field in numeric_fields:
                average_row[field] = f"{statistics.mean(float(row[field]) for row in complete_rows):.3f}"
            writer.writerow(average_row)
    return path



# =============================================================================
# 实验计划：纯函数，可离线审阅；每个测试项独立记录，重试不增加样本数。
# =============================================================================
SUPPORTED_SCHEDULERS = (
    "default", "minrtt", "backup", "balanced", "rap", "act", "bandwidth",
    "fastprobe", "fastprobe5", "fastprobe10", "fastprobe20", "fastonly", "adaptive",
)
SUPPORTED_CC = ("default", "cubic", "bbr")


def validate_schedulers(value) -> List[str]:
    values = value.split(",") if isinstance(value, str) else list(value)
    if any(not isinstance(item, str) or not item.strip() for item in values):
        raise argparse.ArgumentTypeError("算法列表不能含空项")
    names = [item.strip().lower() for item in values]
    if len(set(names)) != len(names):
        raise argparse.ArgumentTypeError("算法列表不能重复")
    if any(name not in SUPPORTED_SCHEDULERS for name in names):
        raise argparse.ArgumentTypeError("不支持的调度算法；可选: " + ",".join(SUPPORTED_SCHEDULERS))
    return names


@dataclass(frozen=True)
class TestCase:
    case_id: str
    profile: str
    phase: str
    block_id: str
    round: int
    scheduler: str  # 数据发送端算法：GET服务端，POST客户端
    cc: str
    direction: str
    mode: str
    client_scheduler: str
    client_cc: str
    server_scheduler: str
    server_cc: str


def balanced_algorithm_order(profiles: Sequence[str], cycle: int) -> List[str]:
    """N轮循环移位，再N轮反向移位；三算法时覆盖全部六种排列。"""
    n = len(profiles)
    base = list(profiles)
    if ((cycle - 1) // n) % 2:
        base.reverse()
    offset = (cycle - 1) % n
    return base[offset:] + base[:offset]


def build_test_plan(args) -> List[TestCase]:
    directions = ("get", "post") if args.direction == "both" else (args.direction,)
    profiles = list(args.schedulers) if args.schedulers else ["legacy"]
    plan: List[TestCase] = []
    for phase, cycles in (("warmup", args.warmup_cycles), ("measured", args.iterations)):
        blocks = []
        if args.execution_mode == "sequential":
            blocks = [(p, r) for p in profiles for r in range(1, cycles + 1)]
        else:
            for r in range(1, cycles + 1):
                order = balanced_algorithm_order(profiles, r) if args.sequence == "balanced" else profiles
                blocks.extend((p, r) for p in order)
        for profile, cycle in blocks:
            reverse = args.sequence == "balanced" and cycle % 2 == 0
            direction_order = tuple(reversed(directions)) if reverse else directions
            mode_order = list(reversed(args.order)) if reverse else args.order
            block_id = f"{phase}_{profile}_r{cycle:04d}"
            for direction in direction_order:
                scheduler, cc = transfer_algorithms_for_direction(args, direction)
                if args.schedulers:
                    scheduler, cc = profile, args.cc or DEFAULT_CC
                client_scheduler = "adaptive" if direction == "get" else scheduler
                client_cc = cc
                server_scheduler = scheduler if direction == "get" else "adaptive"
                server_cc = "unchanged"  # 不增加或改变服务端拥塞控制参数。
                for mode in mode_order:
                    plan.append(TestCase(
                        f"{block_id}_{direction}_{mode}", profile, phase, block_id,
                        cycle, scheduler, cc, direction, mode,
                        client_scheduler, client_cc, server_scheduler, server_cc,
                    ))
    return plan


def case_metadata(case: TestCase, session: str) -> Dict[str, object]:
    return {"experiment_id": session, **asdict(case)}


def append_json_record(path: Path, record: Dict[str, object]) -> None:
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(record, ensure_ascii=False, allow_nan=False) + "\n")
        handle.flush()
        os.fsync(handle.fileno())


def write_experiment_plan(root: Path, session: str, args, plan: Sequence[TestCase]) -> None:
    # 白名单快照：不序列化argparse整个对象，避免输出密码和私钥路径。
    profiles = list(dict.fromkeys(c.profile for c in plan))
    position_counts = {p: [0] * len(profiles) for p in profiles}
    if args.execution_mode == "round-robin":
        cycle_orders: Dict[int, List[str]] = {}
        seen_blocks = set()
        for case in plan:
            if case.phase == "measured" and case.block_id not in seen_blocks:
                seen_blocks.add(case.block_id)
                cycle_orders.setdefault(case.round, []).append(case.profile)
        for order in cycle_orders.values():
            for position, profile in enumerate(order):
                position_counts[profile][position] += 1
        if args.sequence == "balanced" and any(len(set(v)) > 1 for v in position_counts.values()):
            LOG.info("轮数未覆盖完整的位置均衡；各算法位置次数=%s", position_counts)
    config = {
        "experiment_id": session, "schedulers": args.schedulers,
        "execution_mode": args.execution_mode, "sequence": args.sequence,
        "iterations_per_algorithm": args.iterations, "warmup_cycles": args.warmup_cycles,
        "direction": args.direction, "base_mode_order": args.order,
        "retries": args.retries, "cooldown_s": args.cooldown_s,
        "algorithm_cooldown_s": args.algorithm_cooldown_s,
        "verify_sha256": args.verify_sha256, "dry_run": args.dry_run,
        "scheduler_scope": "sender algorithm; GET server / POST client; receiver scheduler=adaptive",
        "client_timeout_ms": args.client_timeout_ms,
        "command_timeout_s": args.command_timeout_s,
        "board_resource_interval_s": args.board_resource_interval_s,
        "server_resource_interval_s": args.server_resource_interval_s,
        "server_hwm_scope": "server process lifetime; use RSS samples for per-case comparison",
        "metrics": EXPECTED_B_METRIC,
        "measured_cases": sum(c.phase == "measured" for c in plan),
        "warmup_cases": sum(c.phase == "warmup" for c in plan),
        "maximum_attempts": len(plan) * (args.retries + 1),
        "algorithm_position_counts": position_counts if args.execution_mode == "round-robin" else None,
    }
    (root / "experiment_config.json").write_text(
        json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8")
    fields = ["experiment_id", *TestCase.__dataclass_fields__]
    with (root / "test_plan.csv").open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(case_metadata(c, session) for c in plan)



def write_experiment_results(root, session, args, plan, records, started_cases):
    """显式保留未执行/中止项；按profile过滤后才调用原来的GX汇总。"""
    by_id = {r["case_id"]: r for r in records}
    fields = ["experiment_id", *TestCase.__dataclass_fields__, "status", "attempt",
              "success", "filesize", "time_ms", "mbps", "error"]
    with (root / "case_results.csv").open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        for case in plan:
            record = by_id.get(case.case_id)
            row = case_metadata(case, session)
            if record:
                row.update(record)
                row["status"] = "SUCCESS" if record["success"] else "FAILED"
            else:
                row["status"] = "ABORTED" if case.case_id in started_cases else "NOT_RUN"
            writer.writerow(row)
    global_rows = []
    for profile in dict.fromkeys(c.profile for c in plan):
        profile_root = root / profile
        profile_root.mkdir(exist_ok=True)
        measured = [r for r in records if r["profile"] == profile and r["phase"] == "measured"]
        directions = list(dict.fromkeys(c.direction for c in plan if c.profile == profile))
        summary = write_gx_final_summary(profile_root, session, measured, args.iterations, directions)
        for direction in directions:
            write_aggregation_summary(profile_root, session, measured, args.iterations, direction)
        with summary.open(encoding="utf-8-sig", newline="") as handle:
            for row in csv.DictReader(handle):
                matching = [c for c in plan if c.profile == profile and c.phase == "measured"
                            and c.direction == row["direction"] and DISPLAY_MODE_NAMES[c.mode] == row["mode"]]
                sample = matching[0]
                row.update(experiment_id=session, profile=profile, scheduler=sample.scheduler, cc=sample.cc)
                row["aborted"] = sum(c.case_id in started_cases and c.case_id not in by_id for c in matching)
                row["not_run"] = sum(c.case_id not in started_cases and c.case_id not in by_id for c in matching)
                global_rows.append(row)
    if global_rows:
        with (root / "algorithm_summary.csv").open("w", encoding="utf-8-sig", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(global_rows[0]))
            writer.writeheader()
            writer.writerows(global_rows)


def gx_main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_gx_args(argv)
    directions = ("get", "post") if args.direction == "both" else (args.direction,)
    configure_console_logging()
    started_at = datetime.now()
    session = f"{started_at:%Y%m%d_%H%M%S}_{uuid.uuid4().hex[:8]}"
    plan = build_test_plan(args)
    algorithm_names = "_".join(dict.fromkeys(case.scheduler for case in plan))
    dirname = f"{started_at:%m%d_%H%M%S}_{algorithm_names}"
    log_parent = Path(args.output_root).expanduser().resolve()
    log_parent.mkdir(parents=True, exist_ok=True)
    log_root = log_parent / dirname
    suffix = 2
    while True:
        try:
            log_root.mkdir()
            break
        except FileExistsError:
            log_root = log_parent / f"{dirname}_{suffix:02d}"
            suffix += 1
    board_resource_local_dir = log_root / "resources" / session
    raw_client_dir = log_root / "raw_client" / session
    server_temp_dir = log_root / ".server_resource_tmp" / session
    server_memory_csv = log_root / f"server_memory_{session}.csv"

    board = SSHRemote(
        hostname=args.board_ssh_host,
        username=args.board_ssh_user,
        password=args.board_ssh_password,
        key_filename=args.board_ssh_key,
        port=args.board_ssh_port,
        strict_host_key=args.strict_host_key,
        label="板卡",
    )
    server = SSHRemote(
        hostname=args.server_ssh_host,
        username=args.server_ssh_user,
        password=args.server_ssh_password,
        key_filename=args.server_ssh_key,
        port=args.server_ssh_port,
        strict_host_key=args.strict_host_key,
        label="云端",
    )

    remote_state_dir = f"/tmp/mini_api_gx_{session}"
    remote_pidfile = posixpath.join(remote_state_dir, "server.pid")
    server_pid: Optional[int] = None
    server_started = False
    get_expected_size = 0
    get_expected_sha = ""
    post_expected_size = 0
    post_expected_sha = ""
    args.planned_get_attempts = sum(c.direction == "get" for c in plan) * (args.retries + 1)
    write_experiment_plan(log_root, session, args, plan)
    LOG.info("实验计划: 正式=%s 预热=%s 最大尝试=%s，目录=%s",
             sum(c.phase == "measured" for c in plan),
             sum(c.phase == "warmup" for c in plan), len(plan) * (args.retries + 1), log_root)
    if args.dry_run:
        LOG.info("仅生成计划，未连接SSH。预览前%s项:", min(18, len(plan)))
        for case in plan[:18]:
            LOG.info("%s: client_scheduler=%s cc=%s server=%s", case.case_id,
                     case.client_scheduler, case.client_cc, case.server_scheduler)
        return 0
    records: List[Dict[str, object]] = []
    started_cases = set()
    last_block = None
    current_server_scheduler: Optional[str] = None
    remote_activity_started = False
    interrupted = False
    fatal_error = ""
    active_attempt_token: Optional[str] = None
    active_board_path: Optional[str] = None
    active_board_remove_file = False
    active_board_stop_file = ""
    active_server_monitor: Optional[Dict[str, object]] = None
    server_stop_confirmed = False

    try:
        connect_ssh_with_optional_prompt(server, "云端")
        connect_ssh_with_optional_prompt(board, "板卡")
        source_path, get_expected_size = server_preflight(
            server,
            args.server_dir,
            args.server_executable,
            args.remote_path,
            "get" in directions,
        )
        validate_server_scheduler_support(server, args)
        board_preflight(
            board,
            args,
            get_expected_size,
            session,
            args.verify_sha256,
            "post" in directions,
        )
        validate_server_resource_monitor(server, args)
        cleanup_stale_board_resource_files(board, args)
        cleanup_stale_server_resource_files(server, args)
        if "post" in directions:
            validate_server_post_targets(server, args)
            post_expected_size, post_expected_sha = get_board_post_source_info(
                board,
                args.post_local,
                args.verify_sha256,
            )
        if "get" in directions and args.verify_sha256:
            get_expected_sha = get_server_sha256(server, source_path)

        for case in plan:
            round_index, direction, mode = case.round, case.direction, case.mode
            scheduler, cc = case.scheduler, case.cc
            profile_root = log_root / case.profile
            if case.phase == "warmup":
                profile_root = profile_root / "warmup"
            board_resource_local_dir = profile_root / "resources" / session
            raw_client_dir = profile_root / "raw_client" / session
            server_memory_csv = profile_root / f"server_memory_{session}.csv"
            board_resource_local_dir.mkdir(parents=True, exist_ok=True)
            raw_client_dir.mkdir(parents=True, exist_ok=True)
            new_block = last_block != case.block_id
            if new_block:
                LOG.info("========== %s 周期 %s/%s 算法=%s ==========",
                         case.phase, round_index,
                         args.warmup_cycles if case.phase == "warmup" else args.iterations,
                         case.profile)
            if new_block and last_block is not None and args.algorithm_cooldown_s > 0:
                time.sleep(args.algorithm_cooldown_s)
            if server_started and current_server_scheduler != case.server_scheduler:
                if not stop_server(server, server_pid, args.server_executable, remote_pidfile, session):
                    raise RuntimeError("无法确认原服务端已停止，拒绝切换算法或开始传输")
                server_started = False
                server_pid = None
                server_stop_confirmed = True
            if not server_started:
                max_connections = len(plan) * (args.retries + 1) + 8
                server_stop_confirmed = False
                remote_activity_started = True
                # A lost SSH response may leave a launched process behind. Mark
                # it as potentially running before dispatch so final cleanup
                # retains its ownership file when startup cannot be confirmed.
                server_started = True
                server_pid = start_server_quiet(
                    server, args, max_connections, remote_pidfile, session,
                    scheduler=case.server_scheduler)
                current_server_scheduler = case.server_scheduler
                append_json_record(log_root / "server_starts.jsonl", {
                    **case_metadata(case, session), "server_pid": server_pid,
                    "started_at": datetime.now().isoformat(),
                    "verification": "startup_scheduler_and_listening_port",
                })
            last_block = case.block_id
            started_cases.add(case.case_id)
            display_mode = DISPLAY_MODE_NAMES[mode]
            successful_row: Optional[Dict[str, object]] = None
            successful_record: Optional[Dict[str, object]] = None
            first_start = ""
            last_end = ""

            for attempt in range(1, args.retries + 2):
                run_tag = (
                    f"api_{session}_{case.phase}_{case.profile}_r{round_index:03d}_{display_mode}_"
                    f"{direction}_i{round_index:03d}_a{attempt:02d}_{time.time_ns()}"
                )
                raw_stdout_path = raw_client_dir / f"{run_tag}.stdout.log"
                raw_stderr_path = raw_client_dir / f"{run_tag}.stderr.log"
                attempt_token = run_tag
                remote_path = (
                    args.remote_path
                    if direction == "get"
                    else post_remote_path_for_mode(args, mode)
                )
                board_path = (
                    f"get-{mode}-{session}-{case.phase}-{case.profile}-r{round_index:03d}-a{attempt:02d}.bin"
                    if direction == "get"
                    else args.post_local
                )
                remove_board_file = direction == "get"
                remote_board_csv = posixpath.join(
                    args.board_resource_dir,
                    f"{run_tag}_resource.csv",
                )
                remote_board_stop = f"/tmp/{run_tag}_resource.stop"
                remote_board_monitor_log = f"/tmp/{run_tag}_resource_monitor.log"
                local_board_csv = board_resource_local_dir / f"{run_tag}_resource.csv"

                if direction == "post" and not prepare_server_post_target(
                    server, args, remote_path
                ):
                    raise RuntimeError(
                        f"无法清理POST目标文件，拒绝开始测试: {remote_path}"
                    )

                LOG.info(
                    "开始测试: round=%s mode=%s direction=%s interfaces=%s "
                    "调度算法=%s 拥塞算法=%s attempt=%s/%s",
                    round_index,
                    display_mode,
                    direction,
                    ",".join(MODE_INTERFACES[mode]),
                    scheduler,
                    cc,
                    attempt,
                    args.retries + 1,
                )
                active_attempt_token = attempt_token
                active_board_path = board_path
                active_board_remove_file = remove_board_file
                active_board_stop_file = remote_board_stop
                start_dt = datetime.now()
                if not first_start:
                    first_start = start_dt.strftime("%Y-%m-%d %H:%M:%S")

                active_server_monitor = start_server_resource_monitor(
                    server,
                    args,
                    run_tag,
                    remote_pidfile,
                )
                execution_error = ""
                post_cleanup_ok = True
                command_started = time.monotonic()
                try:
                    command = build_client_command(
                        args,
                        mode,
                        direction,
                        board_path,
                        remote_path,
                        attempt_token,
                        True,
                        remote_board_csv,
                        remote_board_stop,
                        remote_board_monitor_log,
                        scheduler=case.client_scheduler,
                        cc=case.client_cc,
                    )
                    command_result = board.run(
                        command,
                        timeout_s=args.command_timeout_s,
                        output_path=raw_stdout_path,
                        stderr_path=raw_stderr_path,
                        max_capture_bytes=8 * 1024 * 1024,
                    )
                except Exception as exc:
                    execution_error = (
                        f"remote execution exception: {type(exc).__name__}: {exc}"
                    )
                    command_result = CommandResult(
                        output="",
                        exit_code=125,
                        controller_elapsed_s=time.monotonic() - command_started,
                    )
                end_dt = datetime.now()
                last_end = end_dt.strftime("%Y-%m-%d %H:%M:%S")

                server_stop_status = stop_server_resource_monitor(
                    server,
                    active_server_monitor,
                )
                server_resource = collect_server_resource(
                    server,
                    active_server_monitor,
                    server_stop_status,
                    server_memory_csv,
                    server_temp_dir,
                    run_tag,
                    mode,
                    direction,
                    round_index,
                    round_index,
                    attempt,
                )
                active_server_monitor = None

                if direction == "get" and args.verify_sha256:
                    try:
                        sha_result = board.run(
                            f"cd -- {q(args.board_dir)} && "
                            f"test -f {q(board_path)} && "
                            f"sha256sum {q(board_path)} "
                            "| awk '{printf \"__MINI_LOCAL_SHA256__=%s\\n\", $1}'",
                            timeout_s=max(120.0, args.command_timeout_s),
                        )
                        command_result.output += "\n" + sha_result.output
                        if sha_result.exit_code != 0 or sha_result.timed_out:
                            execution_error = "; ".join(
                                item for item in (
                                    execution_error,
                                    "local sha256 failed",
                                ) if item
                            )
                    except Exception as exc:
                        execution_error = "; ".join(
                            item for item in (
                                execution_error,
                                f"local sha256 exception: {type(exc).__name__}: {exc}",
                            ) if item
                        )
                elif direction == "post":
                    post_output, post_cleanup_ok = inspect_and_cleanup_server_post(
                        server,
                        args,
                        remote_path,
                        args.verify_sha256,
                    )
                    command_result.output += "\n" + post_output
                    if not post_cleanup_ok:
                        execution_error = "; ".join(
                            item for item in (
                                execution_error,
                                "server POST target cleanup could not be confirmed",
                            ) if item
                        )

                expected_size = (
                    get_expected_size if direction == "get" else post_expected_size
                )
                expected_sha = (
                    get_expected_sha if direction == "get" else post_expected_sha
                )
                success, parsed, error = parse_client_result(
                    command_result,
                    direction,
                    expected_size,
                    expected_sha,
                    args.verify_sha256,
                )
                if execution_error:
                    success = False
                    error = "; ".join(
                        item for item in (error, execution_error) if item
                    )

                cleanup_ok = False
                try:
                    cleanup_ok = cleanup_board_attempt(
                        board,
                        attempt_token,
                        args.board_dir,
                        board_path,
                        not remove_board_file,
                        remote_board_stop,
                    )
                except Exception:
                    cleanup_ok = False
                if not cleanup_ok:
                    try:
                        board.close()
                        connect_ssh_with_optional_prompt(board, "板卡")
                        cleanup_ok = cleanup_board_attempt(
                            board,
                            attempt_token,
                            args.board_dir,
                            board_path,
                            not remove_board_file,
                            remote_board_stop,
                        )
                    except Exception:
                        cleanup_ok = False

                board_monitor_state = extract_resource_status(
                    command_result.output,
                    True,
                )
                board_resource = collect_board_resource_gx(
                    board,
                    board_monitor_state,
                    remote_board_csv,
                    remote_board_stop,
                    remote_board_monitor_log,
                    local_board_csv,
                )

                if cleanup_ok:
                    active_attempt_token = None
                    active_board_path = None
                    active_board_remove_file = False
                    active_board_stop_file = ""
                else:
                    success = False
                    error = "; ".join(
                        item for item in (
                            error,
                            "board cleanup could not be confirmed",
                        ) if item
                    )

                attempt_record = {
                    **case_metadata(case, session), "attempt": attempt,
                    "success": success, "error": error, "start_time": start_dt.isoformat(),
                    "end_time": end_dt.isoformat(), "raw_stdout": str(raw_stdout_path),
                    "raw_stderr": str(raw_stderr_path), "parsed": parsed,
                    "board_resource": board_resource, "server_resource": server_resource,
                }
                append_json_record(log_root / "attempts.jsonl", attempt_record)
                if success:
                    successful_row = {
                        "iter": round_index,
                        "filesize": int(parsed["total_bytes"]),
                        "time_ms": float(parsed["transfer_elapsed_s"]) * 1000.0,
                        "mbps": float(parsed["throughput_mbps"]),
                        "mode": direction,
                        "start_time": start_dt.strftime("%Y-%m-%d %H:%M:%S"),
                        "end_time": end_dt.strftime("%Y-%m-%d %H:%M:%S"),
                    }
                    successful_record = {
                        "round": round_index,
                        "mode": mode,
                        "direction": direction,
                        "attempt": attempt,
                        "success": True,
                        "filesize": int(parsed["total_bytes"]),
                        "time_ms": float(parsed["transfer_elapsed_s"]) * 1000.0,
                        "mbps": float(parsed["throughput_mbps"]),
                        "board_resource": board_resource,
                        "server_resource": server_resource,
                    }
                    LOG.info(
                        "测试成功: mode=%s direction=%s filesize=%s time_ms=%.3f mbps=%.3f",
                        display_mode,
                        direction,
                        successful_row["filesize"],
                        successful_row["time_ms"],
                        successful_row["mbps"],
                    )
                    break

                LOG.error(
                    "测试失败: mode=%s direction=%s round=%s attempt=%s reason=%s",
                    display_mode,
                    direction,
                    round_index,
                    attempt,
                    error or "unknown error",
                )
                if not cleanup_ok:
                    raise RuntimeError("无法确认旧API客户端已终止，停止测试")
                if direction == "post" and not post_cleanup_ok:
                    raise RuntimeError("无法确认服务器POST文件已删除，停止测试")
                if attempt <= args.retries and args.cooldown_s > 0:
                    time.sleep(args.cooldown_s)

            write_speed_log(
                profile_root,
                display_mode,
                direction,
                round_index,
                successful_row,
                first_start,
                last_end,
            )
            record = successful_record or {
                "round": round_index, "mode": mode, "direction": direction,
                "attempt": attempt, "success": False, "filesize": "", "time_ms": "", "mbps": "",
                "board_resource": empty_resource_result("MISSING"),
                "server_resource": empty_server_resource_result("MISSING"),
            }
            record.update(case_metadata(case, session))
            record["error"] = "" if record["success"] else error
            records.append(record)
            append_json_record(log_root / "results.jsonl", record)
            if args.cooldown_s > 0:
                time.sleep(args.cooldown_s)

    except KeyboardInterrupt:
        interrupted = True
        LOG.warning("用户中断，开始清理")
    except Exception as exc:
        fatal_error = str(exc)
        LOG.exception("API压测终止: %s", exc)
    finally:
        if active_server_monitor is not None:
            try:
                stop_server_resource_monitor(server, active_server_monitor)
            except Exception:
                pass
        if active_attempt_token and active_board_path:
            try:
                cleanup_board_attempt(
                    board,
                    active_attempt_token,
                    args.board_dir,
                    active_board_path,
                    not active_board_remove_file,
                    active_board_stop_file,
                )
            except Exception:
                pass
        if server_started and server_pid:
            try:
                if not server.is_connected():
                    connect_ssh_with_optional_prompt(server, "云端")
                server_stop_confirmed = stop_server(
                    server,
                    server_pid,
                    args.server_executable,
                    remote_pidfile,
                    session,
                )
            except Exception as exc:
                LOG.warning("清理云端API服务端失败: %s", exc)

        try:
            if not board.is_connected():
                connect_ssh_with_optional_prompt(board, "板卡")
        except Exception as exc:
            LOG.warning("最终清理前无法重连主板: %s", exc)
        try:
            if not server.is_connected():
                connect_ssh_with_optional_prompt(server, "云端")
        except Exception as exc:
            LOG.warning("最终清理前无法重连云服务器: %s", exc)
        if remote_activity_started:
            cleanup_current_session_remote_artifacts(
                board,
                server,
                args,
                session,
                remote_state_dir,
                server_stop_confirmed or not server_started,
                directions,
            )

        try:
            board.close()
        except Exception:
            pass
        try:
            server.close()
        except Exception:
            pass

    server_temp_dir.mkdir(parents=True, exist_ok=True)
    try:
        for temp_file in server_temp_dir.glob("*"):
            temp_file.unlink(missing_ok=True)
        server_temp_dir.rmdir()
        server_temp_dir.parent.rmdir()
    except OSError:
        pass

    write_experiment_results(log_root, session, args, plan, records, started_cases)
    LOG.info("实验结果: %s", log_root)
    LOG.info("跨算法汇总: %s", log_root / "algorithm_summary.csv")

    if interrupted:
        return 130
    if fatal_error:
        return 2
    all_successful = all(bool(record["success"]) for record in records)
    expected_records = len(plan)
    return 0 if all_successful and len(records) == expected_records else 1




if __name__ == "__main__":
    sys.exit(gx_main())
