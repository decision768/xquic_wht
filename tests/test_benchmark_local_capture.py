"""Local capture regressions, with an in-memory SSH channel and no network."""
import importlib.util
import itertools
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "test_wht"))
spec = importlib.util.spec_from_file_location(
    "benchmark_local_capture", ROOT / "test_wht/压测_100MB_FCT.py")
core = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = core
spec.loader.exec_module(core)


class Channel:
    def __init__(self, paths):
        self.paths = paths
        self.stdout = b"transfer stdout\n"
        self.stderr = b"diagnostic stderr\n"
        self.exec_command = Mock(side_effect=self.dispatch)
        self.set_combine_stderr = Mock()
        self.close = Mock()

    def dispatch(self, command):
        assert all(path.is_file() for path in self.paths), "capture files must precede dispatch"

    def recv_ready(self):
        return bool(self.stdout)

    def recv(self, size):
        value, self.stdout = self.stdout, b""
        return value

    def recv_stderr_ready(self):
        return bool(self.stderr)

    def recv_stderr(self, size):
        value, self.stderr = self.stderr, b""
        return value

    def exit_status_ready(self):
        return True

    def recv_exit_status(self):
        return 0


def remote_for(paths):
    channel = Channel(paths)
    transport = SimpleNamespace(is_active=lambda: True, open_session=Mock(return_value=channel))
    remote = core.SSHRemote.__new__(core.SSHRemote)
    remote.label = "local capture fixture"
    remote.client = SimpleNamespace(get_transport=lambda: transport)
    return remote, transport, channel


class LocalCapture(unittest.TestCase):
    def test_both_capture_files_exist_before_dispatch_and_stay_separate(self):
        with tempfile.TemporaryDirectory() as folder:
            stdout = Path(folder) / "not-created-yet" / "out.log"
            stderr = stdout.with_name("err.log")
            remote, transport, channel = remote_for([stdout, stderr])
            with patch.object(core.time, "monotonic", side_effect=itertools.count(0, 0.2)), \
                    patch.object(core.time, "sleep"):
                result = remote.run("fixture command", 5, stdout, stderr)
            self.assertEqual(result.exit_code, 0)
            self.assertEqual(result.output, "transfer stdout\n")
            self.assertEqual(result.stderr, "diagnostic stderr\n")
            self.assertEqual(stdout.read_bytes(), b"transfer stdout\n")
            self.assertEqual(stderr.read_bytes(), b"diagnostic stderr\n")
            channel.exec_command.assert_called_once_with("fixture command")
            channel.close.assert_called_once()

    def test_failed_stdout_or_stderr_never_dispatches_a_remote_command(self):
        for fail_stderr in (False, True):
            with self.subTest(fail_stderr=fail_stderr), tempfile.TemporaryDirectory() as folder:
                stdout = Path(folder) / "out.log"
                stderr = Path(folder) / "err.log"
                blocked = stderr if fail_stderr else stdout
                blocked.mkdir()  # Opening a directory as a capture file must fail.
                remote, transport, channel = remote_for([stdout, stderr])
                with self.assertRaisesRegex(RuntimeError, "创建本地原始日志失败"):
                    remote.run("must never launch", 5, stdout, stderr)
                transport.open_session.assert_not_called()
                channel.exec_command.assert_not_called()
                if stdout.is_file():
                    stdout.unlink()  # Also proves the first handle closed after stderr failed.

    def test_long_original_attempt_name_has_a_portable_local_capture_path(self):
        tag = ("api_20261126_124100_d8b731b9_measured_original_minrtt_r001_Agg_"
               "get_i001_a01_1795668345870402400")
        with tempfile.TemporaryDirectory(prefix="xqc-capture-") as folder:
            root = Path(folder)
            old_leaf = tag + ".stdout.log"
            parent = root / ("p" * (261 - len(str(root)) - len(old_leaf) - 2))
            parent.mkdir()
            self.assertEqual(len(str(parent / old_leaf)), 261)
            stem = core.local_attempt_basename(tag)
            self.assertNotEqual(stem, core.local_attempt_basename(tag + "-retry"))
            stdout = parent / (stem + ".stdout.log")
            stderr = parent / (stem + ".stderr.log")
            self.assertLess(len(str(stdout)), 260)
            remote, transport, channel = remote_for([stdout, stderr])
            with patch.object(core.time, "monotonic", side_effect=itertools.count(0, 0.2)), \
                    patch.object(core.time, "sleep"):
                result = remote.run("long attempt fixture", 5, stdout, stderr)
            self.assertEqual(result.exit_code, 0)
            self.assertEqual(stdout.read_bytes(), b"transfer stdout\n")


if __name__ == "__main__":
    unittest.main()
