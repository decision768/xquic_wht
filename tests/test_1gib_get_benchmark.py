"""Offline validation of the one-file 1GiB GET entry point; no SSH or large files."""
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


entry = load("one_gib_benchmark_tests", ROOT / "test_wht/压测_1GB_GET_FCT.py")
core = entry.engine


class OneGiBGet(unittest.TestCase):
    def test_fixed_profile_and_original_defaults(self):
        args = entry.parse_args([])
        self.assertEqual((args.direction, args.get_repeat), ("get", 1))
        self.assertEqual(args.board_dir, "/oemdata/gx/xquic_file")
        self.assertEqual(args.download_dir, "/userdata")
        self.assertEqual(args.remote_path, "/upload/1-1GiB")
        self.assertEqual(args.file_bytes, 1073741824)
        old = load("original_100mb_get_profile_tests", ROOT / "test_wht/压测_100MB_FCT.py")
        old_args = old.parse_gx_args([])
        self.assertEqual(old_args.get_repeat, 3)
        self.assertEqual(old.DEFAULT_GET_BYTES, 100 * 1024 * 1024)
        self.assertIsNone(old_args.download_dir)

    def test_rejects_post_and_three_files(self):
        for argv in (["--direction", "post"], ["--direction", "both"], ["--repeat", "3"],
                     ["--post-local", "/userdata/not-used"]):
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

    def test_source_is_read_only_and_every_url_must_be_exactly_1gib(self):
        for size in (1073741824, 100*1024*1024, 1073741825):
            calls = []

            def run(command, **kwargs):
                calls.append(command)
                text = f"__MINI_SOURCE_SIZE__={size}\n" if "__MINI_SOURCE_SIZE__" in command else ""
                return core.CommandResult(text, 0, 0)

            remote = SimpleNamespace(run=run)
            if size == entry.FILE_BYTES:
                path, actual = entry.server_preflight(remote, "/root/test_xquic_server",
                    "./netins_udp_mini_api_server", "/upload/1-1GiB")
                self.assertEqual((path, actual), ("/root/test_xquic_server/upload/1-1GiB", size))
            else:
                with self.assertRaises(RuntimeError):
                    entry.server_preflight(remote, "/root/test_xquic_server",
                        "./netins_udp_mini_api_server", "/upload/custom-file")
            for command in calls:
                self.assertNotIn("head -c", command)
                self.assertNotIn("mktemp", command)
                self.assertNotIn("ln -T", command)
                self.assertNotIn("dd ", command)

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
                core.CommandResult("__MINI_FREE_BYTES__=1000000000\n", 0, 0)),
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

    def test_dry_run_has_one_file_and_no_ssh(self):
        with tempfile.TemporaryDirectory() as folder, redirect_stdout(io.StringIO()), \
                patch.object(core.SSHRemote, "connect", side_effect=AssertionError("unexpected SSH")):
            self.assertEqual(entry.main(["--iterations", "1", "--schedulers", "adaptive,original_minrtt,adaptive-v2",
                "--adaptive-arrival-mode", "shadow", "--dry-run", "--output-root", folder]), 0)
            config_path = next(Path(folder).glob("*/experiment_config.json"))
            config = json.loads(config_path.read_text(encoding="utf-8"))
            self.assertEqual((config["file_bytes"], config["files_per_connection"]), (entry.FILE_BYTES, 1))
            self.assertEqual(config["board_download_dir"], "/userdata")
            self.assertEqual(config["get_rate_case_statistic"], "single_file_fct_throughput")
            with (config_path.parent / "test_plan.csv").open(encoding="utf-8-sig", newline="") as handle:
                plan = list(csv.DictReader(handle))
            self.assertEqual(len(plan), 11)
            self.assertTrue(all(row["direction"] == "get" and row["files_per_connection"] == "1" for row in plan))
            self.assertEqual(next(row["adaptive_arrival_mode"] for row in plan
                                  if row["scheduler"] == "adaptive-v2"), "shadow")

    def run_fake_transfers(self, folder, include_fct):
        commands = []
        client_argv = []
        size = entry.FILE_BYTES
        fct = dict(iteration=1, iterations=1, attempt_id=None, received_bytes=size,
            expected_bytes=size, first_get_submit_us=1000000, last_body_us=3000000,
            get_fct_us=2000000, status="ok")
        stdout = (f"transfer iteration=1/1 direction=GET bytes={size} fragments=256/256 "
                  f"elapsed_us=1000000 throughput_mbps={size*8/1000000:.3f} metric=first_body_to_last_body\n"
                  f"__MINI_LOCAL_SIZE__={size}\n__MINI_WALL_SECONDS__=3.000\n__MINI_CLIENT_RC__=0\n")
        if include_fct:
            stdout += "[get_fct] " + json.dumps(fct) + "\n"

        class Remote:
            def __init__(self, **kwargs):
                self.board = kwargs["hostname"] == entry.DEFAULT_BOARD_SSH_HOST
            def is_connected(self): return True
            def close(self): pass
            def run(self, command, **kwargs):
                if self.board:
                    commands.append(command)
                result = stdout if self.board else ""
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
            return None

        observer = SimpleNamespace(manifest={"cleanup": {"complete": True}},
            cleanup_temporary=lambda **kwargs: True, _cleanup_idle=lambda endpoint: True,
            release_cleanup_lock=lambda: None)

        def create_observer(args, session, log_root, board, server):
            (Path(log_root) / "observability").mkdir(parents=True, exist_ok=True)
            return observer

        replacements = dict(SSHRemote=Remote, connect_ssh_with_optional_prompt=Mock(),
            recover_pending_cleanup=Mock(), server_preflight=Mock(return_value=("/root/test_xquic_server/upload/1-1GiB", size)),
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
            status = entry.main(["--iterations", "1", "--scheduler", "adaptive", "--disable-observer",
                "--retries", "0", "--cooldown-s", "0", "--algorithm-cooldown-s", "0", "--output-root", folder])
        self.assertEqual(len(commands), 5)
        self.assertEqual(len(client_argv), 5)
        self.assertEqual(replacements["cleanup_board_attempt"].call_count, 5)
        for call in replacements["cleanup_board_attempt"].call_args_list:
            self.assertEqual(call.args[2], "/oemdata/gx/xquic_file")
            self.assertTrue(call.args[3].startswith("/userdata/get-"))
            self.assertFalse(call.args[4])
        for argv in client_argv:
            self.assertEqual(argv[argv.index("--repeat")+1], "1")
            self.assertTrue(argv[argv.index("--local")+1].startswith("/userdata/get-"))
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
                self.assertEqual(int(row["filesize"]), entry.FILE_BYTES)
                self.assertEqual(int(row["get_repeat_count"]), 1)
                self.assertAlmostEqual(float(row["mbps"]), entry.FILE_BYTES*8/2000000)

    def test_fake_missing_fct_is_a_failed_measurement(self):
        with tempfile.TemporaryDirectory() as directory:
            status, rows = self.run_fake_transfers(directory, False)
            self.assertEqual(status, 1)
            self.assertTrue(all(row["status"] == "FAILED" and row["get_fct_status"] == "missing_fct_record" for row in rows))


if __name__ == "__main__":
    unittest.main()
