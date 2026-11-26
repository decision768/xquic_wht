import argparse
import ast
import csv
from dataclasses import dataclass
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from typing import List, Optional, Sequence
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("arrival_benchmark", ROOT / "test_wht/adaptive_arrival_benchmark.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)
observer_spec = importlib.util.spec_from_file_location(
    "arrival_p0_observer", ROOT / "test_wht/new_roadtest_observability_fct.py")
observer = importlib.util.module_from_spec(observer_spec)
observer_spec.loader.exec_module(observer)


class ArrivalTools(unittest.TestCase):
    def test_mode_and_labels(self):
        args = argparse.Namespace(adaptive_arrival_mode="shadow", adaptive_arrival_option=[],
            direction="get", schedulers=["adaptive", "adaptive-v2"], order=["agg"],
            sequence="fixed", warmup_cycles=0, iterations=1, cc="bbr")
        source = ast.parse(next((ROOT / "test_wht").glob("*100MB_FCT.py")).read_text(encoding="utf-8"))
        wanted = {"TestCase", "build_test_plan", "balanced_algorithm_order"}
        selected = ast.Module(body=[n for n in source.body if isinstance(n, (ast.ClassDef, ast.FunctionDef))
                                   and n.name in wanted], type_ignores=[])
        namespace = dict(dataclass=dataclass, List=List, Optional=Optional, Sequence=Sequence, arrival_benchmark=bench,
            transfer_algorithms_for_direction=lambda a, d: ("adaptive", "bbr"), DEFAULT_CC="bbr",
            BASELINE_POSITIONS=("before", "after"), SINGLE_LINK_PROFILE="single_link", SINGLE_LINK_SCHEDULER="original_minrtt")
        exec(compile(selected, "benchmark-plan", "exec"), namespace)
        plan = namespace["build_test_plan"](args)
        treatment = next(c for c in plan if c.scheduler == "adaptive-v2")
        self.assertEqual((treatment.adaptive_arrival_mode, treatment.scheduler_label),
                         ("shadow", "adaptive-v2(shadow)"))
        argv = shlex.split(bench.server_arguments(args, "adaptive-v2", "/tmp/state"))
        self.assertIn("--adaptive-arrival-mode", argv)
        self.assertEqual(argv[argv.index("--adaptive-arrival-mode")+1], "shadow")
        self.assertEqual(bench.server_arguments(args, "adaptive", "/tmp/state"), "")
        args.direction = "post"
        post = namespace["build_test_plan"](args)
        self.assertTrue(all(c.adaptive_arrival_mode == "off" for c in post))
        args.adaptive_arrival_option = ["rate_sample_capacity=17"]
        with self.assertRaises(ValueError):
            namespace["build_test_plan"](args)

    def test_archive_rejects_checksum_mismatch(self):
        class Remote:
            removed = False
            def run(self, command, **kwargs):
                if "find " in command:
                    output = "/tmp/state/adaptive_arrival/events.transport.000000.jsonl\n"
                elif "sha256sum" in command:
                    output = "0"*64 + " file\n"
                else:
                    self.removed = True
                    output = ""
                return argparse.Namespace(exit_code=0, timed_out=False, truncated=False, output=output)
            def download(self, remote, local):
                local.write_bytes(b"retained evidence")
        remote = Remote()
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(OSError):
                bench.collect(remote, "/tmp/state", directory)
            self.assertFalse(remote.removed)

    def test_p0_role_switch_with_new_and_legacy_labels(self):
        def sample(kind, time, primary, counts, **fields):
            event = kind in ("role_change", "control_update")
            return dict(sample_kind=kind, actual_mono_us=time, sample_seq=time, attempt_id="get-1",
                        conn_uid="c1", schema_family="observer_events" if event else "transport",
                        schema_version=1 if event else 2, primary_valid=True,
                        primary_path_id=primary, secondary_path_id=3 - primary,
                        role_epoch=1 if primary == 1 else 2,
                        paths=[dict(path_id=index, mapping_generation=1, stream_packet_sent_bytes=count)
                               for index, count in enumerate(counts, start=1)], **fields)

        def summarize(root, endpoints):
            files, paths = {}, []
            for endpoint, rows in endpoints.items():
                path = root / (endpoint + ".jsonl")
                path.write_text("\n".join(map(json.dumps, rows)), encoding="utf-8")
                files[endpoint] = dict(endpoint=endpoint, local_path=path.name,
                                       sha256=hashlib.sha256(path.read_bytes()).hexdigest())
                paths.append(path)
            return observer._summarize(root, {"files": files}, paths)["path_windows"]

        for scheduler, legacy in (("adaptive-v2", False), ("adaptive-v2", True), ("adaptive", True)):
            with self.subTest(scheduler=scheduler, legacy=legacy), tempfile.TemporaryDirectory() as directory:
                name = dict(scheduler_name=scheduler)
                event_name = {} if legacy else name
                rows = summarize(Path(directory), {"server": [
                    sample("start", 100, 1, (10, 20), **name),
                    sample("role_change", 200, 2, (40, 30), **event_name),
                    sample("control_update", 250, 2, (48, 45), **event_name),
                    sample("end", 300, 2, (50, 60), **name),
                ]})
                self.assertEqual({row["scheduler_name"] for row in rows}, {scheduler})
                shares = observer._secondary_shares(rows, "get-1", "server")
                self.assertEqual([(r["role_epoch"], r["primary_stream_packet_bytes"],
                                   r["secondary_stream_packet_bytes"]) for r in shares],
                                 [(1, 30, 10), (2, 30, 10)])

        with tempfile.TemporaryDirectory() as directory:
            rows = summarize(Path(directory), {
                "server": [
                    sample("start", 100, 1, (10, 20), scheduler_name="adaptive-v2"),
                    sample("role_change", 200, 2, (40, 30)),
                    sample("end", 300, 2, (50, 60), scheduler_name="adaptive"),
                ],
                "board": [sample("start", 100, 1, (10, 20), scheduler_name="adaptive")],
            })
            event_rows = [row for row in rows if row["sample_kind"] == "role_change"]
            self.assertEqual(len(event_rows), 2)
            self.assertTrue(all(not row.get("scheduler_name") and row["delta_valid"]
                                and not row["role_delta_valid"] for row in event_rows))
            self.assertEqual(observer._secondary_shares(rows, "get-1", "server"), [])

    def test_FCT_hol_and_clock_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            family = dict(family="adaptive_arrival_events", schema_version=1, conn_uid=1)
            rows = [dict(family, event="config", mode="enforce", source_build_id="unit-test-fixture"),
                dict(family, event="wait_begin", episode_id=1, transfer_id="a", first_defer_us=90, deadline_us=5000),
                dict(family, event="wait_end", episode_id=1, transfer_id="a", first_defer_us=90, actual_mono_us=110),
                dict(family, event="packet_sent", send_time_us=100, W_error_us=2),
                dict(family, event="packet_sent", send_time_us=110, socket_return_us=113,
                     send_time_basis="socket_call_start", W_error_us=8),
                dict(family, event="packet_sent", send_time_us=115, socket_return_us=119,
                     send_time_basis="batch_socket_call_start", W_error_us=10),
                dict(family, event="packet_sent", send_time_us=115, socket_return_us=119,
                     send_time_basis="batch_socket_call_start", W_error_us=-2),
                dict(family, event="summary", lifecycle_dropped=0, mechanism_complete=True)]
            (root / "server.jsonl").write_text("\n".join(map(json.dumps, rows)), encoding="utf-8")
            with (root / "fct.csv").open("w", newline="", encoding="utf-8") as handle:
                writer = csv.DictWriter(handle, fieldnames=["attempt_id", "first_get_submit_us", "last_body_us", "get_fct_status"])
                writer.writeheader()
                writer.writerow(dict(attempt_id="a", first_get_submit_us=100, last_body_us=200, get_fct_status="ok"))
            receiver = [dict(endpoint="board", transfer_id="a", sample_kind="hol_episode", start_us=90, end_us=130),
                dict(endpoint="board", transfer_id="a", sample_kind="hol_episode", start_us=120, end_us=150),
                dict(endpoint="board", transfer_id="a", sample_kind="end", actual_mono_us=210, observer_valid=True)]
            (root / "board.jsonl").write_text("\n".join(map(json.dumps, receiver)), encoding="utf-8")
            command = [sys.executable, str(ROOT / "tools/analyze_adaptive_arrival.py"), str(root / "server.jsonl"),
                "--output", str(root / "out"), "--fct-csv", str(root / "fct.csv"),
                "--receiver-events", str(root / "board.jsonl")]
            subprocess.run(command, check=True, capture_output=True, text=True)
            def read(name):
                with (root / "out" / name).open(encoding="utf-8-sig", newline="") as handle:
                    return list(csv.DictReader(handle))
            self.assertEqual(read("receiver_hol_fct.csv")[0]["hol_union_us"], "50")
            self.assertEqual(read("file_fct.csv"), [dict(attempt_id="a", first_get_submit_us="100",
                                                        last_body_us="200", get_fct_status="ok")])
            report = json.loads((root / "out" / "analysis.json").read_text(encoding="utf-8"))
            self.assertEqual(report["chosen_path_W_error_samples"], 4)
            self.assertIsNone(report["chosen_path_W_error_mean_us"])
            self.assertEqual(report["chosen_path_W_error_by_basis"], {
                "legacy_pre_socket": dict(samples=1, mean_us=2.0),
                "socket_call_start": dict(samples=1, mean_us=8.0),
                "batch_socket_call_start": dict(samples=2, mean_us=4.0),
            })
            self.assertEqual(read("packet_sends.csv")[0]["send_time_basis"], "legacy_pre_socket")
            self.assertIn("no_calibration", read("boundary_uncertainty.csv")[0]["reason"])
            (root / "calibration.json").write_text(json.dumps({"a":dict(server_to_client_offset_us=0, error_us=5)}), encoding="utf-8")
            subprocess.run(command + ["--calibration", str(root / "calibration.json")], check=True, capture_output=True, text=True)
            boundary = read("boundary_uncertainty.csv")[0]
            self.assertEqual((boundary["overlap_min_us"], boundary["overlap_max_us"], boundary["reason"]),
                             ("5", "15", "boundary_uncertain"))


if __name__ == "__main__":
    unittest.main()
