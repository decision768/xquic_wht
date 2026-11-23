#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Collect LTE/NR5G signal strength from one or more board serial consoles.

The board is expected to expose an already logged-in shell over the COM port.
For every board the script starts ``/oemapp/usr/bin/test_api`` and selects:

    15 (NW) -> 20 (INIT) -> 25 (GET_SIGNAL_STRENGTH)

Install the only third-party dependency with::

    python -m pip install pyserial

Example::

    python tests/collect_modem_signal.py \
        --duration 30m --output signal_strength.csv

The default interval is one second. ``--duration 0`` keeps sampling until
Ctrl+C. Signal values are recorded exactly as reported by test_api; in
particular, NR5G SNR is not scaled by this script. By default, 5G1 uses COM10
and 5G2 uses COM11; one or more ``--device`` arguments override that mapping.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Sequence, TextIO, Tuple

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # Keep --help usable before pyserial is installed.
    serial = None
    list_ports = None


# =============================================================================
# 用户常用配置区
# =============================================================================
# 支持纯秒数或 s/m/h 后缀，例如 "30s" 、"30m"、"1h"；"0" 表示持续采集。
DEFAULT_COLLECTION_DURATION = "5h"
# 两轮采样开始时间的间隔，1.0 表示每秒采集一轮。
DEFAULT_SAMPLE_INTERVAL_SECONDS = 0.5

DEFAULT_DEVICE_PORTS = (
    ("5G1", "COM10"),
    ("5G2", "COM11"),
)
DEFAULT_BAUDRATE = 115200
# =============================================================================
# 用户常用配置区结束
# =============================================================================


TEST_API_COMMAND = "cd /oemapp/usr/bin && ./test_api"
MAIN_PROMPT_RE = re.compile(r"Option\s*>")
NW_PROMPT_RE = re.compile(r"input\s+SUBID\s*:", re.IGNORECASE)
LEVEL_RE = re.compile(r"(?m)^\s*level\s*=\s*(-?\d+)\s*$", re.IGNORECASE)
TECH_LINE_RE = re.compile(
    r"(?m)^\s*(gsm|wcdma|lte|nr5g)\s*=\s*(-?\d+)\s*([^\r\n]*)\r?$",
    re.IGNORECASE,
)
METRIC_RE = re.compile(
    r"\b(rssi|rsrp|rsrq|snr|ecio)\s*=\s*(-?\d+)", re.IGNORECASE
)
DURATION_RE = re.compile(r"^\s*(\d+(?:\.\d+)?)\s*([smh]?)\s*$", re.IGNORECASE)

CSV_FIELDS = (
    "timestamp_utc",
    "sample_index",
    "device",
    "port",
    "success",
    "error",
    "level",
    "gsm",
    "gsm_rssi",
    "wcdma",
    "wcdma_rssi",
    "wcdma_ecio",
    "lte",
    "lte_rssi",
    "lte_rsrp",
    "lte_rsrq",
    "lte_snr",
    "nr5g",
    "nr5g_rsrp",
    "nr5g_rsrq",
    "nr5g_snr",
)


class SignalCollectionError(RuntimeError):
    """Raised when the serial console or test_api response is unusable."""


@dataclass(frozen=True)
class DeviceSpec:
    name: str
    port: str


DEFAULT_DEVICES = (
    *(DeviceSpec(name=name, port=port) for name, port in DEFAULT_DEVICE_PORTS),
)


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def parse_device(value: str) -> DeviceSpec:
    if "=" not in value:
        raise argparse.ArgumentTypeError(
            f"设备格式错误: {value!r}，应为 NAME=COM_PORT，例如 5G1=COM5"
        )
    name, port = (part.strip() for part in value.split("=", 1))
    if not name or not port:
        raise argparse.ArgumentTypeError(
            f"设备格式错误: {value!r}，名称和串口均不能为空"
        )
    return DeviceSpec(name=name, port=port)


def parse_duration(value: str) -> float:
    """Parse seconds or a duration with an s/m/h suffix; zero means unlimited."""
    match = DURATION_RE.fullmatch(value)
    if match is None:
        raise argparse.ArgumentTypeError(
            f"时长格式错误: {value!r}，可使用 30s、30m、1h 或纯秒数"
        )
    amount = float(match.group(1))
    multiplier = {"": 1.0, "s": 1.0, "m": 60.0, "h": 3600.0}[
        match.group(2).lower()
    ]
    return amount * multiplier


def parse_signal_response(text: str) -> Dict[str, int]:
    """Parse one GET_SIGNAL_STRENGTH response without changing units."""
    level_match = LEVEL_RE.search(text)
    if level_match is None:
        raise SignalCollectionError("返回内容中缺少 level")

    values: Dict[str, int] = {"level": int(level_match.group(1))}
    found_technologies = set()
    for match in TECH_LINE_RE.finditer(text):
        technology = match.group(1).lower()
        found_technologies.add(technology)
        values[technology] = int(match.group(2))
        for metric, raw_value in METRIC_RE.findall(match.group(3)):
            values[f"{technology}_{metric.lower()}"] = int(raw_value)

    missing = {"lte", "nr5g"} - found_technologies
    if missing:
        raise SignalCollectionError(
            "返回内容中缺少信号行: " + ", ".join(sorted(missing))
        )
    return values


class BoardSession:
    def __init__(
        self,
        spec: DeviceSpec,
        baudrate: int,
        command_timeout: float,
        line_ending: bytes,
        raw_log: Optional[TextIO],
        raw_log_lock: threading.Lock,
    ) -> None:
        self.spec = spec
        self.baudrate = baudrate
        self.command_timeout = command_timeout
        self.line_ending = line_ending
        self.raw_log = raw_log
        self.raw_log_lock = raw_log_lock
        self.connection = None
        self.receive_buffer = ""
        self.started = False

    def open(self) -> None:
        if serial is None:
            raise SignalCollectionError(
                "缺少 pyserial，请先执行: python -m pip install pyserial"
            )
        try:
            self.connection = serial.Serial(
                port=self.spec.port,
                baudrate=self.baudrate,
                timeout=0.1,
                write_timeout=2.0,
            )
        except Exception as exc:
            raise SignalCollectionError(
                f"无法打开串口 {self.spec.port}: {exc}"
            ) from exc
        self.receive_buffer = ""
        self.started = False
        self.connection.reset_input_buffer()
        self.connection.reset_output_buffer()

    def close(self) -> None:
        connection = self.connection
        self.connection = None
        if connection is None:
            return
        if self.started:
            try:
                connection.write(b"e" + self.line_ending)
                connection.flush()
            except Exception:
                pass
        try:
            connection.close()
        finally:
            self.started = False
            self.receive_buffer = ""

    def _write(self, data: bytes) -> None:
        if self.connection is None:
            raise SignalCollectionError(f"{self.spec.name} 串口尚未打开")
        try:
            self.connection.write(data)
            self.connection.flush()
        except Exception as exc:
            raise SignalCollectionError(
                f"{self.spec.name} 写串口失败: {exc}"
            ) from exc

    def _send_line(self, value: str) -> None:
        self._write(value.encode("utf-8") + self.line_ending)

    def _discard_until_quiet(self, quiet_seconds: float = 0.25) -> None:
        if self.connection is None:
            return
        quiet_deadline = time.monotonic() + quiet_seconds
        while time.monotonic() < quiet_deadline:
            waiting = self.connection.in_waiting
            if waiting:
                self.connection.read(waiting)
                quiet_deadline = time.monotonic() + quiet_seconds
            else:
                time.sleep(0.02)
        self.receive_buffer = ""

    def _expect(self, pattern: re.Pattern[str], description: str) -> str:
        if self.connection is None:
            raise SignalCollectionError(f"{self.spec.name} 串口尚未打开")

        deadline = time.monotonic() + self.command_timeout
        while time.monotonic() < deadline:
            match = pattern.search(self.receive_buffer)
            if match is not None:
                consumed = self.receive_buffer[: match.end()]
                self.receive_buffer = self.receive_buffer[match.end() :]
                return consumed

            try:
                chunk = self.connection.read(max(1, self.connection.in_waiting))
            except Exception as exc:
                raise SignalCollectionError(
                    f"{self.spec.name} 读串口失败: {exc}"
                ) from exc
            if chunk:
                self.receive_buffer += chunk.decode("utf-8", errors="replace")
                if len(self.receive_buffer) > 1024 * 1024:
                    self.receive_buffer = self.receive_buffer[-512 * 1024 :]

        tail = self.receive_buffer[-500:].replace("\r", "\\r").replace(
            "\n", "\\n"
        )
        raise SignalCollectionError(
            f"{self.spec.name} 等待{description}超时，最近输出: {tail!r}"
        )

    def _record_raw(self, timestamp: str, text: str) -> None:
        if self.raw_log is None:
            return
        with self.raw_log_lock:
            self.raw_log.write(
                f"\n===== {timestamp} {self.spec.name} {self.spec.port} =====\n"
            )
            self.raw_log.write(text)
            if not text.endswith("\n"):
                self.raw_log.write("\n")
            self.raw_log.flush()

    def start(self) -> None:
        if self.connection is None:
            self.open()

        # Return an accidentally running test_api to the shell before starting
        # a known session. The board console is expected to be logged in.
        self._write(b"\x03" + self.line_ending)
        self._discard_until_quiet()
        self._send_line(TEST_API_COMMAND)
        self._expect(MAIN_PROMPT_RE, "test_api 主菜单")

        self._send_line("15")
        self._expect(NW_PROMPT_RE, "NW 子菜单")

        self._send_line("20")
        init_text = self._expect(NW_PROMPT_RE, "NW INIT 结果")
        if not re.search(r"(?m)^\s*OK\s*$", init_text):
            raise SignalCollectionError(
                f"{self.spec.name} 执行 NW INIT 后未返回 OK"
            )
        self.started = True

    def collect(self) -> Tuple[str, Dict[str, int], str]:
        if not self.started:
            self.start()
        timestamp = utc_timestamp()
        self._send_line("25")
        response = self._expect(NW_PROMPT_RE, "GET_SIGNAL_STRENGTH 结果")
        self._record_raw(timestamp, response)
        if not re.search(r"(?m)^\s*OK\s*$", response):
            raise SignalCollectionError(
                f"{self.spec.name} 查询信号强度后未返回 OK"
            )
        return timestamp, parse_signal_response(response), response


def empty_result(spec: DeviceSpec, timestamp: str) -> Dict[str, object]:
    row: Dict[str, object] = {field: "" for field in CSV_FIELDS}
    row.update(
        {
            "timestamp_utc": timestamp,
            "device": spec.name,
            "port": spec.port,
            "success": 0,
        }
    )
    return row


def collect_one(session: BoardSession) -> Dict[str, object]:
    try:
        timestamp, values, _response = session.collect()
        row = empty_result(session.spec, timestamp)
        row.update(values)
        row["success"] = 1
        return row
    except Exception as exc:
        # A fresh test_api session will be established on the next sample.
        session.close()
        row = empty_result(session.spec, utc_timestamp())
        row["error"] = str(exc)
        return row


def print_result(row: Dict[str, object]) -> None:
    if row["success"]:
        output = {
            "timestamp_utc": row["timestamp_utc"],
            "sample_index": row["sample_index"],
            "device": row["device"],
            "port": row["port"],
            "level": row["level"],
            "lte": {
                "available": row["lte"],
                "rssi": row["lte_rssi"],
                "rsrp": row["lte_rsrp"],
                "rsrq": row["lte_rsrq"],
                "snr": row["lte_snr"],
            },
            "nr5g": {
                "available": row["nr5g"],
                "rsrp": row["nr5g_rsrp"],
                "rsrq": row["nr5g_rsrq"],
                "snr": row["nr5g_snr"],
            },
        }
        print(json.dumps(output, ensure_ascii=False), flush=True)
    else:
        print(
            f"[{row['timestamp_utc']}] {row['device']} ({row['port']}) "
            f"采集失败: {row['error']}",
            file=sys.stderr,
            flush=True,
        )


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="通过板卡 COM 串口驱动 test_api，采集 LTE/NR5G 信号强度",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--device",
        action="append",
        type=parse_device,
        metavar="NAME=COM_PORT",
        help=(
            "板卡名称和串口；可重复；不传时默认使用 "
            "5G1=COM10 和 5G2=COM11"
        ),
    )
    parser.add_argument(
        "--baudrate", type=int, default=DEFAULT_BAUDRATE, help="串口波特率"
    )
    parser.add_argument(
        "--line-ending",
        choices=("cr", "lf", "crlf"),
        default="cr",
        help="向板卡发送命令时使用的行结束符",
    )
    parser.add_argument(
        "--command-timeout",
        type=float,
        default=15.0,
        help="每个菜单操作的超时时间（秒）",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=DEFAULT_SAMPLE_INTERVAL_SECONDS,
        help="两轮采样开始时间的间隔（秒）",
    )
    parser.add_argument(
        "--duration",
        type=parse_duration,
        default=DEFAULT_COLLECTION_DURATION,
        metavar="DURATION",
        help="总采集时长，支持 s/m/h 后缀；0 表示持续到 Ctrl+C",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("signal_strength.csv"),
        help="CSV 输出文件",
    )
    parser.add_argument(
        "--raw-log",
        type=Path,
        help="可选：把每次 test_api 原始返回同时追加到此文件",
    )
    parser.add_argument(
        "--list-ports", action="store_true", help="列出当前串口后退出"
    )
    return parser


def validate_args(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if args.list_ports:
        return
    if not args.device:
        args.device = list(DEFAULT_DEVICES)
    names = [spec.name for spec in args.device]
    ports = [spec.port.casefold() for spec in args.device]
    if len(names) != len(set(names)):
        parser.error("--device 名称不能重复")
    if len(ports) != len(set(ports)):
        parser.error("同一个 COM 口不能配置给多个设备")
    if args.baudrate <= 0:
        parser.error("--baudrate 必须大于 0")
    if args.command_timeout <= 0:
        parser.error("--command-timeout 必须大于 0")
    if args.interval < 0:
        parser.error("--interval 不能小于 0")


def show_ports() -> int:
    if list_ports is None:
        print("缺少 pyserial，请先执行: python -m pip install pyserial", file=sys.stderr)
        return 2
    ports = sorted(list_ports.comports(), key=lambda item: item.device.casefold())
    if not ports:
        print("未发现串口")
        return 0
    for port in ports:
        print(f"{port.device}\t{port.description}\t{port.hwid}")
    return 0


def run(args: argparse.Namespace) -> int:
    if serial is None:
        print("缺少 pyserial，请先执行: python -m pip install pyserial", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    raw_log: Optional[TextIO] = None
    if args.raw_log is not None:
        args.raw_log.parent.mkdir(parents=True, exist_ok=True)
        raw_log = args.raw_log.open("a", encoding="utf-8", newline="")

    line_endings = {"cr": b"\r", "lf": b"\n", "crlf": b"\r\n"}
    raw_log_lock = threading.Lock()
    sessions = [
        BoardSession(
            spec=spec,
            baudrate=args.baudrate,
            command_timeout=args.command_timeout,
            line_ending=line_endings[args.line_ending],
            raw_log=raw_log,
            raw_log_lock=raw_log_lock,
        )
        for spec in args.device
    ]

    had_failure = False
    completed_samples = 0
    interrupted = False
    try:
        with args.output.open("w", encoding="utf-8-sig", newline="") as output_file:
            writer = csv.DictWriter(output_file, fieldnames=CSV_FIELDS)
            writer.writeheader()
            output_file.flush()

            with ThreadPoolExecutor(max_workers=len(sessions)) as executor:
                collection_started_at = time.monotonic()
                collection_deadline = (
                    collection_started_at + args.duration
                    if args.duration > 0
                    else None
                )
                next_sample_at = collection_started_at
                while True:
                    if (
                        collection_deadline is not None
                        and completed_samples > 0
                        and next_sample_at >= collection_deadline
                    ):
                        break
                    delay = next_sample_at - time.monotonic()
                    if delay > 0:
                        time.sleep(delay)

                    if (
                        collection_deadline is not None
                        and completed_samples > 0
                        and time.monotonic() >= collection_deadline
                    ):
                        break

                    rows = list(executor.map(collect_one, sessions))
                    for row in rows:
                        row["sample_index"] = completed_samples + 1
                        writer.writerow(row)
                        print_result(row)
                        had_failure = had_failure or not bool(row["success"])
                    output_file.flush()

                    completed_samples += 1
                    next_sample_at += args.interval
                    if next_sample_at < time.monotonic():
                        next_sample_at = time.monotonic()
    except KeyboardInterrupt:
        interrupted = True
        print("\n收到 Ctrl+C，停止采集。", file=sys.stderr)
    finally:
        for session in sessions:
            session.close()
        if raw_log is not None:
            raw_log.close()

    if interrupted:
        return 0
    return 1 if had_failure else 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    validate_args(parser, args)
    if args.list_ports:
        return show_ports()
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
