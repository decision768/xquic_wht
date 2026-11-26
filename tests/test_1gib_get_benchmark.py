"""Validate the former 1GiB entry's 500MiB aggregate / 100MiB baseline GETs offline."""
import argparse
from contextlib import ExitStack, redirect_stderr, redirect_stdout
import csv
import importlib.util
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


entry = load("mixed_size_get_benchmark_tests", ROOT / "test_wht/压测_1GB_GET_FCT.py")
core = entry.engine


class FiveHundredMiBGet(unittest.TestCase):
    def test_fixed_profile_and_original_defaults(self):
        args = entry.parse_args([])
        self.assertEqual((args.direction, args.get_repeat), ("get", 1))
        self.assertEqual(args.adaptive_arrival_mode, "enforce")
        self.assertEqual(args.board_dir, "/oemdata/gx/xquic_file")
        self.assertEqual(args.download_dir, "/userdata")
        self.assertEqual(args.remote_path, "/upload/1-500MiB")
        self.assertEqual(Path(args.output_root).name, "log_500MB_GET")
        self.assertEqual(args.single_link_remote_path, "/upload/1-100MiB")
        self.assertEqual(args.get_file_bytes_by_mode,
                         {"agg": 524288000, "rmnet": 104857600, "usb": 104857600})
        old = load("original_100mb_get_profile_tests", ROOT / "test_wht/压测_100MB_FCT.py")
        old_args = old.parse_gx_args(["--iterations", "1"])
        self.assertEqual(old_args.adaptive_arrival_mode, "enforce")
        self.assertEqual(old_args.get_repeat, 3)
        self.assertEqual(old.DEFAULT_GET_BYTES, 100 * 1024 * 1024)
        self.assertIsNone(old_args.download_dir)
        old_plan = old.build_test_plan(old_args)
        self.assertTrue(all(c.remote_path == "/upload/1-100MiB" for c in old_plan))
        self.assertTrue(all(c.file_bytes is None for c in old_plan))
        for engine, profile_args in ((core, entry.parse_args(["--iterations", "1"])), (old, old_args)):
            plan = engine.build_test_plan(profile_args)
            aggregates = [case for case in plan if case.mode == "agg"]
            baselines = [case for case in plan if case.mode != "agg"]
            self.assertEqual([case.scheduler for case in aggregates],
                             ["adaptive", "adaptive-v2", "original_minrtt", "blest", "ecf"])
            self.assertEqual(len(baselines), 12)
            self.assertTrue(all(case.scheduler == case.server_scheduler == "original_minrtt"
                                for case in baselines))

    def test_arrival_mode_is_forwarded_only_to_v2_get_sender(self):
        old = load("arrival_mode_original_100mb_tests", ROOT / "test_wht/压测_100MB_FCT.py")
        for mode in ("enforce", "off", "shadow"):
            for parse, engine in ((entry.parse_args, core), (old.parse_gx_args, old)):
                with self.subTest(mode=mode, parse=parse.__name__):
                    argv = ["--iterations", "1", "--schedulers", "adaptive,adaptive-v2"]
                    if mode != "enforce":
                        argv += ["--adaptive-arrival-mode", mode]
                    args = parse(argv)
                    plan = engine.build_test_plan(args)
                    treatment = next(c for c in plan if c.scheduler == "adaptive-v2")
                    self.assertEqual((treatment.adaptive_arrival_mode, treatment.scheduler_label),
                                     (mode, f"adaptive-v2({mode})"))
                    self.assertTrue(all(c.adaptive_arrival_mode == "off" for c in plan
                                        if c.scheduler != "adaptive-v2"))
                    command = engine.arrival_benchmark.server_arguments(args, "adaptive-v2", "/tmp/state")
                    self.assertIn(f"--adaptive-arrival-mode {mode} ", command)
                    self.assertNotIn("--adaptive-arrival-mode",
                                     engine.arrival_benchmark.server_arguments(args, "adaptive", "/tmp/state"))

    def test_custom_urls_keep_sizes_bound_to_the_mode(self):
        args = entry.parse_args(["--iterations", "1", "--remote-path", "/upload/custom-large",
                                 "--single-link-remote-path", "/upload/custom-small"])
        plan = core.build_test_plan(args)
        for case in plan:
            self.assertEqual((case.remote_path, case.file_bytes),
                ("/upload/custom-large", entry.FILE_BYTES) if case.mode == "agg"
                else ("/upload/custom-small", entry.SINGLE_LINK_FILE_BYTES))

    def test_rejects_post_and_three_files(self):
        for argv in (["--direction", "post"], ["--direction", "both"], ["--repeat", "3"],
                     ["--post-local", "/userdata/not-used"],
                     ["--single-link-remote-path", "/upload/1-500MiB"],
                     ["--single-link-remote-path", "/upload/../escape"]):
            with self.subTest(argv=argv), redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as caught:
                    entry.parse_args(argv)
                self.assertEqual(caught.exception.code, 2)

    def test_client_command_runs_from_program_dir_and_downloads_once(self):
        args = entry.parse_args(["--disable-observer"])
        captured = []

        def observe(args, method, *values):
            if method == "command":
                captured.append(values[1])
            return None

        name = core.get_download_path(args, "get-agg-test-measured-adaptive.bin")
        with patch.object(core, "observe", side_effect=observe):
            command = core.build_client_command(args, "agg", "get", name, args.remote_path,
                                                "api_test", False)
        argv = captured[0]
        self.assertIn("--get", argv)
        self.assertNotIn("--post", argv)
        self.assertEqual(argv[argv.index("--local")+1], "/userdata/get-agg-test-measured-adaptive.bin")
        self.assertEqual(argv[argv.index("--repeat")+1], "1")
        self.assertNotIn("--get-confirm-timeout-ms", argv)
        self.assertIn("cd -- /oemdata/gx/xquic_file", command)
        self.assertEqual(argv.count("--interface"), 2)

    def test_source_is_read_only_and_every_url_must_be_exactly_500mib(self):
        for size in (524288000, 1073741824, 100*1024*1024, 524288001):
            calls = []

            def run(command, **kwargs):
                calls.append(command)
                text = f"__MINI_SOURCE_SIZE__={size}\n" if "__MINI_SOURCE_SIZE__" in command else ""
                return core.CommandResult(text, 0, 0)

            remote = SimpleNamespace(run=run)
            if size == entry.FILE_BYTES:
                path, actual = entry.server_preflight(remote, "/root/test_xquic_server",
                    "./netins_udp_mini_api_server", "/upload/1-500MiB")
                self.assertEqual((path, actual), ("/root/test_xquic_server/upload/1-500MiB", size))
            else:
                with self.assertRaises(RuntimeError):
                    entry.server_preflight(remote, "/root/test_xquic_server",
                        "./netins_udp_mini_api_server", "/upload/custom-file")
            for command in calls:
                self.assertNotIn("head -c", command)
                self.assertNotIn("mktemp", command)
                self.assertNotIn("ln -T", command)
                self.assertNotIn("dd ", command)

    def test_baseline_source_must_be_exactly_100mib(self):
        for size in (entry.SINGLE_LINK_FILE_BYTES, entry.FILE_BYTES, entry.SINGLE_LINK_FILE_BYTES - 1):
            remote = SimpleNamespace(run=lambda *a, **k:
                core.CommandResult(f"__MINI_SOURCE_SIZE__={size}\n", 0, 0))
            if size == entry.SINGLE_LINK_FILE_BYTES:
                source, actual = entry.server_preflight(remote, "/root/test_xquic_server",
                    "./netins_udp_mini_api_server", "/upload/custom-small",
                    expected_bytes=entry.SINGLE_LINK_FILE_BYTES)
                self.assertEqual((source, actual), ("/root/test_xquic_server/upload/custom-small", size))
            else:
                with self.assertRaises(RuntimeError):
                    entry.server_preflight(remote, "/root/test_xquic_server",
                        "./netins_udp_mini_api_server", "/upload/custom-small",
                        expected_bytes=entry.SINGLE_LINK_FILE_BYTES)

    def test_disk_space_is_checked_on_userdata(self):
        args = entry.parse_args(["--disable-observer"])
        commands = []

        def run(command, **kwargs):
            commands.append(command)
            if "df -Pk" in command:
                free = 4*entry.FILE_BYTES if "df -Pk /userdata" in command else 174*1024*1024
                return core.CommandResult(f"__MINI_FREE_BYTES__={free}\n", 0, 0)
            return core.CommandResult("", 0, 0)

        core.board_preflight(SimpleNamespace(run=run), args, entry.FILE_BYTES,
                             "test_session", False, require_post_source=False)
        self.assertTrue(any("df -Pk /userdata" in command for command in commands))
        self.assertFalse(any("df -Pk /oemdata" in command for command in commands))
        self.assertTrue(any(": > /userdata/.mini_bench_test_session" in command for command in commands))
        self.assertFalse(any("test -w /oemdata/gx/xquic_file" in command for command in commands))
        with self.assertRaises(RuntimeError):
            core.board_preflight(SimpleNamespace(run=lambda *a, **kw:
                core.CommandResult(f"__MINI_FREE_BYTES__={entry.FILE_BYTES - 1}\n", 0, 0)),
                args, entry.FILE_BYTES, "test_session", False, require_post_source=False)

    def test_cleanup_and_journal_use_the_download_directory(self):
        args = entry.parse_args(["--disable-observer"])
        observer_module = sys.modules["new_roadtest_observability_fct"]
        observer = observer_module.RoadtestObserver.__new__(observer_module.RoadtestObserver)
        observer.args, observer.session = args, "unit_session"
        name = "/userdata/get-agg-unit_session-measured-adaptive-r001-a01.bin"
        self.assertEqual(observer._temporary_parent("board", name), "/userdata")
        for path in ("/userdata/other.bin", "/tmp/" + Path(name).name,
                     "/userdata/get-agg-someone_else-a01.bin"):
            with self.assertRaises(ValueError):
                observer._temporary_parent("board", path)
        commands = []
        remote = SimpleNamespace(is_connected=lambda: True, run=lambda command, **kwargs:
            (commands.append(command) or core.CommandResult("", 0, 0)))
        self.assertTrue(core.cleanup_current_session_remote_artifacts(remote, remote, args,
            "unit_session", "/tmp/mini_api_gx_unit_session", True, ("get",)))
        self.assertIn("find /userdata -maxdepth 1", commands[0])
        self.assertNotIn("find /oemdata/gx/xquic_file", commands[0])

    def test_retained_download_space_sums_the_mixed_file_sizes(self):
        args = entry.parse_args(["--iterations", "1", "--scheduler", "adaptive"])
        args.keep_downloads = True
        args.planned_get_download_bytes = entry.FILE_BYTES + 4 * entry.SINGLE_LINK_FILE_BYTES
        required = args.planned_get_download_bytes + entry.FILE_BYTES // 10
        for free in (required, required - 1):
            remote = SimpleNamespace(run=lambda command, **kwargs:
                core.CommandResult(f"__MINI_FREE_BYTES__={free}\n" if "df -Pk" in command else "", 0, 0))
            if free == required:
                core.board_preflight(remote, args, entry.FILE_BYTES, "mixed_space", False,
                                     require_post_source=False)
            else:
                with self.assertRaises(RuntimeError):
                    core.board_preflight(remote, args, entry.FILE_BYTES, "mixed_space", False,
                                         require_post_source=False)

    def test_dry_run_has_one_file_and_no_ssh(self):
        with tempfile.TemporaryDirectory() as folder, redirect_stdout(io.StringIO()), \
                patch.object(core.SSHRemote, "connect", side_effect=AssertionError("unexpected SSH")):
            self.assertEqual(entry.main(["--iterations", "1", "--schedulers", "adaptive,original_minrtt,adaptive-v2",
                "--dry-run", "--output-root", folder]), 0)
            config_path = next(Path(folder).glob("*/experiment_config.json"))
            config = json.loads(config_path.read_text(encoding="utf-8"))
            self.assertIsNone(config["file_bytes"])
            self.assertEqual(config["aggregate_file_bytes"], entry.FILE_BYTES)
            self.assertEqual(config["benchmark_profile"],
                             "GET_500MiB_aggregate_100MiB_single_link_once_per_connection")
            self.assertEqual(config["file_size_label"], "aggregate: 500MiB; single_link: 100MiB")
            self.assertEqual(config["server_source_path"], "/root/test_xquic_server/upload/1-500MiB")
            self.assertEqual(config["single_link_file_bytes"], entry.SINGLE_LINK_FILE_BYTES)
            self.assertEqual(config["files_per_connection"], 1)
            self.assertEqual(config["board_download_dir"], "/userdata")
            self.assertEqual(config["get_rate_case_statistic"], "single_file_fct_throughput")
            self.assertEqual(config["adaptive_arrival"]["mode"], "enforce")
            self.assertEqual(config["single_link_scheduler"], "original_minrtt")
            with (config_path.parent / "test_plan.csv").open(encoding="utf-8-sig", newline="") as handle:
                plan = list(csv.DictReader(handle))
            self.assertEqual(len(plan), 11)
            self.assertTrue(all(row["direction"] == "get" and row["files_per_connection"] == "1" for row in plan))
            for row in plan:
                expected = entry.FILE_BYTES if row["mode"] == "agg" else entry.SINGLE_LINK_FILE_BYTES
                path = entry.DEFAULT_REMOTE_PATH if row["mode"] == "agg" else entry.DEFAULT_SINGLE_LINK_REMOTE_PATH
                self.assertEqual((int(row["file_bytes"]), row["remote_path"]), (expected, path))
                if row["mode"] != "agg":
                    self.assertEqual((row["scheduler"], row["server_scheduler"]),
                                     ("original_minrtt", "original_minrtt"))
            self.assertEqual(config["measured_planned_get_bytes"],
                             3 * entry.FILE_BYTES + 8 * entry.SINGLE_LINK_FILE_BYTES)
            self.assertEqual(next(row["adaptive_arrival_mode"] for row in plan
                                  if row["scheduler"] == "adaptive-v2"), "enforce")

    def run_fake_transfers(self, folder, include_fct, verify_sha256=False, wrong_baseline_size=False,
                           all_schedulers=False):
        commands = []
        client_argv = []
        beginnings = []

        def current_size():
            argv = client_argv[-1]
            aggregate = argv.count("--interface") == 2
            return entry.FILE_BYTES if aggregate or wrong_baseline_size else entry.SINGLE_LINK_FILE_BYTES

        def source_hash(source):
            return ("a" if source.endswith(entry.DEFAULT_REMOTE_PATH) else "b") * 64

        class Remote:
            def __init__(self, **kwargs):
                self.board = kwargs["hostname"] == entry.DEFAULT_BOARD_SSH_HOST
            def is_connected(self): return True
            def close(self): pass
            def run(self, command, **kwargs):
                result = ""
                if self.board:
                    if "__MINI_LOCAL_SHA256__" in command:
                        path = client_argv[-1][client_argv[-1].index("--remote") + 1]
                        return core.CommandResult("__MINI_LOCAL_SHA256__=" + source_hash(path) + "\n", 0, 0)
                    commands.append(command)
                    size = current_size()
                    fct = dict(iteration=1, iterations=1, attempt_id=None, received_bytes=size,
                        expected_bytes=size, first_get_submit_us=1000000, last_body_us=3000000,
                        get_fct_us=2000000, status="ok")
                    result = (f"transfer iteration=1/1 direction=GET bytes={size} fragments=256/256 "
                              f"elapsed_us=1000000 throughput_mbps={size*8/1000000:.3f} metric=first_body_to_last_body\n"
                              f"__MINI_LOCAL_SIZE__={size}\n__MINI_WALL_SECONDS__=3.000\n__MINI_CLIENT_RC__=0\n")
                    if include_fct:
                        result += "[get_fct] " + json.dumps(fct) + "\n"
                for key in ("output_path", "stderr_path"):
                    if kwargs.get(key):
                        path = Path(kwargs[key]);path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_text(result if key == "output_path" else "", encoding="utf-8")
                return core.CommandResult(result, 0, 3)

        def observe(args, method, *values):
            if method == "command" and values[0] == "board":
                client_argv.append(list(values[1]))
            if method == "end":
                return {"status": "DISABLED", "errors": []}
            if method == "begin":
                beginnings.append(values[1])
            return None

        observer = SimpleNamespace(manifest={"cleanup": {"complete": True}},
            cleanup_temporary=lambda **kwargs: True, _cleanup_idle=lambda endpoint: True,
            release_cleanup_lock=lambda: None)

        def create_observer(args, session, log_root, board, server):
            (Path(log_root) / "observability").mkdir(parents=True, exist_ok=True)
            return observer

        def preflight(server, directory, executable, path, **kwargs):
            return directory + path, kwargs["expected_bytes"]

        replacements = dict(SSHRemote=Remote, connect_ssh_with_optional_prompt=Mock(),
            recover_pending_cleanup=Mock(), server_preflight=Mock(side_effect=preflight),
            get_server_sha256=Mock(side_effect=lambda server, source: source_hash(source)),
            board_preflight=Mock(), validate_server_scheduler_support=Mock(),
            start_server_quiet=Mock(return_value=123), stop_server=Mock(return_value=True),
            create_observer=create_observer, observe=observe,
            cleanup_board_attempt=Mock(return_value=True), cleanup_current_session_remote_artifacts=Mock(return_value=True),
            configure_console_logging=Mock())
        with ExitStack() as stack:
            for name, value in replacements.items():
                stack.enter_context(patch.object(core, name, value))
            stack.enter_context(patch.object(core.LOG, "info"))
            stack.enter_context(patch.object(core.LOG, "error"))
            selection = (["--schedulers", ",".join(entry.DEFAULT_SCHEDULERS)] if all_schedulers
                         else ["--scheduler", "adaptive"])
            status = entry.main(["--iterations", "1", "--disable-observer", *selection,
                "--retries", "0", "--cooldown-s", "0", "--algorithm-cooldown-s", "0", "--output-root", folder]
                + (["--verify-sha256"] if verify_sha256 else []))
        self.assertEqual(replacements["server_preflight"].call_count, 2)
        self.assertEqual({call.args[3]: call.kwargs["expected_bytes"]
                          for call in replacements["server_preflight"].call_args_list},
                         {entry.DEFAULT_REMOTE_PATH: entry.FILE_BYTES,
                          entry.DEFAULT_SINGLE_LINK_REMOTE_PATH: entry.SINGLE_LINK_FILE_BYTES})
        self.assertEqual(replacements["board_preflight"].call_args.args[2], entry.FILE_BYTES)
        self.assertEqual(replacements["get_server_sha256"].call_count, 2 if verify_sha256 else 0)
        expected_cases = 3 * len(entry.DEFAULT_SCHEDULERS) + 2 if all_schedulers else 5
        self.assertEqual(len(commands), expected_cases)
        self.assertEqual(len(client_argv), expected_cases)
        self.assertEqual(replacements["cleanup_board_attempt"].call_count, expected_cases)
        for call in replacements["cleanup_board_attempt"].call_args_list:
            self.assertEqual(call.args[2], "/oemdata/gx/xquic_file")
            self.assertTrue(call.args[3].startswith("/userdata/get-"))
            self.assertFalse(call.args[4])
        for argv in client_argv:
            self.assertEqual(argv[argv.index("--repeat")+1], "1")
            self.assertTrue(argv[argv.index("--local")+1].startswith("/userdata/get-"))
            aggregate = argv.count("--interface") == 2
            path = argv[argv.index("--remote")+1]
            self.assertEqual(path, entry.DEFAULT_REMOTE_PATH if aggregate else entry.DEFAULT_SINGLE_LINK_REMOTE_PATH)
        for info in beginnings:
            size = entry.FILE_BYTES if info["mode"] == "agg" else entry.SINGLE_LINK_FILE_BYTES
            self.assertEqual(info["expected_bytes"], size)
            if info["mode"] != "agg":
                self.assertEqual(info["server_scheduler"], "original_minrtt")
            if verify_sha256:
                self.assertEqual(info["expected_sha256"], ("a" if info["mode"] == "agg" else "b") * 64)
        result_file = next(Path(folder).glob("*/case_results.csv"))
        with result_file.open(encoding="utf-8-sig", newline="") as handle:
            rows = list(csv.DictReader(handle))
        return status, rows

    def test_fake_single_get_flow_uses_fct_rate_and_cleans_each_file(self):
        with tempfile.TemporaryDirectory() as directory:
            status, rows = self.run_fake_transfers(directory, True)
            self.assertEqual(status, 0)
            self.assertEqual(len(rows), 5)
            for row in rows:
                self.assertEqual(row["status"], "SUCCESS")
                size = entry.FILE_BYTES if row["mode"] == "agg" else entry.SINGLE_LINK_FILE_BYTES
                self.assertEqual(int(row["filesize"]), size)
                self.assertEqual(int(row["file_bytes"]), size)
                self.assertEqual(int(row["get_repeat_count"]), 1)
                self.assertAlmostEqual(float(row["mbps"]), size*8/2000000)
            summary = next(Path(directory).glob("*/*/aggregation_summary_get_*.csv"))
            with summary.open(encoding="utf-8-sig", newline="") as handle:
                aggregate = next(csv.DictReader(handle))
            self.assertEqual(aggregate["status"], "COMPLETE")
            self.assertAlmostEqual(float(aggregate["aggregation_efficiency_pct"]), 250.0)

    def test_sha256_is_precomputed_and_checked_per_source(self):
        with tempfile.TemporaryDirectory() as directory:
            status, rows = self.run_fake_transfers(directory, True, verify_sha256=True)
            self.assertEqual(status, 0)
            self.assertTrue(all(row["status"] == "SUCCESS" for row in rows))

    def test_default_algorithms_keep_raw_logs_short_and_full_attempt_ids_in_metadata(self):
        with tempfile.TemporaryDirectory() as directory:
            status, rows = self.run_fake_transfers(directory, True, all_schedulers=True)
            self.assertEqual(status, 0)
            self.assertTrue(all(row["status"] == "SUCCESS" for row in rows))
            self.assertEqual(len(rows), 17)
            self.assertFalse(any(row["scheduler"] == "balanced" for row in rows))
            self.assertTrue(any(row["mode"] == "agg" and row["scheduler"] == "original_minrtt"
                                for row in rows))
            attempts_file = next(Path(directory).glob("*/attempts.jsonl"))
            attempts = [json.loads(line) for line in attempts_file.read_text(encoding="utf-8").splitlines()]
            self.assertEqual(len(attempts), len(rows))
            for record in attempts:
                self.assertTrue(record["attempt_id"].startswith("api_"))
                if record["profile"] == "original_minrtt":
                    self.assertIn("original_minrtt", record["attempt_id"])
                for field in ("raw_stdout", "raw_stderr"):
                    path = Path(record[field])
                    self.assertLess(len(str(path)), 260)
                    self.assertTrue(path.is_file())

    def test_aggregate_sized_response_on_a_100mib_baseline_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            status, rows = self.run_fake_transfers(directory, True, wrong_baseline_size=True)
            self.assertEqual(status, 1)
            for row in rows:
                self.assertEqual(row["status"], "SUCCESS" if row["mode"] == "agg" else "FAILED")

    def test_fake_missing_fct_is_a_failed_measurement(self):
        with tempfile.TemporaryDirectory() as directory:
            status, rows = self.run_fake_transfers(directory, False)
            self.assertEqual(status, 1)
            self.assertTrue(all(row["status"] == "FAILED" and row["get_fct_status"] == "missing_fct_record" for row in rows))


if __name__ == "__main__":
    unittest.main()
