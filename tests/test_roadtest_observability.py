import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace

from roadtest_observability import RoadtestObserver, _union, export, observe
from roadtest_observability import _secondary_shares, _summarize_windows, _window_statistics


def package(tmp_path, records, attempts=None):
    folder = tmp_path / "raw" / "board"
    folder.mkdir(parents=True)
    (folder / "test.transport.000000.jsonl").write_text(
        "\n".join(json.dumps(r) for r in records)+"\n", encoding="utf-8")
    (tmp_path / "manifest.json").write_text(json.dumps({"attempts": attempts or {}}), encoding="utf-8")


def row(kind, stamp, seq, family="transport", **extra):
    return dict(schema_family=family, schema_version=2 if family == "transport" else 1,
                sample_kind=kind, actual_mono_us=stamp, sample_seq=seq,
                endpoint="board", conn_uid="conn", attempt_id="attempt", direction="get",
                observer_valid=True, paths=[], **extra)


def test_union():
    assert _union([(0, 100), (50, 150), (200, 210)]) == [(0, 150), (200, 210)]


def test_window_zero_and_overcommit(tmp_path):
    first = row("start", 0, 1)
    first["paths"] = [dict(path_id=0, effective_cwnd_bytes=38400,
                           inflight_bytes=34800, scheduled_bytes=3600)]
    last = row("end", 100000, 2)
    last["paths"] = [dict(path_id=0, effective_cwnd_bytes=0,
                          inflight_bytes=40000, scheduled_bytes=0)]
    package(tmp_path, [first, last])
    tables = export(tmp_path)
    paths = tables["path_windows"]
    assert paths[0]["real_utilization"] == 0.90625
    assert paths[0]["total_utilization"] == 1
    assert paths[1]["total_utilization"] is None


def test_short_multistream_episode_not_lost(tmp_path):
    records = [row("start", 0, 1), row("end", 100000, 2)]
    for sid in (0, 4):
        records += [row("stream_bind", 1, sid+1, "observer_events", stream_id=sid,
                        request_index=sid//4, file_offset=0, fragment_length=1000),
                    row("hol_episode", 20000, sid+2, "observer_events", stream_id=sid,
                        start_us=10000, end_us=20000, duration_us=10000,
                        peak_bytes=1000, censored=False),
                    row("stream_end", 30000, sid+3, "observer_events", stream_id=sid,
                        hol_pending_us=10000, hol_exposed_us=10000, reorder_byte_us=10000000,
                        reorder_pending_peak_bytes=1000, body_received_bytes=1000)]
    package(tmp_path, records, {"attempt": {"direction": "get", "business_success": True}})
    summary = export(tmp_path)["transfer_observability_summary"][0]
    assert summary["any_stream_hol_us"] == 10000
    assert summary["sum_stream_hol_pending_us"] == 20000
    assert summary["any_stream_hol_ratio"] == 0.1
    assert summary["target_stream_count"] == 2


def test_partial_and_historical_v1(tmp_path):
    old = row("start", 0, 1)
    old["schema_version"] = 1
    old["paths"] = [{"path_id": 0}]
    package(tmp_path, [old])
    with (tmp_path / "raw/board/test.transport.000000.jsonl").open("a", encoding="utf-8") as f:
        f.write('{"half":')
    tables = export(tmp_path)
    assert tables["path_windows"][0]["real_utilization"] is None
    assert any(r["status"] == "INVALID" for r in tables["observer_health"])
    assert any(r.get("end_present") is False for r in tables["observer_health"])


def test_observer_failure_never_changes_verdict():
    class Broken:
        def begin(self):
            raise OSError("disk full")
    assert observe(SimpleNamespace(_observer=Broken()), "begin") is None


def test_both_entrypoints_keep_original_test_plan():
    root = Path(__file__).resolve().parents[1]
    contents = [(root / name).read_text(encoding="utf-8") for name in (
        "test_wht/副本改进版.py", "tests/副本改进版.py")]
    assert contents[0] == contents[1]
    spec = importlib.util.spec_from_file_location("roadtest_entry", root / "tests/副本改进版.py")
    module = importlib.util.module_from_spec(spec)
    import sys
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    args = module.parse_gx_args([])
    plan = module.build_test_plan(args)
    assert len(plan) == 12
    assert {c.mode for c in plan} == {"rmnet", "usb", "agg"}
    assert {c.direction for c in plan} == {"get", "post"}
    assert all(c.client_scheduler == "adaptive" for c in plan if c.direction == "get")
    assert all(c.server_scheduler == "adaptive" for c in plan if c.direction == "post")


def test_disabled_collection_has_no_ssh_calls(tmp_path):
    args = SimpleNamespace(disable_observer=True, observer_interval_ms=100,
                           board_resource_dir="/tmp/board", server_resource_dir="/tmp/server")
    session = RoadtestObserver(args, "session", tmp_path, None, None)
    session.start()
    assert not any(session.supported.values())
    assert session.client_arguments("attempt") == []
    assert session.server_arguments() == ""
    session.finish()


def test_time_weighting_excludes_long_gaps():
    rows = [dict(attempt_id="a", endpoint="server", conn_uid="c", path_id=0,
                 mapping_generation=0, actual_mono_us=t, interval_us=100000,
                 real_utilization=value, scheduled_utilization=0, total_utilization=value)
            for t, value in [(0, .5), (100000, 1.5), (200000, 1), (2000000, 0)]]
    _window_statistics({"path_windows": rows}, {})
    summary = _summarize_windows(rows, "a", "server")[0]
    assert summary["time_weighted_real_utilization"] == 1
    assert summary["covered_us"] == 200000
    assert summary["coverage_ratio"] == .1
    assert rows[2]["unknown_interval_us"] == 1800000


def test_role_epoch_shares_keep_packet_basis():
    rows = [dict(attempt_id="a", endpoint="server", conn_uid="c", role_delta_valid=True,
                 delta_role_epoch=epoch, delta_role=role, delta_stream_packet_sent_bytes=size)
            for epoch, role, size in [(1, "primary", 900), (1, "secondary", 100),
                                      (2, "primary", 50), (2, "secondary", 50)]]
    result = _secondary_shares(rows, "a", "server")
    assert [r["actual_secondary_stream_packet_share_pct"] for r in result] == [10, 50]


def test_malformed_field_does_not_abort_export(tmp_path):
    broken = row("start", 0, 1)
    broken["actual_mono_us"] = "invalid"
    package(tmp_path, [broken, row("end", 100000, 2)])
    tables = export(tmp_path)
    assert any(r["status"] == "INVALID" for r in tables["observer_health"])


def test_same_connection_sequential_attempts_prebind_association(tmp_path):
    records = [row("start", 0, 1), row("end", 100, 2),
               row("stream_bind", 10, 1, "observer_events", stream_id=0),
               row("stream_bind", 110, 2, "observer_events", stream_id=4)]
    records[3]["attempt_id"] = "second"
    records += [row("start", 110, 3), row("end", 200, 4)]
    records[4]["attempt_id"] = records[5]["attempt_id"] = "second"
    package(tmp_path, records)
    health = export(tmp_path)["observer_health"]
    assert {r.get("attempt_id") for r in health if r.get("end_present")} == {"attempt", "second"}


def test_csv_excel_and_four_panel_plot(tmp_path):
    import pytest
    pytest.importorskip("matplotlib")
    pytest.importorskip("openpyxl")
    records = [row("start", 0, 1), row("end", 100000, 2),
               row("stream_bind", 1, 1, "observer_events", stream_id=0,
                   request_index=0, file_offset=0, fragment_length=1000),
               row("hol_episode", 20000, 2, "observer_events", stream_id=0,
                   start_us=10000, end_us=20000, duration_us=10000, peak_bytes=1000, censored=False),
               row("stream_end", 30000, 3, "observer_events", stream_id=0,
                   hol_pending_us=10000, hol_exposed_us=10000, reorder_byte_us=10000000,
                   reorder_pending_bytes=0, reorder_pending_peak_bytes=1000,
                   body_received_bytes=1000, first_body_us=1000, last_body_us=30000)]
    package(tmp_path, records, {"attempt": {"direction": "get", "business_success": True}})
    server = tmp_path / "raw/server"
    server.mkdir()
    sends = []
    for i, kind in enumerate(("start", "periodic", "end")):
        sample = row(kind, i*50000, i+1)
        sample.update(scheduler_name="adaptive", primary_valid=True, primary_path_id=0,
                      secondary_path_id=1, role_epoch=1, target_secondary_pct=10)
        sample["paths"] = [dict(path_id=p, mapping_generation=0, cwnd_bytes=38400,
            effective_cwnd_bytes=38400, inflight_bytes=34800, scheduled_bytes=3600,
            stream_packet_sent_bytes=i*(900 if p == 0 else 100)) for p in (0, 1)]
        sends.append(sample)
    (server / "test.transport.000000.jsonl").write_text(
        "\n".join(json.dumps(r) for r in sends)+"\n", encoding="utf-8")
    export(tmp_path, artifacts=True)
    assert (tmp_path / "observability.xlsx").stat().st_size > 1000
    assert (tmp_path / "plots/attempt.png").stat().st_size > 1000
    assert json.loads((tmp_path / "artifact_status.json").read_text(encoding="utf-8"))["errors"] == []


def test_download_hash_failure_retains_source_and_can_retry(tmp_path):
    import hashlib
    payload = json.dumps(row("start", 0, 1)).encode()+b"\n"
    class Remote:
        bad_hash = True
        calls = []
        def run(self, command, **kwargs):
            self.calls.append(command)
            output = ("/tmp/board/observer_s/a.transport.000000.jsonl\n" if command.startswith("find ")
                      else ("0"*64 if self.bad_hash else hashlib.sha256(payload).hexdigest())+"  source\n")
            return SimpleNamespace(output=output, exit_code=0, timed_out=False, truncated=False)
        def download(self, source, destination):
            destination.write_bytes(payload)
    remote = Remote()
    args = SimpleNamespace(disable_observer=False, observer_interval_ms=100,
                           board_resource_dir="/tmp/board", server_resource_dir="/tmp/server")
    observer = RoadtestObserver(args, "s", tmp_path, remote, None)
    observer.supported["board"] = True
    observer.collect()
    assert not observer.manifest["files"]
    assert observer.manifest["errors"]
    remote.bad_hash = False
    observer.collect()
    assert len(observer.manifest["files"]) == 1
    assert not any("rm " in command for command in remote.calls)
