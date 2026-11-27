"""Check the 1GiB variant against the existing 500MiB experiment offline."""
from contextlib import redirect_stdout
import csv
from dataclasses import asdict
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


fixture = load("one_gib_shared_test_fixture", ROOT / "tests/test_1gib_get_benchmark.py")
baseline = fixture.entry
entry = load("one_gib_variant_tests", ROOT / "test_wht/压测_1GiB_GET_FCT.py")
core = entry.engine


class OneGiBVariant(unittest.TestCase):
    def test_only_aggregate_workload_and_output_identity_change(self):
        old = vars(baseline.parse_args(["--iterations", "1"]))
        new_args = entry.parse_args(["--iterations", "1"])
        new = vars(new_args)
        self.assertEqual(old.keys(), new.keys())
        self.assertEqual({key for key in old if old[key] != new[key]},
                         {"remote_path", "output_root", "benchmark_profile", "get_file_bytes_by_mode"})
        self.assertEqual(new_args.remote_path, "/upload/1-1GiB")
        self.assertEqual(Path(new_args.output_root).name, "log_1GB_GET")
        old_plan = baseline.engine.build_test_plan(baseline.parse_args(["--iterations", "1"]))
        new_plan = core.build_test_plan(new_args)
        self.assertEqual(len(new_plan), 17)
        self.assertEqual(len(old_plan), len(new_plan))
        for old_case, new_case in zip(old_plan, new_plan):
            expected = asdict(old_case)
            if old_case.mode == "agg":
                expected.update(remote_path="/upload/1-1GiB", file_bytes=1073741824)
            self.assertEqual(asdict(new_case), expected)
        self.assertEqual(baseline.parse_args([]).get_file_bytes_by_mode["agg"], 524288000)

    def test_source_requires_exactly_one_gib_without_creating_files(self):
        for size in (1073741824, 524288000, 104857600, 1073741825):
            with self.subTest(size=size):
                commands = []

                def run(command, **kwargs):
                    commands.append(command)
                    return core.CommandResult(f"__MINI_SOURCE_SIZE__={size}\n", 0, 0)

                remote = SimpleNamespace(run=run)
                if size == 1073741824:
                    self.assertEqual(entry.server_preflight(remote, "/root/test_xquic_server",
                        "./netins_udp_mini_api_server", "/upload/1-1GiB"),
                        ("/root/test_xquic_server/upload/1-1GiB", 1073741824))
                else:
                    with self.assertRaises(RuntimeError):
                        entry.server_preflight(remote, "/root/test_xquic_server",
                            "./netins_udp_mini_api_server", "/upload/1-1GiB")
                for command in commands:
                    self.assertNotIn("head -c", command)
                    self.assertNotIn("dd ", command)
                    self.assertNotIn("mktemp", command)

    def test_dry_run_records_one_gib_aggregates_and_100mib_baselines(self):
        with tempfile.TemporaryDirectory() as folder, redirect_stdout(io.StringIO()), \
                patch.object(core.SSHRemote, "connect", side_effect=AssertionError("unexpected SSH")):
            self.assertEqual(entry.main(["--iterations", "1", "--dry-run", "--output-root", folder]), 0)
            config_path = next(Path(folder).glob("*/experiment_config.json"))
            config = json.loads(config_path.read_text(encoding="utf-8"))
            self.assertEqual(config["script_entrypoint"], "压测_1GiB_GET_FCT.py")
            self.assertEqual(config["aggregate_file_bytes"], 1073741824)
            self.assertEqual(config["single_link_file_bytes"], 104857600)
            self.assertEqual(config["file_size_label"], "aggregate: 1GiB (1024MiB); single_link: 100MiB")
            self.assertEqual(config["server_source_path"], "/root/test_xquic_server/upload/1-1GiB")
            self.assertEqual(config["adaptive_arrival"]["mode"], "enforce")
            self.assertEqual(config["single_link_scheduler"], "original_minrtt")
            self.assertEqual(config["measured_planned_get_bytes"], 5 * 1073741824 + 12 * 104857600)
            with (config_path.parent / "test_plan.csv").open(encoding="utf-8-sig", newline="") as handle:
                rows = list(csv.DictReader(handle))
            self.assertEqual(len(rows), 17)
            for row in rows:
                expected = 1073741824 if row["mode"] == "agg" else 104857600
                self.assertEqual(int(row["file_bytes"]), expected)
                self.assertEqual(row["files_per_connection"], "1")

    def test_transfer_flow_verifies_both_sources_and_uses_one_gib_fct_rate(self):
        case = fixture.FiveHundredMiBGet()
        with tempfile.TemporaryDirectory() as folder, \
                patch.object(fixture, "entry", entry), patch.object(fixture, "core", core):
            status, rows = case.run_fake_transfers(folder, True, verify_sha256=True, all_schedulers=True)
            self.assertEqual(status, 0)
            self.assertEqual(len(rows), 17)
            for row in rows:
                expected = 1073741824 if row["mode"] == "agg" else 104857600
                self.assertEqual(row["status"], "SUCCESS")
                self.assertEqual(int(row["filesize"]), expected)
                self.assertAlmostEqual(float(row["mbps"]), expected * 8 / 2000000)
            summaries = list(Path(folder).glob("*/*/aggregation_summary_get_*.csv"))
            self.assertEqual(len(summaries), 5)
            for path in summaries:
                with path.open(encoding="utf-8-sig", newline="") as handle:
                    summary = next(csv.DictReader(handle))
                self.assertEqual(summary["status"], "COMPLETE")
                self.assertAlmostEqual(float(summary["aggregation_efficiency_pct"]), 512.0)


if __name__ == "__main__":
    unittest.main()
