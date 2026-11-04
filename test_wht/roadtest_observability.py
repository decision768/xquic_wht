"""P0 passive observations for 副本改进版.py. No transfer verdicts are changed.

Re-export retained raw observations without SSH:
    python roadtest_observability.py OBSERVATION_DIRECTORY
"""
from __future__ import annotations

import csv
import hashlib
import importlib
import json
import logging
import os
import posixpath
import re
import shlex
import time
from collections import defaultdict
from pathlib import Path

LOG = logging.getLogger("mini_api_get_post_benchmark")
TABLES = ("path_windows", "rx_hol_streams", "rx_hol_episodes",
          "transfer_observability_summary", "observer_health")


def add_arguments(parser):
    from benchmark_telemetry import add_arguments as add_optional_collectors
    add_optional_collectors(parser)
    parser.set_defaults(disable_telemetry=True, disable_signal_5g1=True,
                        disable_signal_5g2=True, disable_network_monitor=True)
    group = parser.add_argument_group("P0 被动观测，独立于业务成功状态")
    group.add_argument("--disable-observer", action="store_true")
    group.add_argument("--observer-interval-ms", type=int, default=100)
    group.add_argument("--observer-checkpoint-timeout-s", type=float, default=15.0)
    group.add_argument("--observer-radio", action="store_true", help="显式启用已有双5G信号采集")
    group.add_argument("--observer-network", action="store_true", help="显式启用已有网卡采集")


def _json_response(output):
    for line in reversed(output.splitlines()):
        try:
            result = json.loads(line)
            if isinstance(result, dict):
                return result
        except ValueError:
            continue
    raise ValueError("端点未返回 JSON 对象")


def _claim_cleanup_lock(path):
    handle = path.open("a+b")
    try:
        handle.seek(0, os.SEEK_END)
        if not handle.tell():
            handle.write(b"0")
            handle.flush()
        handle.seek(0)
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        return handle
    except BaseException:
        handle.close()
        raise


class RoadtestObserver:
    def __init__(self, args, session, root, board, server):
        self.args, self.session = args, session
        self.root = Path(root) / "observability"
        self.root.mkdir(parents=True, exist_ok=True)
        self._cleanup_lock = _claim_cleanup_lock(self.root / "cleanup.lock")
        self.remotes = {"board": board, "server": server}
        self.enabled = not args.disable_observer
        self.supported = {"board": False, "server": False}
        self.paths = {
            "board": args.board_resource_dir.rstrip("/") + "/observer_" + session,
            "server": args.server_resource_dir.rstrip("/") + "/observer_" + session,
        }
        self.manifest = {"session_id": session, "schema_families": {"transport": [1, 2],
                         "observer_events": [1]}, "endpoints": {}, "attempts": {},
                         "files": {}, "errors": [], "interval_ms": args.observer_interval_ms}
        self.active = set()
        self.collected = set()
        self.archive_index = 0
        self.server_log_uncollected = False
        self.optional_collectors = None
        self.manifest["cleanup"] = {
            "version": 1, "hosts": self._cleanup_hosts(), "roots": {},
            "pending": {"board": {}, "server": {}}, "complete": False,
        }
        self.save()

    def release_cleanup_lock(self):
        handle = getattr(self, "_cleanup_lock", None)
        if handle is not None:
            handle.close()
            self._cleanup_lock = None

    def __del__(self):
        self.release_cleanup_lock()

    def _cleanup_hosts(self):
        return {ep: [getattr(self.args, ep + "_ssh_host", None),
                     getattr(self.args, ep + "_ssh_port", None)]
                for ep in ("board", "server")}

    def _temporary_parent(self, endpoint, path):
        """Only generated names in configured roots belong to this session."""
        if (not isinstance(path, str) or not path.startswith("/")
                or posixpath.normpath(path) != path or any(c in path for c in "\n\r\0")
                or not re.fullmatch(r"[A-Za-z0-9_.-]+", self.session)):
            raise ValueError("不安全的临时文件路径")
        parent, name = posixpath.split(path)
        token = re.escape("api_" + self.session + "_") + r"[A-Za-z0-9_.-]+"
        resource = self.args.board_resource_dir if endpoint == "board" else self.args.server_resource_dir
        allowed = parent == resource.rstrip("/") and re.fullmatch(
            token + (r"_resource\.csv" if endpoint == "board" else r"_server_memory\.csv"), name)
        if parent == "/tmp":
            suffix = (r"_(?:resource\.stop|resource_monitor\.log|client\.start)" if endpoint == "board"
                      else r"_server_memory\.(?:stop|status)")
            allowed = allowed or re.fullmatch(token + suffix, name)
        if endpoint == "board" and parent == self.args.board_dir.rstrip("/"):
            allowed = allowed or re.fullmatch(
                r"get-(?:rmnet|usb|agg)-" + re.escape(self.session) + r"-[A-Za-z0-9_.-]+\.bin", name)
        if endpoint == "server" and parent == "/tmp/mini_api_gx_" + self.session:
            allowed = allowed or name == "server.pid" or re.fullmatch(
                r"server_startup_[A-Za-z0-9_-]+_[0-9]+(?:\.stderr)?\.log", name)
        if not allowed:
            raise ValueError("文件不属于本会话临时产物：" + path)
        return parent

    def track_temporary(self, endpoint, attempt, *paths):
        """Persist ownership before a remote producer can create a file."""
        state = self.manifest["cleanup"]
        for path in paths:
            parent = self._temporary_parent(endpoint, path)
            key = endpoint + ":" + parent
            if key not in state["roots"]:
                command = ("mkdir -p -- " + shlex.quote(parent) + "; cd -- " + shlex.quote(parent)
                           + " && pwd -P")
                result = self.remotes[endpoint].run(command, timeout_s=10)
                if result.exit_code or result.timed_out or result.truncated or not result.output.strip().startswith("/"):
                    raise OSError("不能确认临时文件目录：" + parent)
                state["roots"][key] = result.output.strip()
            state["pending"][endpoint][path] = attempt
        state["complete"] = False
        self.save()

    def _cleanup_idle(self, endpoint, attempt=None):
        session = re.escape(self.session)
        if attempt is None:
            pattern = (r"^(MINI_API_BENCH_SESSION=" + session
                       + r"|MINI_API_BENCH_(CLIENT|MONITOR|SERVER_MONITOR)_SESSION=api_"
                       + session + r"_[A-Za-z0-9_.-]+)$")
        else:
            pattern = r"^MINI_API_BENCH_(CLIENT|MONITOR|SERVER_MONITOR)_SESSION=" + re.escape(attempt) + "$"
        command = ("set -e; for f in /proc/[0-9]*/environ; do "
                   "[ -r \"$f\" ] || continue; "
                   "if tr '\\000' '\\n' < \"$f\" 2>/dev/null | grep -Eq " + shlex.quote(pattern)
                   + "; then exit 9; fi; done; printf '__OBSERVER_IDLE__\\n'")
        result = self.remotes[endpoint].run(command, timeout_s=20)
        return (not result.exit_code and not result.timed_out and not result.truncated
                and result.output.strip() == "__OBSERVER_IDLE__")

    def _unlink_temporary(self, endpoint, parent, names, physical):
        """Unlink exact regular files; never follow a file symlink or recurse."""
        if not names:
            return
        quoted = " ".join(shlex.quote(name) for name in names)
        command = ("set -e; cd -- " + shlex.quote(parent)
                   + "; test \"$(pwd -P)\" = " + shlex.quote(physical)
                   + "; for f in " + quoted + "; do test ! -L \"$f\"; "
                   "if test -e \"$f\"; then test -f \"$f\"; fi; done; "
                   "rm -f -- " + quoted + "; for f in " + quoted
                   + "; do test ! -e \"$f\"; test ! -L \"$f\"; done; printf '__OBSERVER_REMOVED__\\n'")
        for retry in range(3):
            try:
                result = self.remotes[endpoint].run(command, timeout_s=20)
                if (not result.exit_code and not result.timed_out and not result.truncated
                        and result.output.strip() == "__OBSERVER_REMOVED__"):
                    return
            except Exception:
                if retry == 2:
                    raise
        raise OSError("临时文件删除未确认：" + parent)

    def cleanup_temporary(self, attempt=None, final=False):
        state = self.manifest["cleanup"]
        failures = []
        for endpoint in self.remotes:
            try:
                pending = state["pending"][endpoint]
                selected = [p for p, tag in pending.items() if final or tag == attempt]
                if not selected and not (final and self.enabled):
                    continue
                if not self._cleanup_idle(endpoint, None if final else attempt):
                    raise OSError("本会话写入进程尚未确认停止")
                groups = defaultdict(list)
                for path in selected:
                    groups[self._temporary_parent(endpoint, path)].append(path)
                if final and self.enabled:
                    directory = self.paths[endpoint]
                    key = endpoint + ":" + directory
                    physical = state["roots"].get(key)
                    if physical:
                        command = ("if test -e " + shlex.quote(directory) + "; then test ! -L "
                                   + shlex.quote(directory) + " && cd -- " + shlex.quote(directory)
                                   + " && test \"$(pwd -P)\" = " + shlex.quote(physical)
                                   + " && find . -maxdepth 1 -type f -print; fi")
                        result = self.remotes[endpoint].run(command, timeout_s=15)
                        if result.exit_code or result.timed_out or result.truncated:
                            raise OSError("不能核验观测目录")
                        owners = ({tag for tag in self.manifest["attempts"]
                                   if re.fullmatch(re.escape("api_" + self.session + "_") + r"[A-Za-z0-9_.-]+", tag)}
                                  if endpoint == "board" else {"server"})
                        for line in result.output.splitlines():
                            name = line.removeprefix("./")
                            match = re.fullmatch(r"(.+)\.[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+"
                                r"\.(?:(?:transport|events)\.[0-9]{6}\.jsonl|health\.json|"
                                r"checkpoint\.[0-9]{6}\.json)(?:\.active)?", name)
                            if match and match[1] in owners:
                                groups[directory].append(directory + "/" + name)
                for parent, paths in groups.items():
                    physical = state["roots"].get(endpoint + ":" + parent)
                    if not physical:
                        raise ValueError("缺少目录身份凭据：" + parent)
                    # Save every candidate before deleting, including incomplete
                    # writer files without a checkpoint after confirmed shutdown.
                    state.setdefault("last_candidates", {})[endpoint] = paths
                    self.save()
                    for offset in range(0, len(paths), 64):
                        batch = paths[offset:offset+64]
                        self._unlink_temporary(endpoint, parent, [posixpath.basename(p) for p in batch], physical)
                        for path in batch:
                            pending.pop(path, None)
                            meta = self.manifest["files"].get(endpoint + ":" + path)
                            if meta:
                                meta["remote_deleted"] = True
                            elif parent == self.paths[endpoint]:
                                state.setdefault("discarded_unarchived", []).append(endpoint + ":" + path)
                        self.save()
            except Exception as exc:
                failures.append(endpoint + ": " + str(exc))
                LOG.error("临时文件回收未完成 %s: %s", endpoint, exc)
        state["failures"] = failures
        if final:
            state["complete"] = not failures and not any(state["pending"].values())
        self.save()
        return not failures

    def save(self):
        target = self.root / "manifest.json"
        pending = target.with_suffix(".tmp")
        with pending.open("w", encoding="utf-8") as output:
            json.dump(self.manifest, output, ensure_ascii=False, indent=2, allow_nan=False)
            output.flush()
            os.fsync(output.fileno())
        pending.replace(target)

    def error(self, endpoint, reason, **extra):
        LOG.warning("P0观测 %s: %s", endpoint, reason)
        self.manifest["errors"].append(dict(endpoint=endpoint, reason=str(reason), **extra))
        self.save()

    def start(self):
        if not self.enabled:
            return
        for dependency in ("openpyxl", "matplotlib.pyplot"):
            importlib.import_module(dependency)
        for endpoint, remote in self.remotes.items():
            executable = self.args.client_executable if endpoint == "board" else self.args.server_executable
            directory = self.args.board_dir if endpoint == "board" else self.args.server_dir
            command = "cd -- " + shlex.quote(directory) + " && " + shlex.quote(executable)
            info = {"executable": executable, "directory": directory}
            self.manifest["endpoints"][endpoint] = info
            try:
                response = remote.run(command + " --telemetry-capabilities", timeout_s=10)
                capabilities = _json_response(response.output)
                if (response.exit_code or response.timed_out or response.truncated
                        or capabilities.get("transport_schema") != 2
                        or capabilities.get("event_schema") != 1
                        or capabilities.get("hol_event_accounting") is not True
                        or capabilities.get("observer_revision") != 2
                        or capabilities.get("clock_domain") != "CLOCK_MONOTONIC"
                        or capabilities.get("task_checkpoint") is not True):
                    raise ValueError("需要 observer revision 2、CLOCK_MONOTONIC 和 task_checkpoint 的配套二进制")
                mkdir = remote.run("mkdir -p -- " + shlex.quote(self.paths[endpoint])
                                   + " && test ! -L " + shlex.quote(self.paths[endpoint])
                                   + " && cd -- " + shlex.quote(self.paths[endpoint]) + " && pwd -P", timeout_s=10)
                if mkdir.exit_code or mkdir.timed_out or mkdir.truncated or not mkdir.output.strip().startswith("/"):
                    raise OSError("不能创建端点观测目录")
                self.manifest["cleanup"]["roots"][endpoint + ":" + self.paths[endpoint]] = mkdir.output.strip()
                self.save()
                info["capabilities"] = capabilities
                info["source_build_id"] = None
                version = remote.run(command + " --version", timeout_s=5)
                info["version_output"] = version.output if version.exit_code == 0 else None
                digest = remote.run("cd -- " + shlex.quote(directory) + " && sha256sum -- "
                                    + shlex.quote(executable), timeout_s=10)
                token = digest.output.strip().split()
                if digest.exit_code or digest.timed_out or digest.truncated or not token or len(token[0]) != 64:
                    raise ValueError("不能取得端点二进制 SHA256")
                info["binary_sha256"] = token[0]
                self.clock_probe(endpoint, command, "before")
                self.supported[endpoint] = True
            except Exception as exc:
                self.error(endpoint, exc, status="UNSUPPORTED")
        self.save()
        if not all(self.supported.values()):
            raise RuntimeError("双端 P0 观测预检未通过，尚未开始文件传输；详见 manifest.errors")
        if getattr(self.args, "observer_radio", False) or getattr(self.args, "observer_network", False):
            from benchmark_telemetry import create_session, telemetry_call
            from argparse import Namespace
            optional_args = Namespace(**{k: v for k, v in vars(self.args).items() if not k.startswith("_")})
            optional_args.board_ssh_key = optional_args.server_ssh_key = ""
            optional_args.disable_telemetry = False
            optional_args.disable_signal_5g1 = optional_args.disable_signal_5g2 = not self.args.observer_radio
            optional_args.disable_network_monitor = not self.args.observer_network
            self.optional_collectors = create_session(optional_args, self.session, self.root)
            telemetry_call(self.optional_collectors, "start", self.remotes["board"], self.remotes["server"])

    def clock_probe(self, endpoint, command=None, phase="after"):
        if command is None:
            directory = self.args.board_dir if endpoint == "board" else self.args.server_dir
            executable = self.args.client_executable if endpoint == "board" else self.args.server_executable
            command = "cd -- " + shlex.quote(directory) + " && " + shlex.quote(executable)
        probes = []
        for _ in range(3):
            start_wall = time.time_ns() // 1000
            start = time.monotonic_ns() // 1000
            result = self.remotes[endpoint].run(command + " --observer-clock", timeout_s=5)
            end = time.monotonic_ns() // 1000
            if result.exit_code or result.timed_out or result.truncated:
                raise OSError("高精度时钟探测失败")
            row = _json_response(result.output)
            if (row.get("observer_revision") != 2 or row.get("clock_domain") != "CLOCK_MONOTONIC"
                    or any(type(row.get(k)) is not int or row[k] <= 0 for k in ("monotonic_us", "realtime_us"))
                    or not isinstance(row.get("boot_id"), str) or row.get("boot_id") in ("", "unknown")):
                raise ValueError("端点时钟基准、时间或 boot_id 无效")
            row.update(controller_realtime_us=start_wall, rtt_us=end-start,
                       controller_offset_us=start_wall+(end-start)//2-row["realtime_us"],
                       uncertainty_us=(end-start+1)//2+2, phase=phase)
            probes.append(row)
        info = self.manifest["endpoints"][endpoint]
        info.setdefault("clock_probes", []).extend(probes)
        info["best_clock_" + phase] = min(probes, key=lambda x: x["rtt_us"])
        info["alignment_valid"] = True
        info["clock_error_bound_us"] = info["best_clock_" + phase]["uncertainty_us"]
        before, after = info.get("best_clock_before"), info.get("best_clock_after")
        if before and after:
            changed = before.get("boot_id") != after.get("boot_id")
            a = before["realtime_us"]-before["monotonic_us"]+before["controller_offset_us"]
            b = after["realtime_us"]-after["monotonic_us"]+after["controller_offset_us"]
            info.update(clock_segment_changed=changed, alignment_valid=not changed,
                        clock_mapping_drift_us=abs(b-a),
                        clock_error_bound_us=max(before["uncertainty_us"], after["uncertainty_us"])+abs(b-a))

    def client_arguments(self, attempt):
        if not self.supported["board"]:
            return []
        return ["--attempt-id", attempt, "--telemetry-output", self.paths["board"] + "/" + attempt,
                "--telemetry-interval-ms", str(self.args.observer_interval_ms)]

    def server_arguments(self):
        if not self.supported["server"]:
            return ""
        return " ".join(shlex.quote(x) for x in ["--telemetry-output", self.paths["server"] + "/server",
                        "--telemetry-interval-ms", str(self.args.observer_interval_ms)]) + " "

    def command(self, endpoint, argv):
        self.manifest["endpoints"].setdefault(endpoint, {}).setdefault("commands", []).append(argv)
        self.save()

    def begin(self, attempt, metadata):
        self.active.add(attempt)
        self.manifest["attempts"][attempt] = dict(metadata, run_tag=attempt,
            controller_start_us=time.time_ns()//1000)
        self.save()
        if self.optional_collectors:
            from benchmark_telemetry import telemetry_call
            telemetry_call(self.optional_collectors, "begin_attempt", attempt, **metadata)

    def archive_server_log(self, state_directory):
        if not self.enabled:
            return
        remote = self.remotes["server"]
        try:
            listing = remote.run("find " + shlex.quote(state_directory)
                + " -maxdepth 1 -type f -name 'server_startup*.log' -print", timeout_s=10)
            if listing.exit_code or listing.truncated or listing.timed_out:
                raise OSError("不能完整列举服务端运行日志")
            for source in listing.output.splitlines():
                if not source.startswith(state_directory + "/") or "server-log:"+source in self.collected:
                    continue
                self.archive_index += 1
                local = self.root / "server_logs" / f"server_{self.archive_index:04d}.log"
                local.parent.mkdir(parents=True, exist_ok=True)
                remote.download(source, local)
                digest = hashlib.sha256(local.read_bytes()).hexdigest()
                check = remote.run("sha256sum -- " + shlex.quote(source), timeout_s=10)
                parts = check.output.strip().split()
                if check.exit_code or not parts or parts[0].lower() != digest:
                    raise OSError("服务端日志校验失败，原文件保留：" + source)
                self.manifest["files"]["server-log:"+source] = dict(endpoint="server", remote_path=source,
                    local_path=str(local.relative_to(self.root)), sha256=digest, size=local.stat().st_size)
                self.collected.add("server-log:"+source)
            self.server_log_uncollected = False
            self.save()
        except Exception as exc:
            self.server_log_uncollected = True
            self.error("server", exc, remote_recovery_directory=state_directory)

    def needs_server_state(self):
        return self.server_log_uncollected

    def end(self, attempt, success=None, error=None):
        try:
            return self._end(attempt, success, error)
        finally:
            try:
                self.cleanup_archived_logs()
            finally:
                if not self.cleanup_temporary(attempt):
                    raise RuntimeError("本任务临时文件回收未完成，停止后续测试")

    def _end(self, attempt, success=None, error=None):
        self.active.discard(attempt)
        entry = self.manifest["attempts"][attempt]
        entry.update(controller_end_us=time.time_ns()//1000, business_success=success, business_error=error)
        if not self.enabled:
            self.save()
            return {"status": "DISABLED", "errors": []}
        deadline = time.monotonic() + getattr(self.args, "observer_checkpoint_timeout_s", 15.0)
        found = set()
        while True:
            self.collect(checkpoints_only=True)
            found.clear()
            for meta in self.manifest["files"].values():
                if ".checkpoint." not in meta.get("local_path", ""):
                    continue
                receipt = json.loads((self.root / meta["local_path"]).read_text(encoding="utf-8"))
                if receipt.get("attempt_id") == attempt:
                    found.add(meta["endpoint"])
            if found == set(self.remotes) or time.monotonic() >= deadline:
                break
            time.sleep(min(0.25, max(0, deadline-time.monotonic())))
        # Receipts are published after sealing; fetch data only after receipt polling.
        # Download duration is separate from the checkpoint publication timeout.
        self.collect()
        self.save()
        tables = export(self.root)
        summary = next((r for r in tables["transfer_observability_summary"] if r["attempt_id"] == attempt), {})
        reasons = list(summary.get("integrity_errors" if success is False else "validation_errors", []))
        if found != set(self.remotes):
            reasons.append("双端任务检查点等待超时：" + ",".join(sorted(set(self.remotes)-found)))
        if success is False and not summary.get("integrity_valid"):
            reasons.append("失败任务观测完整性未通过验收")
        elif success is not False and (not summary.get("hol_valid") or not summary.get("window_valid")):
            reasons.append("HoL 或发送窗口未通过完整性验收")
        result = {"status": "INVALID" if reasons else "OK", "errors": list(dict.fromkeys(reasons)),
                  "metrics_valid": bool(summary.get("observation_valid"))}
        entry["observation"] = result
        self.save()
        if self.optional_collectors:
            from benchmark_telemetry import telemetry_call
            telemetry_call(self.optional_collectors, "end_attempt", attempt)
            telemetry_call(self.optional_collectors, "result", attempt, success, {}, error)
        return result

    def collect(self, final=False, checkpoints_only=False):
        """Copy and verify sealed segments; cleanup is a separate archive-only step.

        On final shutdown, also salvage .active files as incomplete evidence.
        """
        for endpoint, remote in self.remotes.items():
            if not self.supported[endpoint]:
                continue
            directory = self.paths[endpoint]
            try:
                listing = remote.run("find " + shlex.quote(directory) + " -maxdepth 1 -type f -print",
                                     timeout_s=15)
                if listing.exit_code or listing.truncated or listing.timed_out:
                    raise OSError("不能完整列举远端日志，原文件保留")
                for source in listing.output.splitlines():
                    if not source.startswith(directory + "/") or "\n" in source:
                        continue
                    if source.endswith(".active") and not final:
                        continue
                    if checkpoints_only and not (".checkpoint." in source and source.endswith(".json")):
                        continue
                    if not source.endswith((".jsonl", ".health.json", ".active")) and not (".checkpoint." in source and source.endswith(".json")):
                        continue
                    key = endpoint + ":" + source
                    if key in self.collected:
                        continue
                    basename = Path(source).name
                    prefix = _writer_prefix(basename)
                    archive_name = hashlib.sha256(prefix.encode()).hexdigest()[:24] + basename[len(prefix):]
                    local = self.root / "raw" / endpoint / archive_name
                    local.parent.mkdir(parents=True, exist_ok=True)
                    remote.download(source, local)
                    digest = hashlib.sha256(local.read_bytes()).hexdigest()
                    check = remote.run("sha256sum -- " + shlex.quote(source), timeout_s=15)
                    parts = check.output.strip().split()
                    if check.exit_code or not parts or parts[0].lower() != digest:
                        raise OSError("日志 SHA256 不匹配，保留本地和远端副本：" + source)
                    self.manifest["files"][key] = dict(endpoint=endpoint, remote_path=source,
                        local_path=str(local.relative_to(self.root)), sha256=digest,
                        size=local.stat().st_size, incomplete=source.endswith(".active"))
                    self.collected.add(key)
                self.save()
            except Exception as exc:
                self.error(endpoint, exc, remote_recovery_directory=directory)

    def cleanup_archived_logs(self):
        """Unlink exact immutable writer files, only after a durable local archive.

        No directories, wildcards, active files, or server startup logs are removed.
        Checkpoints prove writer ownership even when measurement validity failed.
        """
        if not self.enabled:
            return
        files = self.manifest.get("files", {})
        session = self.session
        if not re.fullmatch(r"[A-Za-z0-9_.-]+", session):
            return

        def archived(meta, endpoint):
            if meta.get("endpoint") != endpoint or meta.get("incomplete"):
                return None
            remote_path = meta.get("remote_path", "")
            directory = self.paths[endpoint]
            if (not directory.startswith("/") or posixpath.normpath(directory) != directory
                    or posixpath.basename(directory) != "observer_" + session
                    or posixpath.dirname(remote_path) != directory):
                return None
            name = posixpath.basename(remote_path)
            match = re.fullmatch(r"(.+\.[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)"
                                 r"(\.(?:transport|events)\.[0-9]{6}\.jsonl|"
                                 r"\.checkpoint\.[0-9]{6}\.json|\.health\.json)", name)
            if not match:
                return None
            prefix, suffix = match.groups()
            digest = hashlib.sha256(prefix.encode()).hexdigest()[:24]
            expected_local = "raw/" + endpoint + "/" + digest + suffix
            if meta.get("local_path", "").replace("\\", "/") != expected_local:
                return None
            local = self.root / expected_local
            if (local.is_symlink() or (self.root / "raw").is_symlink()
                    or (self.root / "raw" / endpoint).is_symlink()
                    or local.resolve().parent != (self.root / "raw" / endpoint).resolve()):
                return None
            try:
                with local.open("r+b") as source:
                    if hashlib.sha256(source.read()).hexdigest() != meta.get("sha256"):
                        return None
                    os.fsync(source.fileno())
            except OSError:
                return None
            return prefix, suffix, local

        for endpoint, remote in self.remotes.items():
            if not self.supported[endpoint]:
                continue
            pending_prefixes = {_writer_prefix(v.get("remote_path", "")) for v in files.values()
                                if v.get("endpoint") == endpoint and not v.get("remote_deleted")}
            owned = set()
            for meta in files.values():
                if (".checkpoint." not in meta.get("remote_path", "")
                        or _writer_prefix(meta.get("remote_path", "")) not in pending_prefixes):
                    continue
                item = archived(meta, endpoint)
                if not item or not item[1].startswith(".checkpoint."):
                    continue
                try:
                    receipt = json.loads(item[2].read_text(encoding="utf-8"))
                    attempt = receipt.get("attempt_id")
                    base = attempt if endpoint == "board" else "server"
                    if (attempt in self.manifest["attempts"] and receipt.get("endpoint") == endpoint
                            and receipt.get("task_complete") is True
                            and receipt.get("writer_prefix") == self.paths[endpoint] + "/" + item[0]
                            and isinstance(base, str) and re.fullmatch(
                                re.escape(base) + r"\.[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+", item[0])):
                        owned.add(item[0])
                except (OSError, ValueError, TypeError, AttributeError):
                    continue
            for meta in files.values():
                if meta.get("remote_deleted"):
                    continue
                item = archived(meta, endpoint)
                if not item or item[0] not in owned:
                    continue
                directory, name = self.paths[endpoint], posixpath.basename(meta["remote_path"])
                # The session directory must resolve to itself. Refuse symlinked
                # directories/files and recheck the remote digest before unlinking.
                command = ("set -e; test ! -L " + shlex.quote(directory)
                    + "; cd -- " + shlex.quote(directory)
                    + "; test \"$(pwd -P)\" = " + shlex.quote(
                        self.manifest.get("cleanup", {}).get("roots", {}).get(endpoint + ":" + directory, directory))
                    + "; test ! -L " + shlex.quote(name)
                    + "; if test ! -e " + shlex.quote(name)
                    + "; then printf '__OBSERVER_ALREADY_ABSENT__\\n'; exit 0; fi"
                    + "; test -f " + shlex.quote(name)
                    + "; observer_digest=$(sha256sum -- " + shlex.quote(name) + ")"
                    + "; test \"${observer_digest%% *}\" = " + shlex.quote(meta["sha256"])
                    + "; rm -f -- " + shlex.quote(name)
                    + "; test ! -e " + shlex.quote(name)
                    + "; printf '__OBSERVER_REMOVED__\\n'")
                try:
                    self.save()
                    result = remote.run(command, timeout_s=15)
                    if (result.exit_code or result.timed_out or result.truncated
                            or result.output.strip() not in ("__OBSERVER_REMOVED__", "__OBSERVER_ALREADY_ABSENT__")):
                        raise OSError("远端临时日志删除未确认")
                    meta["remote_deleted"] = True
                    self.save()
                except Exception as exc:
                    LOG.warning("P0日志回收 %s: %s，保留恢复路径: %s", endpoint, exc, meta["remote_path"])

    def finish(self):
        try:
            return self._finish()
        finally:
            self.cleanup_temporary(final=True)

    def _finish(self):
        if self.optional_collectors:
            from benchmark_telemetry import telemetry_call
            telemetry_call(self.optional_collectors, "finish")
        for tag in list(self.active):
            self.manifest["attempts"][tag].update(business_success=None, incomplete=True,
                                                business_error="controller_interrupted")
        if self.enabled:
            self.collect(final=True)
            for endpoint in self.remotes:
                if self.supported[endpoint]:
                    try:
                        self.clock_probe(endpoint)
                    except Exception as exc:
                        self.error(endpoint, exc)
        self.save()
        export(self.root, artifacts=True)


def recover_pending_cleanup(args, root, board, server, current_session):
    """Recover only journals bound to these hosts and configured directories."""
    for path in Path(root).glob("*/observability/manifest.json"):
        if (path.is_symlink() or path.parent.is_symlink() or path.parent.parent.is_symlink()
                or not path.resolve().is_relative_to(Path(root).resolve())):
            continue
        manifest = json.loads(path.read_text(encoding="utf-8"))
        state = manifest.get("cleanup", {})
        session = manifest.get("session_id", "")
        if state.get("version") != 1 or state.get("complete") or session == current_session:
            continue
        recovered = RoadtestObserver.__new__(RoadtestObserver)
        recovered.args, recovered.session, recovered.root = args, session, path.parent
        recovered.manifest = manifest
        recovered.remotes = {"board": board, "server": server}
        recovered.enabled = True
        recovered.paths = {"board": args.board_resource_dir.rstrip("/") + "/observer_" + session,
                           "server": args.server_resource_dir.rstrip("/") + "/observer_" + session}
        if state.get("hosts") != recovered._cleanup_hosts():
            continue
        try:
            handle = _claim_cleanup_lock(path.parent / "cleanup.lock")
        except OSError as exc:
            raise RuntimeError("旧会话控制机仍在运行，拒绝回收其产物：" + str(path)) from exc
        try:
            if not all(recovered._cleanup_idle(ep) for ep in recovered.remotes):
                raise RuntimeError("旧会话仍有活动进程，拒绝回收：" + str(path))
            if not recovered.cleanup_temporary(final=True):
                raise RuntimeError("旧会话临时文件尚未回收，拒绝开始新测试：" + str(path))
        finally:
            handle.close()


def create_observer(args, session, root, board, server):
    try:
        return RoadtestObserver(args, session, root, board, server)
    except Exception:
        LOG.exception("观测或临时文件回收清单初始化失败，尚未启动业务")
        raise


def observe(args, method, *values):
    observer = getattr(args, "_observer", None)
    if observer is None:
        if method == "track_temporary" or (not getattr(args, "disable_observer", False) and method in ("start", "end")):
            raise RuntimeError("P0 观测不可用，停止后续测试")
        return None
    try:
        return getattr(observer, method)(*values)
    except Exception:
        LOG.exception("P0观测 %s 失败；业务结果保持独立", method)
        if method in ("track_temporary", "finish") or (method == "start" and observer.enabled):
            raise
        if method == "end":
            return {"status": "INVALID", "errors": ["任务观测验收异常，详见控制机日志"]}
        return None


def _union(intervals):
    merged = []
    for start, end in sorted(intervals):
        if end < start:
            continue
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(end, merged[-1][1]))
        else:
            merged.append((start, end))
    return merged


def _write_csv(path, rows, defaults):
    fields = list(dict.fromkeys([*defaults, *(k for row in rows for k in row)]))
    with path.open("w", encoding="utf-8-sig", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: json.dumps(v, ensure_ascii=False) if isinstance(v, (list, dict)) else v
                             for k, v in row.items()})


def _writer_prefix(filename):
    for marker in (".transport.", ".events.", ".checkpoint."):
        if marker in filename:
            return filename.rsplit(marker, 1)[0]
    return filename.removesuffix(".health.json")


def _clock_valid(row, manifest):
    if row.get("observer_revision") != 2 or row.get("clock_domain") != "CLOCK_MONOTONIC":
        return False
    if any(type(row.get(k)) is not int or row[k] <= 0 for k in ("actual_mono_us", "realtime_us")):
        return False
    info = manifest.get("endpoints", {}).get(row.get("endpoint"), {})
    anchor = info.get("best_clock_before")
    if not anchor or anchor.get("clock_domain") != "CLOCK_MONOTONIC" or not info.get("alignment_valid", False):
        return False
    difference = (row["realtime_us"]-row["actual_mono_us"]) - (anchor["realtime_us"]-anchor["monotonic_us"])
    return abs(difference) <= max(100000, info.get("clock_mapping_drift_us", 0)+10000)


def _window_statistics(tables, manifest):
    grouped = defaultdict(list)
    clocks = manifest.get("endpoints", {})
    for row in tables["path_windows"]:
        key = tuple(row.get(k) for k in ("attempt_id", "endpoint", "conn_uid", "path_id", "mapping_generation"))
        grouped[key].append(row)
        row["module"] = {"rmnet_data0": "5G1", "usb0": "5G2"}.get(row.get("interface_name"), "UNKNOWN")
        row["mapping_basis"] = "local_interface" if row["module"] != "UNKNOWN" else "unknown"
        row["window_time_estimated"] = True
        row["scheduler_counter_scope"] = "connection; path_* counters are per physical path"
        anchor = clocks.get(row.get("endpoint"), {}).get("best_clock_before")
        if anchor and _clock_valid(row, manifest):
            row["aligned_controller_us"] = row.get("actual_mono_us", 0) + anchor["realtime_us"] - anchor["monotonic_us"] + anchor["controller_offset_us"]
            row["clock_uncertainty_us"] = clocks[row["endpoint"]].get("clock_error_bound_us", anchor.get("uncertainty_us"))
    for rows in grouped.values():
        rows.sort(key=lambda r: r.get("actual_mono_us", 0))
        for current, following in zip(rows, rows[1:]):
            dt = following.get("actual_mono_us", 0) - current.get("actual_mono_us", 0)
            period = current.get("interval_us") or following.get("interval_us") or manifest.get("interval_ms", 100)*1000
            valid = (0 < dt <= period*2.5 and current.get("observer_valid", True)
                     and following.get("observer_valid", True) and current.get("checksum_valid", True)
                     and following.get("checksum_valid", True) and current.get("total_utilization") is not None
                     and current.get("path_state") == 2 and following.get("path_state") == 2)
            current["time_weight_us"] = dt if valid else 0
            current["unknown_interval_us"] = 0 if valid else max(0, dt)
            for counter in ("fallback_real_inflight_count", "fallback_scheduled_reservation_count",
                            "fallback_real_inflight_bytes", "fallback_scheduled_reservation_bytes",
                            "no_sendable_calls", "both_sendable", "primary_only", "secondary_only", "neither_sendable"):
                a, b = current.get(counter), following.get(counter)
                current["connection_delta_"+counter] = b-a if isinstance(a, int) and isinstance(b, int) and b >= a else None
        if rows:
            rows[-1]["time_weight_us"] = 0


def _summarize_windows(rows, tag, endpoint):
    groups = defaultdict(list)
    for row in rows:
        if row.get("attempt_id") == tag and row.get("endpoint") == endpoint and row.get("path_state") == 2:
            groups[(row.get("conn_uid"), row.get("path_id"), row.get("mapping_generation"))].append(row)
    result = []
    for (conn, path, generation), values in groups.items():
        duration = max(r.get("actual_mono_us", 0) for r in values)-min(r.get("actual_mono_us", 0) for r in values)
        covered = sum(r.get("time_weight_us", 0) for r in values)
        record = dict(conn_uid=conn, path_id=path, mapping_generation=generation,
                      covered_us=covered, coverage_ratio=covered/duration if duration else None, estimated=True)
        for field in ("real_utilization", "scheduled_utilization", "total_utilization"):
            record["time_weighted_"+field] = sum(r.get(field, 0)*r.get("time_weight_us", 0)
                for r in values if r.get(field) is not None)/covered if covered else None
        record["total_at_least_90pct_time_ratio"] = sum(r.get("time_weight_us", 0) for r in values
            if r.get("total_utilization") is not None and r["total_utilization"] >= 0.9)/covered if covered else None
        result.append(record)
    return result


def _secondary_shares(rows, tag, endpoint):
    groups = defaultdict(lambda: {"primary": 0, "secondary": 0})
    for row in rows:
        if row.get("attempt_id") != tag or row.get("endpoint") != endpoint or not row.get("role_delta_valid"):
            continue
        role, epoch = row.get("delta_role"), row.get("delta_role_epoch")
        if role in ("primary", "secondary") and epoch is not None:
            groups[(row.get("conn_uid"), epoch)][role] += row.get("delta_stream_packet_sent_bytes", 0)
    return [dict(conn_uid=conn, role_epoch=epoch, primary_stream_packet_bytes=value["primary"],
                 secondary_stream_packet_bytes=value["secondary"],
                 actual_secondary_stream_packet_share_pct=100*value["secondary"]/sum(value.values())
                 if sum(value.values()) else None,
                 basis="encoded_STREAM_or_DATAGRAM_packets_including_repeats")
            for (conn, epoch), value in groups.items()]


def _artifacts(root, tables, manifest):
    errors = []
    try:
        from openpyxl import Workbook
        workbook = Workbook(write_only=True)
        for name, rows in tables.items():
            fields = list(dict.fromkeys(["attempt_id", "endpoint", *(k for r in rows for k in r)]))
            sheet = workbook.create_sheet(name[:31])
            sheet.append(fields)
            for row in rows:
                values = []
                for field in fields:
                    value = row.get(field)
                    if isinstance(value, (dict, list)):
                        value = json.dumps(value, ensure_ascii=False)
                    elif isinstance(value, int) and abs(value) > 2**53:
                        value = str(value)
                    # Treat strings from captured logs as text, never Excel formulas.
                    if isinstance(value, str) and value.startswith(("=", "+", "-", "@")):
                        value = "'" + value
                    values.append(value)
                sheet.append(values)
        workbook.save(root / "observability.xlsx")
    except Exception as exc:
        errors.append("Excel: " + str(exc))
    try:
        _plots(root, tables, manifest)
    except Exception as exc:
        errors.append("plots: " + str(exc))
    (root / "artifact_status.json").write_text(json.dumps({"errors": errors}, ensure_ascii=False, indent=2), encoding="utf-8")


def _plots(root, tables, manifest):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    for tag, attempt in manifest.get("attempts", {}).items():
        direction = attempt.get("direction")
        sender, receiver = ("server", "board") if direction == "get" else ("board", "server")
        paths = [r for r in tables["path_windows"] if r.get("attempt_id") == tag and r.get("endpoint") == sender]
        streams = [r for r in tables["rx_hol_streams"] if r.get("attempt_id") == tag and r.get("endpoint") == receiver]
        if not paths and not streams:
            continue
        anchors = manifest.get("endpoints", {})
        synchronized = bool(paths) and bool(streams) and all(_clock_valid(r, manifest) for r in [*paths, *streams])
        def stamp(row, endpoint):
            mono = row.get("actual_mono_us", 0)
            anchor = anchors.get(endpoint, {}).get("best_clock_before")
            return mono+anchor["realtime_us"]-anchor["monotonic_us"]+anchor["controller_offset_us"] if synchronized else mono
        receiver_start = next((r.get("receiver_task_start_us") for r in tables["transfer_observability_summary"]
                               if r.get("attempt_id") == tag), None)
        origins = {sender: min((stamp(r, sender) for r in paths), default=0),
                   receiver: min((stamp(r, receiver) for r in streams), default=0)}
        if receiver_start is not None:
            origins[receiver] = stamp({"actual_mono_us": receiver_start}, receiver)
        origin = min(origins.values()) if synchronized else None
        def seconds(row, endpoint):
            return (stamp(row, endpoint)-(origin if synchronized else origins[endpoint]))/1e6
        figure, axes = plt.subplots(4, 1, figsize=(13, 11), sharex=True)
        try:
            for key in dict.fromkeys((r.get("conn_uid"), r.get("stream_id")) for r in streams):
                values = [r for r in streams if (r.get("conn_uid"), r.get("stream_id")) == key]
                axes[0].plot([seconds(r, receiver) for r in values], [r.get("reorder_pending_bytes", 0) for r in values], alpha=0.5)
            labeled = 0
            for episode in tables["rx_hol_episodes"]:
                if episode.get("attempt_id") == tag:
                    start = seconds(dict(episode, actual_mono_us=episode["start_us"]), receiver)
                    end = seconds(dict(episode, actual_mono_us=episode["end_us"]), receiver)
                    axes[0].axvspan(start, end, color="red", alpha=0.08)
                    if episode.get("peak_bytes") and labeled < 6:
                        axes[0].annotate(f"peak {episode['peak_bytes']} B", (start, episode["peak_bytes"]), fontsize=8)
                        labeled += 1
            peaks = [r.get("peak_bytes", 0) for r in tables["rx_hol_episodes"] if r.get("attempt_id") == tag]
            sampled_peak = max((r.get("reorder_pending_bytes", 0) for r in streams), default=0)
            axes[0].set_ylim(0, max([1, sampled_peak, *peaks])*1.15)
            axes[0].set_ylabel("Pending STREAM B")
            body_axis = axes[0].twinx()
            for key in dict.fromkeys((r.get("conn_uid"), r.get("stream_id")) for r in streams):
                values = sorted((r for r in streams if (r.get("conn_uid"), r.get("stream_id")) == key), key=lambda r: r["actual_mono_us"])
                xs, ys = [], []
                for a, b in zip(values, values[1:]):
                    dt = b["actual_mono_us"]-a["actual_mono_us"]
                    if dt > 0:
                        xs.append(seconds(b, receiver)); ys.append(8*(b.get("body_received_bytes", 0)-a.get("body_received_bytes", 0))/dt)
                body_axis.plot(xs, ys, color="green", alpha=0.35)
                if not xs and values:
                    first, last = values[-1].get("first_body_us"), values[-1].get("last_body_us")
                    if first and last and last > first:
                        body_axis.hlines(8*values[-1].get("body_received_bytes", 0)/(last-first),
                            seconds({"actual_mono_us": first}, receiver), seconds({"actual_mono_us": last}, receiver),
                            color="green", linestyles="--", label="body interval mean")
            body_axis.set_ylabel("Body Mbps / stream")
            for axis, path_id in zip(axes[1:3], list(dict.fromkeys(r.get("path_id") for r in paths))[:2]):
                values = [r for r in paths if r.get("path_id") == path_id]
                x = [seconds(r, sender) for r in values]
                inflight = [r.get("inflight_bytes", 0) or 0 for r in values]
                reserved = [r.get("scheduled_bytes", 0) or 0 for r in values]
                axis.stackplot(x, inflight, reserved, labels=["inflight", "scheduled"], alpha=0.5)
                axis.plot(x, [r.get("cwnd_bytes") for r in values], label="raw CWND")
                axis.plot(x, [r.get("effective_cwnd_bytes") for r in values], label="effective CWND")
                for r in values:
                    if r.get("sample_kind") == "role_change":
                        axis.axvline(seconds(r, sender), color="black", linestyle=":", alpha=0.5)
                axis.set_ylabel(f"Path {path_id} B"); axis.legend(loc="upper right", ncol=4)
            values = list({(r.get("conn_uid"), r.get("sample_seq"), r.get("schema_family")): r for r in paths}.values())
            role_values = [r for r in values if r.get("primary_valid")
                           and r.get("secondary_path_id") is not None]
            if role_values:
                axes[3].plot([seconds(r, sender) for r in values],
                    [r.get("target_secondary_pct") if r.get("primary_valid")
                     and r.get("secondary_path_id") is not None else float("nan") for r in values],
                    label="target secondary %")
            else:
                axes[3].text(0.5, 0.5, "Secondary share: N/A (no primary/secondary pair)",
                             ha="center", va="center", transform=axes[3].transAxes)
            if values and any(r.get("cwnd_fallback_selected_bytes") is not None for r in values):
                fallback_axis = axes[3].twinx()
                baseline = next(r["cwnd_fallback_selected_bytes"] for r in values if r.get("cwnd_fallback_selected_bytes") is not None)
                fallback_axis.plot([seconds(r, sender) for r in values],
                    [r.get("cwnd_fallback_selected_bytes", baseline)-baseline for r in values],
                    color="red", alpha=0.4)
                fallback_axis.set_ylabel("CWND fallback selected B")
            shares = _secondary_shares(paths, tag, sender)
            for share in shares:
                epoch_rows = [r for r in paths if r.get("role_epoch") == share["role_epoch"]]
                if epoch_rows and share["actual_secondary_stream_packet_share_pct"] is not None:
                    axes[3].hlines(share["actual_secondary_stream_packet_share_pct"],
                        min(seconds(r, sender) for r in epoch_rows), max(seconds(r, sender) for r in epoch_rows),
                        color="orange", label="actual encoded packet share %")
            axes[3].set_ylabel("Secondary %")
            if axes[3].get_legend_handles_labels()[0]:
                axes[3].legend(loc="upper right")
            axes[3].set_xlabel("Aligned seconds" if synchronized else "Seconds from each endpoint start; clocks NOT aligned")
            for axis in axes:
                axis.grid(alpha=0.2)
            uncertainty = sum((anchors.get(e, {}).get("clock_error_bound_us",
                               anchors.get(e, {}).get("best_clock_before", {}).get("uncertainty_us", 0))
                               for e in (sender, receiver)))
            quality = "COARSE alignment; no sub-100 ms ordering" if uncertainty > 100000 else "aligned"
            figure.suptitle(f"{tag}\n{quality}; combined clock uncertainty <= {uncertainty/1000:.3f} ms" if synchronized else tag+"\nCross-end timing unavailable")
            figure.tight_layout()
            target = root / "plots"; target.mkdir(exist_ok=True)
            filename = tag if Path(tag).name == tag and tag not in (".", "..") else hashlib.sha256(tag.encode()).hexdigest()
            figure.savefig(target / (filename+".png"), dpi=130)
        finally:
            plt.close(figure)


def _checkpoint_errors(receipt, rows, root, manifest):
    errors = []
    if any(type(receipt.get(k)) is not int or receipt[k] < 0 for k in (
            "written_records", "transport_records", "event_records", "transport_seq", "event_seq",
            "dropped_records", "writer_error", "sealed_transport_segments", "sealed_event_segments")):
        errors.append("checkpoint_numeric_fields_invalid")
    if (receipt.get("observer_revision") != 2 or receipt.get("clock_domain") != "CLOCK_MONOTONIC"
            or not receipt.get("checksum_valid") or receipt.get("task_complete") is not True
            or receipt.get("observer_valid") is not True or not receipt.get("attempt_id")
            or receipt.get("writer_error") != 0 or receipt.get("dropped_records") != 0):
        errors.append("checkpoint_invalid_or_writer_loss")
    info = manifest.get("endpoints", {}).get(receipt.get("endpoint"), {})
    if receipt.get("boot_id") != info.get("best_clock_before", {}).get("boot_id"):
        errors.append("checkpoint_boot_mismatch")
    total = 0
    for family, field, count_field, segment_field, filename in (
            ("transport", "transport_seq", "transport_records", "sealed_transport_segments", "transport"),
            ("observer_events", "event_seq", "event_records", "sealed_event_segments", "events")):
        limit = receipt.get(field)
        if type(limit) is not int or limit < 0:
            errors.append("checkpoint_sequence_invalid")
            continue
        values = [r["sample_seq"] for r in rows if r.get("schema_family") == family and r.get("sample_seq", 0) <= limit]
        ordered_rows = sorted((r for r in rows if r.get("schema_family") == family
                               and r.get("sample_seq", 0) <= limit), key=lambda r: r["sample_seq"])
        if any(b["actual_mono_us"] < a["actual_mono_us"] for a, b in zip(ordered_rows, ordered_rows[1:])):
            errors.append(family + "_monotonic_clock_went_backwards")
        if (len(values) != limit or len(set(values)) != limit or (values and (min(values) != 1 or max(values) != limit))
                or receipt.get(count_field) != limit):
            errors.append(family + "_checkpoint_sequence_or_count_mismatch")
        total += len(values)
        count = receipt.get(segment_field)
        prefix = receipt["writer_prefix"]
        segments = list(root.glob(prefix + "." + filename + ".*.jsonl"))
        indices = {int(p.name.rsplit(".", 2)[1]) for p in segments if p.name.rsplit(".", 2)[1].isdigit()}
        if type(count) is not int or count < 0 or not set(range(min(count, len(indices)+1))).issubset(indices):
            errors.append(family + "_sealed_segments_missing")
    if receipt.get("written_records") != total:
        errors.append("checkpoint_written_count_mismatch")
    return errors


def _mapping_completed(earlier, later):
    """A GET's unknown length may be resolved by its later header binding."""
    identity = ("attempt_id", "endpoint", "conn_uid", "stream_id", "direction",
                "request_index", "file_offset")
    if (earlier.get("sample_kind") not in ("stream_bind", "stream_snapshot")
            or later.get("sample_kind") != "stream_bind" or earlier.get("direction") != "get"
            or earlier.get("request_index") != 0 or earlier.get("file_offset") != 0
            or type(earlier.get("fragment_length")) is not int or earlier["fragment_length"] != 0
            or type(later.get("fragment_length")) is not int or later["fragment_length"] <= 0
            or any(earlier.get(k) is None or earlier[k] != later.get(k) for k in identity)):
        return False
    for field in ("sample_seq", "actual_mono_us"):
        if (type(earlier.get(field)) is not int or type(later.get(field)) is not int
                or earlier[field] <= 0 or earlier[field] > later[field]
                or (field == "sample_seq" and earlier[field] == later[field])):
            return False
    if earlier["sample_kind"] == "stream_snapshot":
        # STREAM data may be buffered out of order before response headers are
        # readable. Preserve its HoL accounting when the first GET length resolves.
        return all(type(earlier.get(k)) is int and earlier[k] == 0 for k in (
            "body_received_bytes", "first_body_us", "last_body_us"))
    return True


def export(root, artifacts=False):
    root = Path(root)
    manifest_path = root / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8")) if manifest_path.exists() else {}
    records = []
    checkpoints = []
    health = [dict(status="PARTIAL", **r) if "status" not in r else dict(r)
              for r in manifest.get("errors", [])]
    file_metadata = {v["local_path"].replace("\\", "/"): v for v in manifest.get("files", {}).values()}
    for path in sorted((root / "raw").glob("*/*")):
        rel = path.relative_to(root).as_posix()
        meta = file_metadata.get(rel, {})
        endpoint = meta.get("endpoint", path.parent.name)
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        checksum_valid = bool(meta) and digest == meta.get("sha256")
        if ".checkpoint." in path.name and path.name.endswith(".json"):
            try:
                receipt = json.loads(path.read_text(encoding="utf-8"))
                if not isinstance(receipt, dict):
                    raise ValueError("检查点必须是 JSON 对象")
                if any(not isinstance(receipt.get(k), str) or not receipt[k]
                       for k in ("attempt_id", "conn_uid", "writer_prefix")):
                    raise ValueError("检查点缺少有效的 attempt/连接/writer 标识")
                remote_prefix = _writer_prefix(meta.get("remote_path", ""))
                if remote_prefix and receipt.get("writer_prefix") != remote_prefix:
                    raise ValueError("检查点 writer_prefix 与归档来源不一致")
                receipt.update(file=rel, endpoint=endpoint, checksum_valid=checksum_valid,
                               writer_prefix=_writer_prefix(rel))
                checkpoints.append(receipt)
            except (ValueError, TypeError, OSError) as exc:
                health.append(dict(file=rel, endpoint=endpoint, status="INVALID", reason=str(exc)))
            continue
        if path.name.endswith(".health.json"):
            try:
                status = json.loads(path.read_text(encoding="utf-8"))
                if not isinstance(status, dict):
                    raise ValueError("健康记录必须是 JSON 对象")
                if "writer_prefix" in status:
                    prefix = status["writer_prefix"]
                    remote_prefix = _writer_prefix(meta.get("remote_path", ""))
                    if not isinstance(prefix, str) or not prefix or (remote_prefix and prefix != remote_prefix):
                        raise ValueError("健康记录 writer_prefix 无效或与归档来源不一致")
                status.update(file=rel, endpoint=endpoint, checksum_valid=checksum_valid,
                              writer_prefix=_writer_prefix(rel),
                              status="INVALID" if status.get("writer_error") or status.get("dropped_records")
                              or not checksum_valid else "OK")
                health.append(status)
            except (ValueError, OSError) as exc:
                health.append(dict(file=rel, endpoint=endpoint, status="INVALID", reason=str(exc)))
            continue
        if ".jsonl" not in path.name:
            continue
        with path.open(encoding="utf-8", errors="replace") as source:
            for line_number, line in enumerate(source, 1):
                try:
                    row = json.loads(line)
                    if not isinstance(row, dict):
                        raise ValueError("记录必须是 JSON 对象")
                    family = row.get("schema_family", "transport")
                    version = row.get("schema_version")
                    if family not in ("transport", "observer_events") or version not in (
                            (1, 2) if family == "transport" else (1,)):
                        raise ValueError("未知 schema family/version")
                    if family == "transport" and not isinstance(row.get("paths"), list):
                        raise ValueError("transport.paths 必须是数组")
                    for key in ("sample_seq", "actual_mono_us", "realtime_us", "stream_id"):
                        if key in row and (type(row[key]) is not int or row[key] < 0):
                            raise ValueError("无效无符号整数字段：" + key)
                    if not isinstance(row.get("conn_uid", ""), str) or not isinstance(row.get("attempt_id", ""), str):
                        raise ValueError("无效连接/attempt 标识")
                    for key in ("start_us", "end_us", "duration_us", "hol_pending_us", "hol_exposed_us",
                                "reorder_byte_us", "reorder_pending_peak_bytes", "body_received_bytes"):
                        if key in row and (type(row[key]) is not int or row[key] < 0):
                            raise ValueError("无效累计量/事件字段：" + key)
                    for path_row in row.get("paths", []):
                        if not isinstance(path_row, dict):
                            raise ValueError("path 必须是对象")
                        for key, value in path_row.items():
                            if (key.endswith("_bytes") or key in ("path_id", "mapping_generation")) and value is not None:
                                if type(value) is not int or value < 0:
                                    raise ValueError("无效路径计数：" + key)
                    if row.get("observer_revision") == 2:
                        required = []
                        if row.get("sample_kind") in ("stream_snapshot", "stream_end"):
                            required = ["stream_id", "C", "N", "reorder_pending_bytes", "readable_unread_bytes",
                                        "hol_pending_us", "hol_exposed_us", "reorder_byte_us", "reorder_pending_peak_bytes",
                                        "hol_episode_count", "first_body_us", "last_body_us", "observed_from_us",
                                        "body_received_bytes", "file_offset", "fragment_length", "request_index"]
                        if any(type(row.get(k)) is not int or row[k] < 0 for k in required):
                            raise ValueError("流统计缺少有效的必需字段")
                        for path_row in row.get("paths", []):
                            if any(type(path_row.get(k)) is not int or path_row[k] < 0 for k in (
                                    "path_id", "mapping_generation", "cwnd_bytes", "effective_cwnd_bytes",
                                    "inflight_bytes", "scheduled_bytes", "occupied_bytes", "headroom_bytes",
                                    "wire_sent_bytes", "stream_packet_sent_bytes", "retrans_sent_bytes",
                                    "reinject_sent_bytes", "delivered_quic_bytes")):
                                raise ValueError("路径统计缺少有效的必需字段")
                            if (path_row["occupied_bytes"] != path_row["inflight_bytes"]+path_row["scheduled_bytes"]
                                    or path_row["headroom_bytes"] != max(0, path_row["effective_cwnd_bytes"]-path_row["occupied_bytes"])):
                                raise ValueError("路径窗口公式不一致")
                    row.update(endpoint=endpoint, raw_file=rel,
                               writer_prefix=_writer_prefix(rel),
                               file_incomplete=path.name.endswith(".active"), checksum_valid=checksum_valid)
                    records.append(row)
                except (ValueError, TypeError, AttributeError) as exc:
                    health.append(dict(file=rel, line=line_number, endpoint=endpoint,
                                       status="INVALID", reason=str(exc)))
    bindings = {}
    connection_attempt = defaultdict(list)
    for row in records:
        key = (row.get("endpoint"), row.get("conn_uid"))
        if row.get("sample_kind") == "stream_bind":
            binding_key = (*key, row.get("stream_id"))
            previous_binding = bindings.get(binding_key)
            if previous_binding and not _mapping_completed(previous_binding, row) and any(previous_binding.get(k) != row.get(k) for k in ("attempt_id", "direction", "request_index", "file_offset", "fragment_length")):
                health.append(dict(attempt_id=row.get("attempt_id"), endpoint=key[0], conn_uid=key[1], status="INVALID", reason="conflicting_stream_mapping"))
            bindings[binding_key] = row
            connection_attempt[key].append((row.get("actual_mono_us", 0), row.get("attempt_id")))
    for row in records:
        if row.get("sample_kind") not in ("stream_snapshot", "stream_end") or not row.get("bound"):
            continue
        key = (row.get("endpoint"), row.get("conn_uid"), row.get("stream_id"))
        fields = ("attempt_id", "direction", "request_index", "file_offset", "fragment_length")
        if not row.get("attempt_id") or row.get("direction") not in ("get", "post") or any(row.get(k) is None for k in fields):
            continue
        if key not in bindings:
            bindings[key] = dict(row)
            connection_attempt[key[:2]].append((row.get("actual_mono_us", 0), row["attempt_id"]))
            health.append(dict(attempt_id=row["attempt_id"], endpoint=key[0], conn_uid=key[1],
                               status="PARTIAL", reason="missing_stream_bind; recovered_from_stream_record"))
        elif not _mapping_completed(row, bindings[key]) and any(bindings[key].get(k) != row.get(k) for k in fields):
            health.append(dict(attempt_id=row["attempt_id"], endpoint=key[0], conn_uid=key[1],
                               status="INVALID", reason="conflicting_stream_mapping"))
    for key in connection_attempt:
        connection_attempt[key].sort()
    for row in records:
        key = (row.get("endpoint"), row.get("conn_uid"))
        bind = bindings.get((*key, row.get("stream_id")))
        if bind:
            for name in ("attempt_id", "direction", "request_index", "file_offset", "fragment_length"):
                row[name] = bind.get(name)
        elif not row.get("attempt_id") and connection_attempt.get(key):
            timeline = connection_attempt[key]
            candidates = [tag for stamp, tag in timeline if stamp <= row.get("actual_mono_us", 0)]
            row["attempt_id"] = candidates[-1] if candidates else timeline[0][1]
    writers = defaultdict(list)
    for row in records:
        writers[row["writer_prefix"]].append(row)
    checkpoint_index = {}
    for receipt in checkpoints:
        prefix = receipt["writer_prefix"]
        errors = _checkpoint_errors(receipt, writers.get(prefix, []), root, manifest)
        receipt.update(status="INVALID" if errors else "OK", reason="; ".join(errors),
                       attempt_ids=[receipt.get("attempt_id")], checkpoint=True)
        health.append(receipt)
        key = (receipt.get("attempt_id"), receipt["endpoint"], receipt.get("conn_uid"))
        if key in checkpoint_index:
            receipt.update(status="INVALID", reason="duplicate_task_checkpoint")
        checkpoint_index[key] = receipt
    writer_health = {r.get("writer_prefix"): r for r in health if r.get("writer_prefix") and not r.get("checkpoint")}
    for prefix, rows in writers.items():
        status = writer_health.get(prefix)
        tags = {r.get("attempt_id") for r in rows if r.get("attempt_id")}
        if status:
            status["attempt_ids"] = sorted(tags)
            if status.get("written_records") != len(rows):
                status.update(status="INVALID" if status["status"] == "INVALID" else "PARTIAL", reason="writer_record_count_mismatch")
        elif not any(r["writer_prefix"] == prefix for r in checkpoints):
            health.append(dict(endpoint=rows[0].get("endpoint"), writer_prefix=prefix,
                               attempt_ids=sorted(tags), status="PARTIAL", reason="missing_writer_health"))
    records = [r for r in records if not (
        (cp := checkpoint_index.get((r.get("attempt_id"), r.get("endpoint"), r.get("conn_uid"))))
        and cp.get("status") == "OK"
        and r.get("sample_seq", 0) > cp.get("transport_seq" if r.get("schema_family") == "transport" else "event_seq", 0))]
    # Bad raw lines inherit the attempt of the retained file when known.
    for item in health:
        if item.get("file") and "attempt_ids" not in item:
            tags = {r.get("attempt_id") for r in records if r["raw_file"] == item["file"] and r.get("attempt_id")}
            if tags:
                item["attempt_ids"] = sorted(tags)
    tables = {name: [] for name in TABLES}
    samples = defaultdict(list)
    episodes = defaultdict(list)
    final_streams = {}
    stream_rows = defaultdict(list)
    sequences = defaultdict(list)
    seen = set()
    for row in sorted(records, key=lambda r: (r.get("actual_mono_us", 0), r.get("sample_seq", 0))):
        endpoint, conn = row.get("endpoint"), row.get("conn_uid")
        tag, kind = row.get("attempt_id"), row.get("sample_kind")
        family = row.get("schema_family", "transport")
        sequence_key = (endpoint, conn, family)
        seq = row.get("sample_seq")
        identity = (*sequence_key, seq)
        if seq is not None:
            if identity in seen:
                health.append(dict(endpoint=endpoint, conn_uid=conn, status="INVALID", reason="duplicate_sequence"))
                continue
            seen.add(identity)
            sequences[sequence_key].append(seq)
        if family == "transport" or kind in ("role_change", "control_update"):
            if kind in ("role_change", "control_update"):
                row["scheduler_name"] = "adaptive"
            samples[(tag, endpoint, conn)].append(row)
        if kind == "observer_error":
            health.append(dict(attempt_id=tag, endpoint=endpoint, conn_uid=conn,
                               status="INVALID", reason=row.get("reason")))
        sid = row.get("stream_id")
        bound = bindings.get((endpoint, conn, sid))
        direction = row.get("direction")
        receiver = endpoint == ("board" if direction == "get" else "server")
        if bound and receiver and kind in ("stream_snapshot", "stream_end"):
            copy = dict(row)
            copy["bound"] = True
            limit, used = copy.get("stream_send_limit_bytes"), copy.get("stream_send_offset_bytes")
            copy["stream_send_credit_bytes"] = max(limit-used, 0) if isinstance(limit, int) and isinstance(used, int) else None
            tables["rx_hol_streams"].append(copy)
            final_streams[(tag, endpoint, conn, sid)] = copy
            stream_rows[(tag, endpoint, conn, sid)].append(copy)
        if bound and receiver and kind == "hol_episode":
            tables["rx_hol_episodes"].append(dict(row))
            episodes[tag].append(row)
    # Detect missing START/END, gaps, observer failures, and counter resets explicitly.
    for (endpoint, conn, family), seqs in sequences.items():
        ordered = sorted(seqs)
        gaps = sum(max(0, b-a-1) for a, b in zip(ordered, ordered[1:])) + max(0, ordered[0]-1)
        if gaps:
            health.append(dict(endpoint=endpoint, conn_uid=conn, schema_family=family,
                               status="PARTIAL", sequence_gaps=gaps))
    for key, rows in samples.items():
        tag, endpoint, conn = key
        start, end = any(r.get("sample_kind") == "start" for r in rows), any(
            r.get("sample_kind") == "end" for r in rows)
        valid = all(r.get("observer_valid", True) and r["checksum_valid"] and not r["file_incomplete"] for r in rows)
        health.append(dict(attempt_id=tag, endpoint=endpoint, conn_uid=conn, start_present=start,
                           end_present=end, status="OK" if start and end and valid else "PARTIAL"))
        previous = {}
        for row in rows:
            for path in row.get("paths", []):
                record = {k: v for k, v in row.items() if k != "paths"}
                record.update(path)
                limit, used = row.get("connection_send_limit_bytes"), row.get("connection_sent_offset_bytes")
                record["connection_credit_bytes"] = max(limit-used, 0) if isinstance(limit, int) and isinstance(used, int) else None
                record["connection_credit_scope"] = "shared_connection"
                eff, inflight, scheduled = (path.get(k) for k in (
                    "effective_cwnd_bytes", "inflight_bytes", "scheduled_bytes"))
                supported = all(isinstance(v, int) and v >= 0 for v in (eff, inflight, scheduled))
                record["utilization_reason"] = None if supported and eff else "unsupported_or_zero_effective_cwnd"
                for name, numerator in (("real_utilization", inflight), ("scheduled_utilization", scheduled),
                                        ("total_utilization", inflight+scheduled if supported else None)):
                    record[name] = numerator/eff if supported and eff else None
                record["role"] = ("not_applicable" if row.get("scheduler_name") != "adaptive" else
                                  "unassigned" if not row.get("primary_valid") else
                                  "primary" if path.get("path_id") == row.get("primary_path_id") else
                                  "secondary" if path.get("path_id") == row.get("secondary_path_id") else "other")
                pkey = (path.get("path_id"), path.get("mapping_generation"))
                old = previous.get(pkey)
                record["delta_valid"] = False
                if old:
                    dt = row.get("actual_mono_us", 0)-old.get("actual_mono_us", 0)
                    record["delta_us"] = dt
                    same_epoch = old.get("role_epoch") == row.get("role_epoch")
                    counters = [k for k in path if (k.endswith("sent_bytes") or k.endswith("selected_bytes")
                                or k.endswith("selected_count") or k == "delivered_quic_bytes")
                                and isinstance(path[k], int)]
                    paired = [k for k in counters if isinstance(old.get(k), int)]
                    reset = any(path[k] < old[k] for k in paired)
                    record["delta_valid"] = dt > 0 and not reset
                    record["role_delta_valid"] = ((same_epoch or row.get("sample_kind") == "role_change")
                        and record["delta_valid"] and "stream_packet_sent_bytes" in paired
                        and old.get("observer_valid", True) and row.get("observer_valid", True))
                    record["delta_role_epoch"] = old.get("role_epoch")
                    record["delta_role"] = old.get("role")
                    if record["delta_valid"]:
                        for counter in paired:
                            record["delta_"+counter] = path[counter]-old[counter]
                    if reset:
                        health.append(dict(attempt_id=tag, endpoint=endpoint, conn_uid=conn,
                                           status="PARTIAL", reason="counter_reset"))
                previous[pkey] = dict(record)
                tables["path_windows"].append(record)
    _window_statistics(tables, manifest)
    tables["observer_health"].extend(health)
    for tag, attempt in manifest.get("attempts", {}).items():
        direction = attempt.get("direction")
        receiver = "board" if direction == "get" else "server"
        sender = "server" if direction == "get" else "board"
        streams = [r for (t, ep, _, _), r in final_streams.items() if t == tag and ep == receiver]
        transfer_samples = [r for (t, ep, _), rs in samples.items() if t == tag and ep == receiver for r in rs]
        starts = [r["actual_mono_us"] for r in transfer_samples if r.get("sample_kind") == "start"]
        ends = [r["actual_mono_us"] for r in transfer_samples if r.get("sample_kind") == "end"]
        observed = max(ends)-min(starts) if starts and ends else None
        intervals = [(r["start_us"], r["end_us"]) for r in episodes[tag]]
        union = _union(intervals)
        if starts and ends:
            union = _union((max(a, min(starts)), min(b, max(ends))) for a, b in union)
        any_us = sum(b-a for a, b in union)
        complete = sorted(r["duration_us"] for r in episodes[tag] if not r.get("censored"))
        summary = dict(attempt_id=tag, direction=direction, receiver=receiver, sender=sender,
            profile=attempt.get("profile"), experiment_phase=attempt.get("phase"), mode=attempt.get("mode"),
            phase="task", observed_transfer_us=observed, receiver_task_start_us=min(starts) if starts else None,
            receiver_task_end_us=max(ends) if ends else None, target_stream_count=len(streams),
            any_stream_hol_us=any_us if streams else None,
            any_stream_hol_ratio=any_us/observed if streams and observed else None,
            sum_stream_hol_pending_us=sum(r["hol_pending_us"] for r in streams) if streams else None,
            sum_stream_hol_exposed_us=sum(r["hol_exposed_us"] for r in streams) if streams else None,
            reorder_byte_us=sum(r["reorder_byte_us"] for r in streams) if streams else None,
            max_stream_pending_bytes=max((r["reorder_pending_peak_bytes"] for r in streams), default=None),
            body_received_bytes=sum(r["body_received_bytes"] for r in streams) if streams else None,
            censored_episode_count=sum(bool(r.get("censored")) for r in episodes[tag]),
            max_completed_episode_us=max(complete, default=None),
            completed_episode_p50_us=complete[max(0, (len(complete)*50+99)//100-1)] if complete else None,
            completed_episode_p95_us=complete[max(0, (len(complete)*95+99)//100-1)] if complete else None,
            hol_bytes_basis="target_request_STREAM_bytes_including_H3_framing",
            body_bytes_basis="unique_H3_body_bytes_per_request",
            estimated_window_time=True, business_success=attempt.get("business_success"))
        first_body = min((r.get("first_body_us", 0) for r in streams if r.get("first_body_us")), default=None)
        last_body = max((r.get("last_body_us", 0) for r in streams if r.get("last_body_us")), default=None)
        summary.update(first_body_us=first_body, last_body_us=last_body,
                       receiver_body_fct_us=last_body-first_body if first_body is not None and last_body is not None else None,
                       receiver_fct_metric="first_body_to_last_body",
                       old_client_metric="first_body_to_last_body" if direction == "get" else "app_submit_not_remote_receive")
        final_transport = max((r for r in transfer_samples if r.get("schema_family", "transport") == "transport"),
                              key=lambda r: r.get("actual_mono_us", 0), default={})
        for field in ("sum_B_peak_bytes", "aggregate_from_us", "aggregate_prebind_unobserved", "aggregate_valid"):
            summary[field] = final_transport.get(field)
        sender_samples = [r for (t, ep, _), rs in samples.items() if t == tag and ep == sender for r in rs
                          if r.get("schema_family", "transport") == "transport"]
        if sender_samples:
            initial, final = sender_samples[0], sender_samples[-1]
            reasons = ("warmup", "primary", "probe", "cwnd_fallback", "reinject_fallback", "best_fallback", "single", "original")
            selected = sum(max(0, final.get(k+"_selected_bytes", 0)-initial.get(k+"_selected_bytes", 0)) for k in reasons)
            fallback = final.get("cwnd_fallback_selected_bytes", 0)-initial.get("cwnd_fallback_selected_bytes", 0)
            summary["cwnd_fallback_selected_share"] = fallback/selected if selected and fallback >= 0 else None
        summary["window_statistics"] = _summarize_windows(tables["path_windows"], tag, sender)
        summary["actual_secondary_shares"] = _secondary_shares(tables["path_windows"], tag, sender)
        for event in records:
            if event.get("attempt_id") == tag and event.get("sample_kind") == "phase_end":
                phase = event.get("phase_name", "unknown")
                if phase in ("client_confirmed", "client_failed"):
                    summary["client_task_to_confirmation_us"] = event.get("cost_us")
                endpoint = event.get("endpoint")
                summary[endpoint+"_"+phase+"_at_us"] = event.get("actual_mono_us")
                summary[endpoint+"_phase_clock_domain"] = event.get("clock_domain")
                if phase != "task_end" or endpoint == receiver:
                    summary[phase+"_at_us"] = event.get("actual_mono_us")
        for endpoint, role in ((receiver, "receiver"), (sender, "sender")):
            conns = {conn for (t, ep, conn) in samples if t == tag and ep == endpoint}
            h = [r for r in health if (r.get("attempt_id") == tag or tag in r.get("attempt_ids", [])
                 or (not r.get("attempt_id") and r.get("conn_uid") in conns)) and r.get("endpoint") == endpoint]
            unsupported = endpoint in manifest.get("endpoints", {}) and not manifest["endpoints"][endpoint].get("capabilities")
            summary[role+"_observer_status"] = "UNSUPPORTED" if unsupported else "OK" if h and all(r["status"] == "OK" for r in h) else "PARTIAL"
        summary["hol_valid"] = bool(streams) and summary["receiver_observer_status"] == "OK" and all(r.get("stream_valid", True) for r in streams)
        summary["window_valid"] = summary["sender_observer_status"] == "OK"
        reasons = []
        transfer_records = [r for r in records if r.get("attempt_id") == tag]
        for endpoint, role in ((receiver, "receiver"), (sender, "sender")):
            endpoint_records = [r for r in transfer_records if r.get("endpoint") == endpoint]
            if not endpoint_records or not all(_clock_valid(r, manifest) for r in endpoint_records):
                reasons.append(role + "_clock_domain_or_anchor_invalid")
            conns = {r.get("conn_uid") for r in transfer_records if r.get("endpoint") == endpoint}
            if not conns or any((tag, endpoint, conn) not in checkpoint_index for conn in conns):
                reasons.append(role + "_checkpoint_missing")
            if summary[role+"_observer_status"] != "OK":
                reasons.append(role + "_observer_incomplete")
                conns = {conn for (t, ep, conn) in samples if t == tag and ep == endpoint}
                for item in health:
                    if (item.get("endpoint") != endpoint or item.get("status") == "OK"
                            or not (item.get("attempt_id") == tag or tag in item.get("attempt_ids", [])
                                    or (not item.get("attempt_id") and item.get("conn_uid") in conns))):
                        continue
                    detail = item.get("reason") or ",".join(
                        f"{key}={item[key]}" for key in ("status", "start_present", "end_present",
                            "sequence_gaps", "dropped_records", "writer_error", "checksum_valid") if key in item)
                    reasons.append(role + "_observer_detail: " + str(detail))
        expected = attempt.get("expected_bytes")
        fragments = sorted(((r.get("file_offset"), r.get("fragment_length"), r) for r in streams),
                           key=lambda v: v[0] if type(v[0]) is int else -1)
        cursor = 0
        for offset, length, stream in fragments:
            if (offset != cursor or type(length) is not int or length < 0
                    or stream.get("sample_kind") != "stream_end" or stream.get("body_received_bytes") != length):
                reasons.append("receiver_stream_final_or_range_incomplete")
                break
            cursor += length
            if not stream.get("stream_valid", False):
                reasons.append("receiver_stream_observation_invalid")
            stream_episodes = [r for r in episodes[tag] if r.get("conn_uid") == stream.get("conn_uid")
                               and r.get("stream_id") == stream.get("stream_id")]
            if (stream.get("hol_episode_count") != len(stream_episodes)
                    or stream.get("hol_pending_us") != sum(r.get("duration_us", 0) for r in stream_episodes)
                    or stream.get("hol_exposed_us", 0) > stream.get("hol_pending_us", 0)
                    or any(r.get("end_us", 0) < r.get("start_us", 0)
                           or r.get("duration_us") != r.get("end_us", 0)-r.get("start_us", 0) for r in stream_episodes)):
                reasons.append("receiver_hol_episode_totals_mismatch")
            if length and not (0 < stream.get("first_body_us", 0) <= stream.get("last_body_us", 0)
                               <= stream.get("actual_mono_us", 0)):
                reasons.append("receiver_body_time_bounds_invalid")
        if type(expected) is not int or cursor != expected or summary["body_received_bytes"] != expected:
            reasons.append("receiver_body_size_mismatch")
        if not summary.get("aggregate_valid") or summary.get("aggregate_prebind_unobserved"):
            reasons.append("receiver_aggregate_coverage_incomplete")
        if final_transport.get("any_stream_hol_us") != summary["any_stream_hol_us"]:
            reasons.append("receiver_hol_union_mismatch")
        windows = summary["window_statistics"]
        if not windows or any(v.get("coverage_ratio") is None or v["coverage_ratio"] < 0.99 for v in windows):
            reasons.append("sender_window_coverage_below_99pct")
        failed_business = attempt.get("business_success") is False
        if failed_business:
            # The controller can also fail a completed transfer's hash check.
            phases = ("board_client_failed",) if summary.get("board_client_failed_at_us") is not None else ("board_client_confirmed",)
        else:
            phases = ("get_writer_drained", "get_fsync_complete", "client_confirmed", "get_requests_closed") if direction == "get" else ("post_fsync_complete", "client_confirmed")
        if any(summary.get(k+"_at_us") is None for k in phases):
            reasons.append("required_completion_phase_missing")
        if (summary.get("board_client_confirmed_at_us") is not None
                and summary.get("board_client_failed_at_us") is not None):
            reasons.append("client_terminal_phase_conflict")
        if summary.get("any_stream_hol_ratio") is None or not 0 <= summary["any_stream_hol_ratio"] <= 1:
            reasons.append("hol_time_bounds_invalid")
        summary["validation_errors"] = list(dict.fromkeys(reasons))
        # Integrity is deliberately strict: a plausible number does not repair a missing event.
        summary["hol_valid"] = summary["hol_valid"] and not any(r.startswith(("receiver_", "hol_")) for r in reasons)
        summary["window_valid"] = summary["window_valid"] and not any(r.startswith("sender_") for r in reasons)
        summary["observation_valid"] = summary["hol_valid"] and summary["window_valid"] and not reasons
        # A failed transfer may legitimately leave partial file ranges or too
        # little sampling time. Preserve those metric limitations, while using
        # capture integrity to decide whether the original retry can proceed.
        incomplete_metrics = {"receiver_stream_final_or_range_incomplete", "receiver_body_size_mismatch",
                              "sender_window_coverage_below_99pct", "hol_time_bounds_invalid"}
        integrity_errors = [r for r in summary["validation_errors"]
                            if not failed_business or r not in incomplete_metrics]
        if type(expected) is not int or expected < 0:
            integrity_errors.append("expected_file_size_invalid")
        previous_end = 0
        for offset, length, stream in fragments:
            body = stream.get("body_received_bytes")
            if (type(offset) is not int or offset < previous_end or type(length) is not int or length < 0
                    or (type(expected) is int and offset + length > expected)
                    or stream.get("sample_kind") != "stream_end" or not stream.get("stream_valid", False)
                    or type(body) is not int or not 0 <= body <= length):
                integrity_errors.append("receiver_final_statistics_or_mapping_invalid")
                break
            previous_end = offset + length
            if body and not (0 < stream.get("first_body_us", 0) <= stream.get("last_body_us", 0)
                             <= stream.get("actual_mono_us", 0)):
                integrity_errors.append("receiver_body_time_bounds_invalid")
            stream_episodes = [r for r in episodes[tag] if r.get("conn_uid") == stream.get("conn_uid")
                               and r.get("stream_id") == stream.get("stream_id")]
            if (stream.get("hol_episode_count") != len(stream_episodes)
                    or stream.get("hol_pending_us") != sum(r.get("duration_us", 0) for r in stream_episodes)
                    or stream.get("hol_exposed_us", 0) > stream.get("hol_pending_us", 0)
                    or any(r.get("end_us", 0) < r.get("start_us", 0)
                           or r.get("duration_us") != r.get("end_us", 0)-r.get("start_us", 0) for r in stream_episodes)):
                integrity_errors.append("receiver_hol_episode_totals_mismatch")
        summary["integrity_errors"] = list(dict.fromkeys(integrity_errors))
        summary["integrity_valid"] = not integrity_errors
        tables["transfer_observability_summary"].append(summary)
    for name, rows in tables.items():
        _write_csv(root / (name+".csv"), rows, ["attempt_id", "endpoint"])
    if artifacts:
        _artifacts(root, tables, manifest)
    return tables


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    export(parser.parse_args().directory)
