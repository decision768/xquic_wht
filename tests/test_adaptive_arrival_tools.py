import argparse
import ast
import csv
from dataclasses import dataclass
import importlib.util
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
from typing import List, Sequence
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("arrival_benchmark", ROOT / "test_wht/adaptive_arrival_benchmark.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class ArrivalTools(unittest.TestCase):
    def test_mode_and_labels(self):
        args = argparse.Namespace(adaptive_arrival_mode="shadow", adaptive_arrival_option=[],
            direction="get", schedulers=["adaptive", "adaptive-v2"], order=["agg"],
            sequence="fixed", warmup_cycles=0, iterations=1, cc="bbr")
        source = ast.parse(next((ROOT / "test_wht").glob("*100MB_FCT.py")).read_text(encoding="utf-8"))
        wanted = {"TestCase", "build_test_plan", "balanced_algorithm_order"}
        selected = ast.Module(body=[n for n in source.body if isinstance(n, (ast.ClassDef, ast.FunctionDef))
                                   and n.name in wanted], type_ignores=[])
        namespace = dict(dataclass=dataclass, List=List, Sequence=Sequence, arrival_benchmark=bench,
            transfer_algorithms_for_direction=lambda a, d: ("adaptive", "bbr"), DEFAULT_CC="bbr",
            BASELINE_POSITIONS=("before", "after"), SINGLE_LINK_PROFILE="single_link", SINGLE_LINK_SCHEDULER="minrtt")
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

    def test_FCT_hol_and_clock_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            family = dict(family="adaptive_arrival_events", schema_version=1, conn_uid=1)
            rows = [dict(family, event="config", mode="enforce", source_build_id="unit-test-fixture"),
                dict(family, event="wait_begin", episode_id=1, transfer_id="a", first_defer_us=90, deadline_us=5000),
                dict(family, event="wait_end", episode_id=1, transfer_id="a", first_defer_us=90, actual_mono_us=110),
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
            self.assertIn("no_calibration", read("boundary_uncertainty.csv")[0]["reason"])
            (root / "calibration.json").write_text(json.dumps({"a":dict(server_to_client_offset_us=0, error_us=5)}), encoding="utf-8")
            subprocess.run(command + ["--calibration", str(root / "calibration.json")], check=True, capture_output=True, text=True)
            boundary = read("boundary_uncertainty.csv")[0]
            self.assertEqual((boundary["overlap_min_us"], boundary["overlap_max_us"], boundary["reason"]),
                             ("5", "15", "boundary_uncertain"))


if __name__ == "__main__":
    unittest.main()
