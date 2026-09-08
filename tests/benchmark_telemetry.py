#!/usr/bin/env python3
"""Optional observations for 压测脚本.py; never changes a transfer verdict.

Raw JSONL is the source of truth. Rebuild an interrupted run without hardware:
    python benchmark_telemetry.py PATH/TO/events.jsonl
"""
from __future__ import annotations

import argparse
import bisect
import json
import logging
import math
import os
import posixpath
import re
import shlex
import socket
import statistics
import threading
import time
from datetime import datetime, timezone
from pathlib import Path

LOG = logging.getLogger("mini_api_get_post_benchmark")
MODULES = {"rmnet_data0": "5G1", "usb0": "5G2"}
SUBMENU = re.compile(r"input\s+SUBID\s*:", re.I)
MAIN_MENU = re.compile(r"Option\s*>", re.I)
QUERY_FRAME = re.compile(r"test_nw_get_signal_strength\s*\(\s*\)[\s\S]*?input\s+SUBID\s*:", re.I)
INIT_FRAME = re.compile(r"test_nw_init\s*\(\s*\)[\s\S]*?input\s+SUBID\s*:", re.I)
SHELL = re.compile(r"^(?:[^\r\n]*[~/][^\r\n]*|[\w.@:-]+)\s*[#$][ \t]*$", re.M)
LOGIN = re.compile(r"(?:login:|Password:)[ \t]*$", re.I)
ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
TRANSPORT_SCHEMA_VERSION = 1
UNSUPPORTED = "旧结束快照不包含周期CWND、InP和流控数据"


def add_arguments(parser, defaults=None):
    defaults = defaults or {}
    group = parser.add_argument_group("双模块实验采集（失败不改变传输结果）")
    for flag in ("telemetry", "signal-5g1", "signal-5g2", "network-monitor"):
        group.add_argument("--disable-" + flag, action="store_true")
    for name, default in (("serial-port", "COM11"), ("serial-login-user", "root"),
                          ("signal-api", "/oemapp/usr/bin/test_api")):
        group.add_argument("--" + name, default=defaults.get(name, default))
    group.add_argument("--serial-baudrate", type=int, default=defaults.get("serial-baudrate", 115200))
    group.add_argument("--serial-login-password", default=os.environ.get("MINI_BENCH_SERIAL_PASSWORD", ""),
                       help="仅串口出现密码提示时使用；建议通过 MINI_BENCH_SERIAL_PASSWORD 提供")
    for name, default in (("signal-interval-s", 1.0), ("network-interval-s", 0.5),
                          ("signal-timeout-s", 5.0)):
        group.add_argument("--" + name, type=float, default=defaults.get(name, default))
    group.add_argument("--transport-telemetry-interval-ms", type=int,
                       default=defaults.get("transport-telemetry-interval-ms", 250))
    group.add_argument("--excel-interval-s", type=float,
                       default=defaults.get("excel-interval-s", 1.0))


def validate_arguments(parser, args):
    for name in ("signal_interval_s", "network_interval_s", "signal_timeout_s"):
        value = getattr(args, name)
        if not math.isfinite(value) or value <= 0:
            parser.error(f"--{name.replace('_', '-')}必须是有限正数")
    if args.serial_baudrate <= 0:
        parser.error("--serial-baudrate必须为正数")
    if args.transport_telemetry_interval_ms <= 0:
        parser.error("--transport-telemetry-interval-ms必须为正数")
    if not math.isfinite(args.excel_interval_s) or args.excel_interval_s <= 0:
        parser.error("--excel-interval-s必须是有限正数")
    if not args.signal_api.startswith("/") or "\n" in args.signal_api:
        parser.error("--signal-api必须为单行绝对路径")


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds")


def parse_signal(text):
    """Only a complete response to GET_SIGNAL_STRENGTH is a measurement."""
    text = ANSI.sub("", text).replace("\r", "")
    marker = re.search(r"test_nw_get_signal_strength\s*\(\s*\)", text)
    if not marker:
        raise ValueError("缺少信号查询响应标识")
    text = text[marker.end():]
    if re.search(r"\b(?:ERROR|FAIL)\b", text, re.I) or not re.search(r"(?m)^\s*OK\s*$", text):
        raise ValueError("信号查询未返回 OK 或返回错误")
    values = {}
    level = re.search(r"\blevel=(-?\d+)\b", text)
    if level:
        values["level"] = int(level[1])
    for rat in ("lte", "nr5g"):
        match = re.search(rf"(?m)^\s*{rat}=(\d+)\b([^\n]*)", text)
        if not match:
            raise ValueError(f"缺少 {rat} 响应行")
        valid = int(match[1])
        if valid not in (0, 1):
            raise ValueError(f"未知 {rat} 有效标志 {valid}")
        values[f"{rat}_valid"] = valid
        fields = dict(re.findall(r"\b(rssi|rsrp|rsrq|snr)=(-?\d+)\b", match[2]))
        required = {"rsrp", "rsrq", "snr"} | ({"rssi"} if rat == "lte" else set())
        if valid and not required.issubset(fields):
            raise ValueError(f"{rat} 有效行字段不完整")
        for field in ("rsrp", "rsrq", "rssi", "snr"):
            name = "snr_raw" if field == "snr" else field
            values[f"{rat}_{name}"] = int(fields[field]) if valid and field in fields else None
        values[f"{rat}_snr_db"] = None
    return values


def path_snapshots(text):
    """Parse actual end snapshots only; never invent per-path inflight."""
    connection_inflight = None
    result = []
    for line in text.splitlines():
        if line.startswith("connection multipath="):
            m = re.search(r"\binflight=(\d+)", line)
            connection_inflight = int(m[1]) if m else None
        if not re.match(r"^path\[\d+\] ", line):
            continue
        fields = dict(re.findall(r"\b(\w+)=([^\s]*)", line))
        row = {"snapshot_type": "client_end_snapshot", "endpoint": "board",
               "module": MODULES.get(fields.get("iface"), "UNKNOWN"),
               "interface": fields.get("iface"), "local": fields.get("local"),
               "peer": fields.get("peer"), "connection_inflight_bytes": connection_inflight,
               "unsupported_reason": UNSUPPORTED}
        for field, name in (("id", "path_id"), ("state", "path_state"), ("app", "app_status"),
                            ("tx", "tx_bytes"), ("rx", "rx_bytes"), ("srtt_us", "srtt_us")):
            value = fields.get(field, "")
            row[name] = int(value) if value.isdigit() else None
        if row["srtt_us"] == 0:
            row["srtt_us"] = None
        result.append(row)
    return result


class NetRates:
    def __init__(self):
        self.previous = {}

    def sample(self, boot, iface, index, begin, end, tx, rx):
        row = dict(interface=iface, module=MODULES.get(iface, "UNKNOWN"), boot_id=boot,
                   ifindex=index, source_begin_s=begin, source_end_s=end,
                   tx_bytes=tx, rx_bytes=rx, interval_s=None, tx_mbps=None, rx_mbps=None)
        previous = self.previous.get(iface)
        self.previous[iface] = (boot, index, begin, end, tx, rx)
        row["status"] = "BASELINE"
        if previous:
            pb, pi, pbegin, pend, ptx, prx = previous
            dt = (begin + end - pbegin - pend) / 2
            if boot != pb or index != pi or dt <= 0 or tx < ptx or rx < prx:
                row["status"] = "COUNTER_RESET"
            else:
                row.update(status="OK", interval_s=dt, rate_source_begin_s=pbegin,
                           tx_mbps=(tx - ptx) * 8 / dt / 1e6,
                           rx_mbps=(rx - prx) * 8 / dt / 1e6)
        return row


class Stopped(Exception):
    pass


class ChannelWire:
    def __init__(self, board, command):
        self.channel = None
        try:
            self.channel = board.client.get_transport().open_session(timeout=5)
            self.channel.settimeout(5)
            self.channel.get_pty()
            self.channel.set_combine_stderr(True)
            self.channel.exec_command(command)
            self.channel.settimeout(0.2)
        except Exception:
            self.close()
            raise

    def read(self):
        try:
            chunk = self.channel.recv(8192)
        except socket.timeout:
            return b""
        if not chunk:
            raise ConnectionError("SSH 采集通道已关闭")
        return chunk

    def write(self, data):
        self.channel.sendall(data)

    def close(self):
        if self.channel is not None:
            self.channel.close()


class SerialWire:
    def __init__(self, args):
        import serial
        self.serial = serial.Serial(port=None, baudrate=args.serial_baudrate, bytesize=8,
                                    parity="N", stopbits=1, timeout=0.2, write_timeout=2,
                                    xonxoff=False, rtscts=False, dsrdtr=False)
        self.serial.dtr = False
        self.serial.rts = False
        self.serial.port = args.serial_port
        self.serial.open()

    def read(self):
        return self.serial.read(min(8192, self.serial.in_waiting) or 1)

    def write(self, data):
        self.serial.write(data)

    def close(self):
        self.serial.close()


class PromptReader:
    def __init__(self, wire, stop, raw):
        self.wire, self.stop, self.raw = wire, stop, raw
        self.buffer = ""
        self.sensitive = False

    def until(self, pattern, timeout):
        deadline = time.monotonic() + timeout
        while True:
            if self.stop.is_set():
                raise Stopped()
            match = pattern.search(self.buffer)
            if match:
                response, self.buffer = self.buffer[:match.end()], self.buffer[match.end():]
                return response
            if time.monotonic() >= deadline:
                # Keep the partial response for explicit resynchronization.
                raise TimeoutError("等待终端响应超时")
            chunk = self.wire.read()
            if chunk:
                text = chunk.decode("utf-8", errors="replace")
                self.raw("[AUTH_OUTPUT_OMITTED]" if self.sensitive else text)
                self.buffer += ANSI.sub("", text).replace("\r", "")
                if len(self.buffer) > 1024 * 1024:
                    raise ValueError("终端输出超过 1 MiB 且没有预期提示符")

    def send(self, command):
        self.wire.write((command + "\r\n").encode("utf-8"))


def signal_loop(wire, args, module, stop, emit, ready, serial_login=False):
    reader = PromptReader(wire, stop, lambda text: emit("raw", source=module, text=text))
    timeout = args.signal_timeout_s
    owns_api = False
    synchronized = False
    try:
        if serial_login:
            reader.send("")
            response = reader.until(re.compile(f"(?:{SHELL.pattern})|(?:{LOGIN.pattern})", re.M | re.I), timeout)
            if response.rstrip().lower().endswith("login:"):
                reader.send(args.serial_login_user)
                response = reader.until(re.compile(f"(?:{SHELL.pattern})|(?:{LOGIN.pattern})", re.M | re.I), timeout)
            if response.rstrip().lower().endswith("password:"):
                if not args.serial_login_password:
                    raise ValueError("串口需要登录密码；未设置 MINI_BENCH_SERIAL_PASSWORD")
                reader.sensitive = True
                try:
                    reader.send(args.serial_login_password)
                    response = reader.until(SHELL, timeout)
                finally:
                    reader.sensitive = False
            if not SHELL.search(response):
                raise ValueError("串口未处于可识别的 shell")
            reader.send("printf '__GX_SERIAL_READY__\\n'")
            reader.until(re.compile(r"(?m)^__GX_SERIAL_READY__\n"), timeout)
            reader.send(f"cd {shlex.quote(posixpath.dirname(args.signal_api))} && {shlex.quote(args.signal_api)}")
        reader.until(MAIN_MENU, timeout)
        owns_api = True
        reader.send("15")
        reader.until(SUBMENU, timeout)
        reader.send("20")
        response = reader.until(INIT_FRAME, timeout)
        if not re.search(r"(?m)^\s*OK\s*$", response) or re.search(r"\bERROR\b", response):
            raise ValueError("NW 初始化未成功")
        synchronized = True
        emit("collector", source=module, status="READY")
        ready.set()
        while not stop.is_set():
            started = time.monotonic()
            start_utc = utc_now()
            reader.send("25")
            synchronized = False
            try:
                response = reader.until(QUERY_FRAME, timeout)
                synchronized = True
                values = parse_signal(response)
                emit("signal", source=module, start=started, end=time.monotonic(),
                     query_start_utc=start_utc, query_end_utc=utc_now(), status="OK", **values)
            except (TimeoutError, ValueError) as exc:
                emit("signal", source=module, start=started, end=time.monotonic(),
                     query_start_utc=start_utc, query_end_utc=utc_now(), status="QUERY_FAILED", reason=str(exc))
                if not synchronized:
                    # Drain the *old* reply to its menu before sending anything new.
                    reader.until(QUERY_FRAME, timeout)
                    synchronized = True
                    emit("collector", source=module, status="RESYNCHRONIZED")
            stop.wait(max(0, args.signal_interval_s - (time.monotonic() - started)))
    finally:
        if owns_api:
            try:
                # Only our test_api receives this exit command, never an unknown shell.
                reader.send("e")
                deadline = time.monotonic() + 1
                while time.monotonic() < deadline:
                    chunk = wire.read()
                    if chunk and SHELL.search(chunk.decode("utf-8", errors="replace")):
                        break
            except Exception:
                pass
        wire.close()


def network_loop(wire, interval, stop, emit, started, ready):
    reader = PromptReader(wire, stop, lambda text: emit("raw", source="network", text=text))
    rates = NetRates()
    try:
        line = reader.until(re.compile(r"GX_CLOCK [^\r\n]+\n"), 10)
        fields = re.search(r"GX_CLOCK (\S+) ([\d.]+)", line)
        if not fields:
            raise ValueError("无法建立主板 uptime 时间映射")
        received = time.monotonic()
        offset = (started + received) / 2 - float(fields[2])
        uncertainty = (received - started) / 2 + 0.01
        emit("clock", source="network", offset_s=offset, uncertainty_s=uncertainty,
             method="SSH启动请求/首个uptime响应的中点估计；不含后续时钟漂移", boot_id=fields[1])
        emit("collector", source="network", status="READY")
        ready.set()
        while not stop.is_set():
            line = reader.until(re.compile(r"\n"), max(5, interval * 4)).strip()
            fields = line.split()
            if fields and fields[0] == "GX_MISSING" and len(fields) == 3:
                rates.previous.pop(fields[1], None)
                t = float(fields[2]) + offset
                emit("network", interface=fields[1], module=MODULES.get(fields[1]),
                     start=t - uncertainty, end=t + uncertainty, status="INTERFACE_MISSING")
            elif fields and fields[0] == "GX_NET" and len(fields) == 8:
                _, boot, iface, index, begin, end, tx, rx = fields
                row = rates.sample(boot, iface, int(index), float(begin), float(end), int(tx), int(rx))
                begin_time = row.get("rate_source_begin_s", row["source_begin_s"])
                emit("network", start=begin_time + offset - uncertainty,
                     end=row["source_end_s"] + offset + uncertainty,
                     received_utc=utc_now(), clock_uncertainty_s=uncertainty, **row)
    finally:
        try:
            wire.write(b"\x03")  # Dedicated, owned monitor PTY only.
        except Exception:
            pass
        wire.close()


class TelemetrySession:
    def __init__(self, args, session, log_root):
        self.args, self.session = args, session
        self.root = Path(log_root) / "telemetry" / session
        self.root.mkdir(parents=True, exist_ok=True)
        self.journal = self.root / "events.jsonl"
        self.file = self.journal.open("a", encoding="utf-8", buffering=1)
        self.lock, self.stop = threading.Lock(), threading.Event()
        self.threads = []
        self.attempts = {}
        self.completed = set()
        self.secrets = [str(v) for k, v in vars(args).items() if "password" in k and v]
        self.emit("config", values={k: v for k, v in vars(args).items()
                                    if not any(x in k.lower() for x in ("password", "passphrase", "secret"))})
        self.emit("definition",
                  inp="当前仍由拥塞控制计为在途的ACK-eliciting QUIC包数",
                  swnd="min(连接剩余额度, 活动双向发送流剩余额度之和)，连接共享",
                  signal_units="RSRP/RSRQ/RSSI及SNR均保存test_api原始值；SNR不换算",
                  rate_basis="路径速度含重传；网卡含其他流量；GET first_body_to_last_body；POST app_submit_not_remote_receive")

    def emit(self, kind, **data):
        record = dict(kind=kind, session=self.session, recorded_utc=utc_now(),
                      recorded_mono=time.monotonic(), **data)
        # Never persist authentication secrets, including terminal echo.
        text = json.dumps(record, ensure_ascii=False, default=str)
        for secret in self.secrets:
            text = text.replace(json.dumps(secret, ensure_ascii=False)[1:-1], "[REDACTED]")
        with self.lock:
            self.file.write(text + "\n")
            self.file.flush()

    def _worker(self, source, factory, ready, network=False):
        wire = None
        try:
            started = time.monotonic()
            wire = factory()
            if network:
                network_loop(wire, self.args.network_interval_s, self.stop, self.emit, started, ready)
            else:
                signal_loop(wire, self.args, source, self.stop, self.emit, ready, source == "5G2")
        except Stopped:
            pass
        except Exception as exc:
            self.emit("collector", source=source, status="FAILED", reason=f"{type(exc).__name__}: {exc}")
            LOG.warning("%s采集不可用，文件测试继续: %s", source, exc)
        finally:
            if wire is not None:
                wire.close()
            ready.set()

    def start(self, board, server):
        args = self.args
        for remote, directory, executable in ((board, args.board_dir, args.client_executable),
                                               (server, args.server_dir, args.server_executable)):
            try:
                result = remote.run(f"cd {shlex.quote(directory)} && sha256sum {shlex.quote(executable)}", timeout_s=10)
                self.emit("binary", source=remote.label, directory=directory, executable=executable,
                          status="OK" if result.exit_code == 0 else "UNAVAILABLE", output=result.output, stderr=result.stderr)
            except Exception as exc:
                self.emit("binary", source=remote.label, status="UNAVAILABLE", reason=str(exc))
            try:
                before = time.monotonic()
                probe = remote.run("date +%s", timeout_s=5)
                after = time.monotonic()
                remote_seconds = float(probe.output.strip().splitlines()[-1])
                if probe.exit_code != 0:
                    raise ValueError(probe.stderr or "date命令失败")
                self.emit("clock_probe", source=remote.label, status="OK",
                          controller_mid_s=(before + after) / 2,
                          remote_realtime_s=remote_seconds + 0.5,
                          uncertainty_s=(after - before) / 2 + 0.5)
            except Exception as exc:
                self.emit("clock_probe", source=remote.label,
                          status="FAILED", reason=str(exc))
        api_dir = args.signal_api.rsplit("/", 1)[0] or "/"
        signal_cmd = f"cd {shlex.quote(api_dir)} && exec {shlex.quote(args.signal_api)}"
        monitor = Path(__file__).with_name("monitor_network.sh").read_text(encoding="utf-8")
        net_cmd = "sh -c " + shlex.quote(monitor) + " gx_network " + shlex.quote(str(args.network_interval_s))
        collectors = (("5G1", args.disable_signal_5g1, lambda: ChannelWire(board, signal_cmd), False),
                      ("5G2", args.disable_signal_5g2, lambda: SerialWire(args), False),
                      ("network", args.disable_network_monitor, lambda: ChannelWire(board, net_cmd), True))
        pending = []
        for source, disabled, factory, network in collectors:
            if args.disable_telemetry or disabled:
                self.emit("collector", source=source, status="DISABLED")
                continue
            ready = threading.Event()
            worker = threading.Thread(target=self._worker, args=(source, factory, ready, network), daemon=True)
            self.threads.append(worker)
            pending.append(ready)
            worker.start()
        deadline = time.monotonic() + min(60, args.signal_timeout_s * 6)
        for ready in pending:
            ready.wait(max(0, deadline - time.monotonic()))

    def begin_attempt(self, run_tag, **metadata):
        self.attempts[run_tag] = dict(run_tag=run_tag, start=time.monotonic(), start_utc=utc_now(), **metadata)
        self.emit("attempt_start", **self.attempts[run_tag])

    def end_attempt(self, run_tag):
        self.attempts[run_tag].update(end=time.monotonic(), end_utc=utc_now())
        self.emit("attempt_end", **self.attempts[run_tag])

    def result(self, run_tag, success, parsed, error):
        self.completed.add(run_tag)
        self.emit("attempt_result", run_tag=run_tag, success=success, error=error, **parsed)

    def ingest_transport(self, path, endpoint, expected_attempt=None):
        path = Path(path)
        accepted = 0
        split_files = {}
        with path.open(encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                try:
                    sample = json.loads(line)
                    if sample.get("schema_version") != TRANSPORT_SCHEMA_VERSION:
                        raise ValueError("未知schema_version")
                    if not isinstance(sample.get("paths"), list):
                        raise ValueError("paths不是数组")
                    if expected_attempt and sample.get("attempt_id") != expected_attempt:
                        raise ValueError("attempt_id不匹配")
                    if endpoint == "server" and sample.get("attempt_id"):
                        split_dir = self.root / "transport" / "server"
                        split_dir.mkdir(parents=True, exist_ok=True)
                        split = split_files.get(sample["attempt_id"])
                        if split is None:
                            split = (split_dir / f"{sample['attempt_id']}.jsonl").open(
                                "a", encoding="utf-8")
                            split_files[sample["attempt_id"]] = split
                        split.write(json.dumps(sample, ensure_ascii=False) + "\n")
                    self.emit("transport", endpoint=endpoint,
                              raw_path=str(path), **sample)
                    accepted += 1
                except (json.JSONDecodeError, ValueError, TypeError) as exc:
                    self.emit("transport_error", source=endpoint,
                              status="FAILED", raw_path=str(path),
                              line=line_number, reason=str(exc))
        for split in split_files.values():
            split.close()
        if accepted == 0:
            self.emit("transport_error", source=endpoint, status="FAILED",
                      raw_path=str(path), reason="没有可用的结构化传输样本")
        return accepted

    def stop_collectors(self):
        self.stop.set()
        for thread in self.threads:
            thread.join(timeout=7)

    def finish(self):
        self.stop_collectors()
        alive = any(t.is_alive() for t in self.threads)
        if alive:
            self.emit("collector", source="shutdown", status="FAILED", reason="采集线程未在等待期内退出")
        for tag, row in self.attempts.items():
            if "end" not in row:
                self.end_attempt(tag)
            if tag not in self.completed:
                self.emit("attempt_result", run_tag=tag, success=None, error="测试被中断，未获得完整结果")
        if not alive:
            self.file.close()
        target = self.root.parent.parent / f"experiment_{self.session}.xlsx"
        try:
            export_workbook(self.journal, target)
            LOG.info("实验 Excel: %s", target)
        except Exception as exc:
            LOG.warning("Excel 导出失败，原始采集日志保留于 %s: %s", self.journal, exc)


def telemetry_call(session, method, *args, **kwargs):
    """Every integration hook is observational and must not stop a transfer."""
    if session is None:
        return
    try:
        return getattr(session, method)(*args, **kwargs)
    except Exception as exc:
        LOG.warning("采集步骤 %s 失败，原流程继续: %s", method, exc)


def create_session(args, session, log_root):
    try:
        return TelemetrySession(args, session, log_root)
    except Exception as exc:
        LOG.warning("无法创建采集记录，文件测试继续: %s", exc)
        return None


def read_events(path):
    with Path(path).open(encoding="utf-8") as source:
        for number, line in enumerate(source, 1):
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                yield dict(kind="journal_error", reason=f"忽略损坏的日志行 {number}")


class AttemptIndex:
    def __init__(self, attempts):
        self.rows = sorted((a for a in attempts.values() if "start" in a), key=lambda a: a["start"])
        self.starts = [a["start"] for a in self.rows]

    def associate(self, start, end):
        if start is None or end is None:
            return {"phase": "UNKNOWN"}
        index = bisect.bisect_right(self.starts, start) - 1
        if index >= 0:
            row = self.rows[index]
            if start >= row["start"] and end <= row.get("end", -math.inf):
                return dict(phase="TEST", **{k: row.get(k) for k in ("run_tag", "round", "direction", "mode", "attempt")})
        for row in self.rows[max(0, index):index + 2]:
            if start <= row.get("end", row["start"]) and end >= row["start"]:
                return {"phase": "BOUNDARY"}
        return {"phase": "INITIALIZATION" if index < 0 else "COOLDOWN_OR_CLEANUP"}


def export_workbook(journal, target):
    """Build the three-sheet experiment workbook from the append-only journal."""
    from openpyxl import Workbook
    from openpyxl.cell import WriteOnlyCell
    from openpyxl.styles import Alignment, Font, PatternFill
    from openpyxl.utils import get_column_letter

    events = list(read_events(journal))
    attempts, config, diagnostics = {}, {}, []
    for event in events:
        kind = event.get("kind", "")
        if kind.startswith("attempt_") and event.get("run_tag"):
            attempts.setdefault(event["run_tag"], {}).update(event)
        elif kind == "config":
            config = event.get("values", {})
        elif kind not in ("raw", "signal", "network", "transport"):
            diagnostics.append(event)
    index = AttemptIndex(attempts)
    interval_s = float(config.get("excel_interval_s", 1.0))
    clock_offsets = {}
    for event in events:
        if event.get("kind") == "clock_probe" and event.get("status") == "OK":
            clock_offsets[event.get("source")] = (
                event["controller_mid_s"] - event["remote_realtime_s"])

    workbook = Workbook(write_only=True)
    sheets = {}

    def sheet(name, fields):
        ws = workbook.create_sheet(name)
        ws.freeze_panes = "F2" if name != "配置与异常" else "A2"
        ws.sheet_view.showGridLines = False
        cells = []
        for i, field in enumerate(fields, 1):
            ws.column_dimensions[get_column_letter(i)].width = 20 if field != "detail" else 80
            cell = WriteOnlyCell(ws, field)
            cell.font = Font(bold=True, color="FFFFFF")
            cell.fill = PatternFill("solid", fgColor="234E70")
            cell.alignment = Alignment(wrap_text=True, vertical="center")
            cells.append(cell)
        ws.append(cells)
        sheets[name] = [ws, fields, 1]

    def append(name, row):
        ws, fields, count = sheets[name]
        if count >= 1048576:
            raise ValueError(f"{name}超过Excel行数限制；原始JSONL已保留")
        cells = []
        for field in fields:
            value = row.get(field)
            if isinstance(value, (dict, list, tuple)):
                value = json.dumps(value, ensure_ascii=False, default=str)
            if isinstance(value, str):
                value = re.sub(r"[\x00-\x08\x0b\x0c\x0e-\x1f]", "", value)[:32767]
            cell = WriteOnlyCell(ws, value)
            if isinstance(value, str):
                cell.data_type = "s"
                cell.alignment = Alignment(wrap_text=True, vertical="top")
            elif isinstance(value, float):
                cell.number_format = "0.000"
            cells.append(cell)
        ws.append(cells)
        sheets[name][2] += 1

    ids = ["run_tag", "round", "direction", "mode", "attempt"]
    signal_fields = [f"{rat}_{field}" for rat in ("lte", "nr5g")
                     for field in ("valid", "rsrp", "rsrq", "rssi", "snr_raw")]
    periodic_fields = ids + [
        "window_index", "window_start_s", "window_end_s", "endpoint",
        "module", "interface", "path_id", "path_state", "app_status",
        "cwnd_bytes", "effective_cwnd_bytes", "srtt_us", "inflight_packets",
        "inflight_bytes", "connection_credit_bytes", "active_stream_credit_bytes",
        "effective_swnd_bytes", "cwnd_rtt_mbps", "inp_rtt_packets_s",
        "swnd_rtt_mbps", "path_tx_mbps", "transport_samples",
        "network_tx_mbps", "network_rx_mbps", "network_samples",
        "signal_samples", "signal_status", "level"] + signal_fields
    summary_fields = ids + [
        "success", "error", "start_utc", "end_utc", "total_bytes",
        "transfer_elapsed_s", "throughput_mbps", "metric",
        "wall_throughput_mbps", "aggregation_rate_pct",
        "aggregation_gain_vs_best_pct", "transport_samples",
        "signal_5g1_samples", "signal_5g2_samples", "network_samples",
        "collection_status"]
    sheet("周期采样", periodic_fields)
    sheet("传输汇总", summary_fields)
    sheet("配置与异常", ["kind", "source", "status", "recorded_utc", "detail"])

    for key, value in sorted(config.items()):
        append("配置与异常", {"kind": "config", "source": key, "detail": value})
    append("配置与异常", {"kind": "source", "detail": str(Path(journal).resolve())})
    for event in diagnostics:
        append("配置与异常", {"kind": event.get("kind"),
              "source": event.get("source"), "status": event.get("status"),
              "recorded_utc": event.get("recorded_utc"), "detail": event})

    def add(bucket, field, value):
        if value is not None and isinstance(value, (int, float)) and math.isfinite(value):
            bucket.setdefault(field, []).append(value)

    transport = [e for e in events if e.get("kind") == "transport"
                 and e.get("attempt_id") in attempts]
    client_maps = {}
    for sample in transport:
        if sample.get("endpoint") != "board":
            continue
        mapping = client_maps.setdefault(sample["attempt_id"], {})
        for path in sample.get("paths", []):
            if path.get("interface_name"):
                mapping[path.get("path_id")] = path.get("interface_name")

    starts = {}
    payload_starts = {}
    for sample in transport:
        key = (sample.get("attempt_id"), sample.get("endpoint"))
        if sample.get("sample_kind") == "start":
            starts.setdefault(key, sample.get("actual_mono_us"))
            attempt = attempts.get(sample.get("attempt_id"), {})
            expected_endpoint = "server" if attempt.get("direction") == "get" else "board"
            source = "云端" if sample.get("endpoint") == "server" else "板卡"
            if (sample.get("endpoint") == expected_endpoint
                    and source in clock_offsets
                    and isinstance(sample.get("realtime_us"), (int, float))):
                payload_starts.setdefault(sample.get("attempt_id"),
                    sample["realtime_us"] / 1_000_000 + clock_offsets[source])
    bins = {}
    previous_path = {}
    for sample in sorted(transport, key=lambda e: (e.get("attempt_id", ""),
                                                    e.get("endpoint", ""),
                                                    e.get("actual_mono_us", 0))):
        tag = sample.get("attempt_id")
        attempt = attempts.get(tag)
        endpoint = "server" if attempt and attempt.get("direction") == "get" else "board"
        if not attempt or sample.get("endpoint") != endpoint:
            continue
        start_us = starts.get((tag, endpoint))
        actual_us = sample.get("actual_mono_us")
        if not isinstance(start_us, (int, float)) or not isinstance(actual_us, (int, float)):
            continue
        window = max(0, int((actual_us - start_us) / (interval_s * 1_000_000)))
        for path in sample.get("paths", []):
            interface = path.get("interface_name") or client_maps.get(tag, {}).get(path.get("path_id"), "")
            module = MODULES.get(interface, "UNKNOWN")
            key = (tag, window, module, path.get("path_id"))
            bucket = bins.setdefault(key, {"endpoint": endpoint, "interface": interface,
                                            "path_states": [], "app_statuses": []})
            bucket["path_states"].append(path.get("path_state"))
            bucket["app_statuses"].append(path.get("app_status"))
            for field in ("cwnd_bytes", "effective_cwnd_bytes", "srtt_us",
                          "inflight_packets", "inflight_bytes"):
                add(bucket, field, path.get(field))
            for field in ("connection_credit_bytes", "active_stream_credit_bytes",
                          "effective_swnd_bytes"):
                add(bucket, field, sample.get(field))
            srtt = path.get("srtt_us")
            if isinstance(srtt, (int, float)) and srtt > 0:
                add(bucket, "cwnd_rtt_mbps", 8 * path.get("cwnd_bytes", 0) / srtt)
                add(bucket, "inp_rtt_packets_s", 1_000_000 * path.get("inflight_packets", 0) / srtt)
                add(bucket, "swnd_rtt_mbps", 8 * sample.get("effective_swnd_bytes", 0) / srtt)
            pkey = (tag, endpoint, path.get("path_id"))
            prior = previous_path.get(pkey)
            sent = path.get("sent_bytes")
            if prior and isinstance(sent, int) and sent >= prior[1] and actual_us > prior[0]:
                add(bucket, "path_tx_mbps", 8 * (sent - prior[1]) / (actual_us - prior[0]))
            if isinstance(sent, int):
                previous_path[pkey] = (actual_us, sent)
            bucket["transport_samples"] = bucket.get("transport_samples", 0) + 1

    index_by_tag = {row["run_tag"]: row for row in index.rows}
    signal_bins, network_bins = {}, {}
    for event in events:
        if event.get("kind") not in ("signal", "network"):
            continue
        association = index.associate(event.get("start"), event.get("end"))
        if association.get("phase") != "TEST":
            continue
        tag = association["run_tag"]
        attempt = index_by_tag[tag]
        payload_start = payload_starts.get(tag, attempt["start"])
        module = (event.get("source") if event["kind"] == "signal" else
                  event.get("module") or MODULES.get(event.get("interface"), "UNKNOWN"))
        if event.get("status") != "OK":
            continue
        if event["kind"] == "signal":
            midpoint = (event.get("start", 0) + event.get("end", 0)) / 2
            window = max(0, int((midpoint - payload_start) / interval_s))
            bucket = signal_bins.setdefault((tag, window, module), {})
            bucket["signal_status"] = "OK"
            bucket["signal_samples"] = bucket.get("signal_samples", 0) + 1
            for field in ["level"] + signal_fields:
                add(bucket, field, event.get(field))
            continue

        # Network samples represent rates over [start, end]. Split a sample
        # across every Excel window it overlaps and weight it by that overlap.
        sample_start = max(event.get("start", 0), payload_start, attempt["start"])
        sample_end = min(event.get("end", 0), attempt["end"])
        if sample_end <= sample_start:
            continue
        first_window = max(0, int((sample_start - payload_start) / interval_s))
        last_window = max(first_window, int(math.ceil(
            (sample_end - payload_start) / interval_s) - 1))
        for window in range(first_window, last_window + 1):
            window_start = payload_start + window * interval_s
            window_end = window_start + interval_s
            overlap_s = max(0.0, min(sample_end, window_end)
                            - max(sample_start, window_start))
            if overlap_s <= 0:
                continue
            bucket = network_bins.setdefault((tag, window, module), {})
            bucket["network_samples"] = bucket.get("network_samples", 0) + 1
            bucket["_network_weight_s"] = bucket.get("_network_weight_s", 0.0) + overlap_s
            for field in ("network_tx_mbps", "network_rx_mbps"):
                value = event.get("tx_mbps" if field.endswith("tx_mbps") else "rx_mbps")
                if isinstance(value, (int, float)) and math.isfinite(value):
                    weighted = "_" + field + "_weighted"
                    bucket[weighted] = bucket.get(weighted, 0.0) + value * overlap_s

    all_keys = set(bins)
    for tag, window, module in set(signal_bins) | set(network_bins):
        if not any(k[:3] == (tag, window, module) for k in all_keys):
            all_keys.add((tag, window, module, None))
    for key in sorted(all_keys, key=lambda k: (k[0], k[1], str(k[2]), str(k[3]))):
        tag, window, module, path_id = key
        attempt = attempts[tag]
        bucket = bins.get(key, {})
        row = {name: attempt.get(name) for name in ids}
        row.update(window_index=window, window_start_s=window * interval_s,
                   window_end_s=(window + 1) * interval_s, module=module,
                   path_id=path_id, endpoint=bucket.get("endpoint"),
                   interface=bucket.get("interface"),
                   path_state=(bucket.get("path_states") or [None])[-1],
                   app_status=(bucket.get("app_statuses") or [None])[-1],
                   transport_samples=bucket.get("transport_samples", 0))
        for field, values in bucket.items():
            if isinstance(values, list) and values and field not in ("path_states", "app_statuses"):
                row[field] = statistics.mean(values)
        signal_source = signal_bins.get((tag, window, module), {})
        for field, values in signal_source.items():
            row[field] = statistics.mean(values) if isinstance(values, list) and values else values
        network_source = network_bins.get((tag, window, module), {})
        network_weight = network_source.get("_network_weight_s", 0)
        row["network_samples"] = network_source.get("network_samples", 0)
        if network_weight > 0:
            for field in ("network_tx_mbps", "network_rx_mbps"):
                weighted = network_source.get("_" + field + "_weighted")
                if isinstance(weighted, (int, float)):
                    row[field] = weighted / network_weight
        append("周期采样", row)

    grouped_speeds = {}
    for attempt in attempts.values():
        if attempt.get("success") and isinstance(attempt.get("throughput_mbps"), (int, float)):
            grouped_speeds[(attempt.get("round"), attempt.get("direction"), attempt.get("mode"))] = attempt["throughput_mbps"]
    for attempt in index.rows:
        row = dict(attempt)
        key = (attempt.get("round"), attempt.get("direction"))
        one = grouped_speeds.get(key + ("rmnet",))
        two = grouped_speeds.get(key + ("usb",))
        agg = grouped_speeds.get(key + ("agg",))
        if attempt.get("mode") == "agg" and all(isinstance(x, (int, float)) for x in (one, two, agg)):
            if one + two > 0:
                row["aggregation_rate_pct"] = agg / (one + two) * 100
            if max(one, two) > 0:
                row["aggregation_gain_vs_best_pct"] = (agg / max(one, two) - 1) * 100
        selected = [e for e in transport if e.get("attempt_id") == attempt["run_tag"]
                    and e.get("endpoint") == ("server" if attempt.get("direction") == "get" else "board")]
        row["transport_samples"] = len(selected)
        row["signal_5g1_samples"] = sum(v.get("signal_samples", 0) for k, v in signal_bins.items()
                                               if k[0] == attempt["run_tag"] and k[2] == "5G1")
        row["signal_5g2_samples"] = sum(v.get("signal_samples", 0) for k, v in signal_bins.items()
                                               if k[0] == attempt["run_tag"] and k[2] == "5G2")
        row["network_samples"] = sum(v.get("network_samples", 0) for k, v in network_bins.items()
                                         if k[0] == attempt["run_tag"])
        if config.get("disable_telemetry"):
            row["collection_status"] = "DISABLED"
        else:
            expected = [bool(selected)]
            if not config.get("disable_signal_5g1"):
                expected.append(row["signal_5g1_samples"] > 0)
            if not config.get("disable_signal_5g2"):
                expected.append(row["signal_5g2_samples"] > 0)
            if not config.get("disable_network_monitor"):
                expected.append(row["network_samples"] > 0)
            row["collection_status"] = "SAMPLED" if all(expected) else "PARTIAL"
        if "success" not in row:
            row.update(success=None, error="传输结果未完整记录（可能被中断）")
        append("传输汇总", row)

    for ws, fields, count in sheets.values():
        ws.auto_filter.ref = f"A1:{get_column_letter(len(fields))}{count}"
    target = Path(target)
    target.parent.mkdir(parents=True, exist_ok=True)
    temp = target.with_suffix(".part.xlsx")
    workbook.save(temp)
    os.replace(temp, target)
    return target


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="从实验 events.jsonl 离线重建 Excel，不连接设备")
    parser.add_argument("journal", type=Path)
    parser.add_argument("--output", type=Path)
    cli = parser.parse_args()
    print(export_workbook(cli.journal, cli.output or cli.journal.with_name("experiment_rebuilt.xlsx")))
