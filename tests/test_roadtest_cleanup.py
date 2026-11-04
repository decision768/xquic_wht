"""Hardware-free checks for the active road-test entry point and its cleanup."""

import importlib.util
import json
import shlex
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

import pytest


@pytest.fixture
def benchmark(monkeypatch):
    directory = Path(__file__).resolve().parents[1] / "test_wht"
    monkeypatch.syspath_prepend(str(directory))
    for name, path in (
        ("roadtest_observability", directory / "roadtest_observability.py"),
        ("active_roadtest", directory / "压测（路测+裘博指标）.py"),
    ):
        spec = importlib.util.spec_from_file_location(name, path)
        module = importlib.util.module_from_spec(spec)
        monkeypatch.setitem(sys.modules, name, module)
        spec.loader.exec_module(module)
    return module


@pytest.mark.parametrize("case, excluded", [
    ("initializer", True), ("no_successor", False), ("long_handoff_gap", False),
    ("changed_peer", False), ("changed_local", False), ("already_mapped", False),
    ("bad_checksum", False), ("bad_observer", False), ("positive_duration_gap", False),
    ("single_successor", False), ("different_path", False), ("different_connection", False),
    ("prior_mapping", False),
    ("unknown_address", False), ("missing_generation", False),
])
def test_window_coverage_only_exempts_proven_initial_mapping(benchmark, case, excluded):
    module = sys.modules["roadtest_observability"]

    def row(timestamp, generation, interface):
        return dict(attempt_id="a", endpoint="board", conn_uid="c", path_id=1,
                    mapping_generation=generation, actual_mono_us=timestamp, interval_us=100000,
                    interface_name=interface, local_address="[10.0.0.1]:123", peer_address="[10.0.0.2]:443",
                    path_state=2, observer_valid=True, checksum_valid=True,
                    real_utilization=0.5, scheduled_utilization=0, total_utilization=0.5)

    initial = row(186644, 1, "")
    following = [row(196800, 2, "usb0"), row(296800, 2, "usb0")]
    if case == "no_successor":
        following = []
    elif case == "long_handoff_gap":
        for item in following:
            item["actual_mono_us"] += 1000000
    elif case in ("changed_peer", "changed_local"):
        initial["peer_address" if case == "changed_peer" else "local_address"] = "[10.0.0.3]:456"
    elif case == "already_mapped":
        initial["interface_name"] = "usb0"
    elif case == "unknown_address":
        initial["local_address"] = ""
    elif case == "missing_generation":
        initial["mapping_generation"] = None
    elif case in ("bad_checksum", "bad_observer"):
        initial["checksum_valid" if case == "bad_checksum" else "observer_valid"] = False
    elif case == "single_successor":
        following = following[:1]
    elif case in ("different_path", "different_connection"):
        for item in following:
            item["path_id" if case == "different_path" else "conn_uid"] = 9 if case == "different_path" else "other"
    rows = [initial, *following]
    if case == "prior_mapping":
        rows = [row(0, 0, "usb0"), row(100000, 0, "usb0"), *rows]
    if case == "positive_duration_gap":
        rows = [initial, row(1186644, 1, ""), row(1196800, 2, "usb0"), row(1296800, 2, "usb0")]
    module._window_statistics({"path_windows": rows}, {})
    windows = module._summarize_windows(rows, "a", "board")
    first = next(w for w in windows if w["mapping_generation"] == initial["mapping_generation"])
    assert first["coverage_applicable"] is (not excluded)
    if excluded:
        assert first["coverage_ratio"] is None
        assert first["coverage_reason"] == "initial_interface_mapping_singleton"
    eligible = [w for w in windows if w["coverage_applicable"]]
    assert any(w["coverage_ratio"] is None or w["coverage_ratio"] < 0.99 for w in eligible) is (not excluded)


def test_captured_fifth_round_observation_passes_without_losing_evidence(benchmark, tmp_path):
    source = Path(__file__).resolve().parents[1] / "LOG存档/1104_183828_adaptive_minrtt/observability"
    if not source.is_dir():
        pytest.skip("Optional captured road-test archive is not installed")
    manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
    tag = next(tag for tag, attempt in manifest["attempts"].items()
               if attempt["case_id"] == "measured_adaptive_r0005_post_agg")
    prefixes = set()
    for meta in manifest["files"].values():
        path = Path(meta["local_path"])
        if ".checkpoint." in path.name:
            receipt = json.loads((source / path).read_text(encoding="utf-8"))
            if receipt.get("attempt_id") == tag:
                prefixes.add((path.parent.as_posix(), path.name.split(".")[0]))
    assert len(prefixes) == 2
    manifest["attempts"] = {tag: manifest["attempts"][tag]}
    manifest["files"] = {k: v for k, v in manifest["files"].items()
                         if (Path(v["local_path"]).parent.as_posix(), Path(v["local_path"]).name.split(".")[0]) in prefixes}
    for meta in manifest["files"].values():
        local = Path(meta["local_path"])
        (tmp_path / local).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / local, tmp_path / local)
    (tmp_path / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
    module = sys.modules["roadtest_observability"]
    summary = module.export(tmp_path)["transfer_observability_summary"][0]
    assert summary["observation_valid"] and summary["integrity_valid"]
    assert summary["validation_errors"] == []
    windows = summary["window_statistics"]
    assert len(windows) == 3
    assert len([w for w in windows if not w["coverage_applicable"]]) == 1
    assert all(w["coverage_ratio"] == 1 for w in windows if w["coverage_applicable"])
    # A below-threshold ratio must still invalidate the same captured transfer.
    # Inject it at the summary boundary without changing the archived evidence.
    original = module._summarize_windows

    def missing_windows(rows, attempt, endpoint):
        records = original(rows, attempt, endpoint)
        for window in records:
            if window["path_id"] == 1 and window["coverage_applicable"]:
                window["coverage_ratio"] = 0.98
        return records

    module._summarize_windows = missing_windows
    invalid = module.export(tmp_path)["transfer_observability_summary"][0]
    assert not invalid["window_valid"]
    assert "sender_window_coverage_below_99pct" in invalid["validation_errors"]


@pytest.fixture
def observer_for_save(benchmark, tmp_path):
    cls = sys.modules["roadtest_observability"].RoadtestObserver
    observer = cls.__new__(cls)
    observer.root = tmp_path
    observer.manifest = {"cleanup": {"complete": True}, "note": "回收完成"}
    (tmp_path / "manifest.json").write_text('{"previous": true}', encoding="utf-8")
    return observer


@pytest.mark.parametrize("winerror", [5, 32, 33])
def test_manifest_save_recovers_from_windows_file_lock(observer_for_save, monkeypatch, winerror):
    observer = observer_for_save
    target = observer.root / "manifest.json"
    pending = target.with_suffix(".tmp")
    original_replace = Path.replace
    failures = 0

    def replace(path, destination):
        nonlocal failures
        assert json.loads(pending.read_text(encoding="utf-8")) == observer.manifest
        if failures < 2:
            assert json.loads(target.read_text(encoding="utf-8")) == {"previous": True}
            failures += 1
            error = PermissionError("file in use")
            error.winerror = winerror
            raise error
        return original_replace(path, destination)

    sleep = Mock()
    monkeypatch.setattr(Path, "replace", replace)
    monkeypatch.setattr(sys.modules["roadtest_observability"].time, "sleep", sleep)
    observer.save()
    assert json.loads(target.read_text(encoding="utf-8")) == observer.manifest
    assert not pending.exists()
    assert [call.args[0] for call in sleep.call_args_list] == [0.1, 0.2]


@pytest.mark.skipif(sys.platform != "win32", reason="Requires Windows file sharing semantics")
def test_manifest_save_recovers_from_actual_windows_reader(observer_for_save, monkeypatch):
    observer = observer_for_save
    target = observer.root / "manifest.json"
    with target.open("rb") as reader:
        sleep = Mock(side_effect=lambda delay: reader.close())
        monkeypatch.setattr(sys.modules["roadtest_observability"].time, "sleep", sleep)
        observer.save()
    sleep.assert_called_once_with(0.1)
    assert json.loads(target.read_text(encoding="utf-8")) == observer.manifest
    assert not target.with_suffix(".tmp").exists()


def test_manifest_save_reports_persistent_windows_file_lock(observer_for_save, monkeypatch):
    observer = observer_for_save
    error = PermissionError("still locked")
    error.winerror = 5
    replace = Mock(side_effect=error)
    sleep = Mock()
    monkeypatch.setattr(Path, "replace", replace)
    monkeypatch.setattr(sys.modules["roadtest_observability"].time, "sleep", sleep)
    with pytest.raises(PermissionError) as raised:
        observer.save()
    assert raised.value is error
    assert replace.call_count == 6
    assert [call.args[0] for call in sleep.call_args_list] == [0.1, 0.2, 0.4, 0.8, 1.6]
    assert json.loads((observer.root / "manifest.json").read_text(encoding="utf-8")) == {"previous": True}
    assert json.loads((observer.root / "manifest.tmp").read_text(encoding="utf-8")) == observer.manifest


@pytest.mark.parametrize("error", [PermissionError("not a Windows lock"), OSError("disk failure")])
def test_manifest_save_does_not_retry_other_errors(observer_for_save, monkeypatch, error):
    replace = Mock(side_effect=error)
    sleep = Mock()
    monkeypatch.setattr(Path, "replace", replace)
    monkeypatch.setattr(sys.modules["roadtest_observability"].time, "sleep", sleep)
    with pytest.raises(type(error)) as raised:
        observer_for_save.save()
    assert raised.value is error
    replace.assert_called_once()
    sleep.assert_not_called()


@pytest.mark.parametrize("failure", [None, "scan", "delete", "leftover"])
def test_remote_cleanup_on_restricted_find(benchmark, tmp_path, failure):
    # Execute actual shell commands inside an isolated directory, rejecting the
    # options missing on the board. Never scan or remove the host's /tmp files.
    bash = shutil.which("bash")
    if not bash and sys.platform == "win32" and shutil.which("git"):
        candidate = Path(shutil.which("git")).parents[1] / "bin" / "bash.exe"
        bash = str(candidate) if candidate.is_file() else None
    if not bash:
        pytest.skip("A POSIX shell is required for remote command integration")
    controls = tmp_path / "controls"
    board_dir = tmp_path / "board"
    server_dir = tmp_path / "server"
    state = server_dir / "state"
    for directory in (controls, board_dir, server_dir / "upload", state):
        directory.mkdir(parents=True, exist_ok=True)
    session = "cleanup_test"
    generated = [controls / f"api_{session}_case_resource.stop"]
    generated += [board_dir / f"get-{mode}-{session}-case.bin" for mode in ("rmnet", "usb", "agg")]
    generated += [server_dir / "upload" / f"post-{mode}.bin" for mode in ("rmnet", "usb", "agg")]
    preserved = [board_dir / "source.bin", controls / "api_other_case_resource.stop",
                 board_dir / "get-rmnet-other-case.bin", server_dir / "upload" / "1"]
    for path in generated + preserved:
        path.write_bytes(b"payload")

    def run(command, **unused):
        assert "-mtime" not in command and "-quit" not in command
        command = command.replace("find /tmp ", "find " + shlex.quote(controls.as_posix()) + " ")
        guard = """
find() {
    for arg in "$@"; do
        case "$arg" in -mtime|-quit) return 99;; esac
    done
    case " $* " in
        *" -print "*) [ "$failure" != scan ] || return 3;;
        *" -delete "*)
            [ "$failure" != delete ] || return 4
            [ "$failure" != leftover ] || return 0;;
    esac
    command find "$@"
}
"""
        result = subprocess.run(
            [bash, "--noprofile", "--norc", "-c",
             "failure=" + shlex.quote(failure or "") + "\n" + guard + command],
            capture_output=True, text=True, timeout=15,
        )
        return benchmark.CommandResult(output=result.stdout, stderr=result.stderr,
                                       exit_code=result.returncode, controller_elapsed_s=0)

    remote = Mock(run=run)
    remote.is_connected.return_value = True
    args = SimpleNamespace(board_dir=board_dir.as_posix(), server_dir=server_dir.as_posix(),
                           **{"post_remote_" + mode: f"/upload/post-{mode}.bin"
                              for mode in ("rmnet", "usb", "agg")})
    assert benchmark.cleanup_current_session_remote_artifacts(
        remote, remote, args, session, state.as_posix(), True) is (failure is None)
    if failure is None:
        assert not any(path.exists() for path in generated)
        assert not state.exists()
    assert all(path.read_bytes() == b"payload" for path in preserved)


@pytest.mark.parametrize("outcome, expected", [
    ("success", 0), ("failed_transfer", 1), ("exception", 2),
    ("interrupt", 130), ("cleanup_failure", 2), ("export_failure", 2),
    ("observation_invalid", 2), ("long_run", 0),
])
def test_final_cleanup_for_all_exit_paths(benchmark, monkeypatch, tmp_path, outcome, expected):
    m = benchmark
    remote = Mock()
    remote.is_connected.return_value = True
    remote.run.return_value = m.CommandResult(output="", exit_code=0, controller_elapsed_s=0)
    observer = Mock()
    observer.manifest = {"cleanup": {"complete": True}}
    observer.end.return_value = {"status": "OK", "errors": []}
    if outcome == "observation_invalid":
        observer.end.return_value = {"status": "INVALID", "errors": ["receiver_checkpoint_missing"]}
    observer.needs_server_state.return_value = False
    observer.cleanup_temporary.return_value = True
    observer._cleanup_idle.return_value = True
    if outcome == "export_failure":
        observer.finish.side_effect = OSError("export unavailable")

    def create_observer(args, session, root, *remotes):
        (root / "observability").mkdir()
        return observer

    replacements = {
        "SSHRemote": Mock(return_value=remote), "create_observer": create_observer,
        "server_preflight": Mock(return_value=("/upload/1", 100)),
        "start_server_quiet": Mock(return_value=12345),
        "start_server_resource_monitor": Mock(return_value={}),
        "build_client_command": Mock(return_value="mock-client"),
        "parse_client_result": Mock(return_value=(outcome != "failed_transfer", {
            "total_bytes": 100, "transfer_elapsed_s": 1, "throughput_mbps": 0.0008}, "")),
        "cleanup_board_attempt": Mock(return_value=True),
        "cleanup_current_session_remote_artifacts": Mock(return_value=outcome != "cleanup_failure"),
        "collect_board_resource_gx": Mock(return_value={}),
        "collect_server_resource": Mock(return_value={}), "stop_server": Mock(return_value=True),
        "get_board_post_source_info": Mock(return_value=(100, "")),
        "prepare_server_post_target": Mock(return_value=True),
        "inspect_and_cleanup_server_post": Mock(return_value=("", True)),
    }
    for name in ("configure_console_logging", "connect_ssh_with_optional_prompt", "recover_pending_cleanup",
                 "validate_server_scheduler_support", "board_preflight", "validate_server_resource_monitor",
                 "validate_server_post_targets", "stop_server_resource_monitor", "write_speed_log", "write_experiment_results"):
        replacements[name] = Mock()
    for name, value in replacements.items():
        monkeypatch.setattr(m, name, value)
    if outcome in ("exception", "interrupt"):
        replacements["start_server_resource_monitor"].side_effect = (
            RuntimeError("monitor failed") if outcome == "exception" else KeyboardInterrupt())
    rc = m.gx_main(["--output-root", str(tmp_path), "--schedulers", "adaptive,minrtt" if outcome == "long_run" else "adaptive",
                    "--direction", "both" if outcome == "long_run" else "get", "--iterations", "20" if outcome == "long_run" else "1",
                    "--retries", "0", "--cooldown-s", "0", "--algorithm-cooldown-s", "0"])
    assert rc == expected
    observer.finish.assert_called_once()
    observer.cleanup_temporary.assert_called_once_with(final=True)
    replacements["cleanup_current_session_remote_artifacts"].assert_called_once()
    assert replacements["stop_server"].call_count == replacements["start_server_quiet"].call_count
    if outcome == "long_run":
        assert observer.end.call_count == 240
        assert replacements["cleanup_board_attempt"].call_count == 240
    elif outcome == "observation_invalid":
        observer.end.assert_called_once()
    assert remote.close.call_count == 2
    status = json.loads(next(tmp_path.glob("*/cleanup_status.json")).read_text(encoding="utf-8"))
    assert status["complete"] is (outcome not in ("cleanup_failure", "export_failure"))
    assert bool(status["errors"]) is (not status["complete"])
