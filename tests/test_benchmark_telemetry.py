"""Hardware-free regression checks. Run: python -m unittest discover -s tests -p 'test_*.py'."""
import argparse
import importlib.util
import json
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

import benchmark_telemetry as t

PROMPT = "\nEnter button show next subid list | input SUBID:"
SIGNAL = ("25\r\ntest_nw_get_signal_strength()\r\n\r\nOK\r\n"
          " level=4\r\n gsm=0 rssi=0\r\n wcdma=0 rssi=0 ecio=0\r\n"
          " lte=1 rssi=-70 rsrp=-98 rsrq=-7 snr=182\r\n"
          " nr5g=0 rsrp=0 rsrq=0 snr=0\r\n")


def arguments(**overrides):
    p = argparse.ArgumentParser()
    t.add_arguments(p)
    args = p.parse_args([])
    args.board_dir = "/board"
    args.server_dir = "/server"
    args.client_executable = "./client"
    args.server_executable = "./server"
    for key, value in overrides.items():
        setattr(args, key, value)
    return args


class FakeWire:
    def __init__(self, serial=False, late=False, disconnect=False, repeated=False):
        self.serial, self.late, self.disconnect = serial, late, disconnect
        self.repeated = repeated
        self.commands = []
        self.pending = b"" if serial else b"client api start...\nOption >"
        self.ready_at = 0
        self.queries = 0
        self.closed = False

    def write(self, data):
        command = data.decode().strip()
        self.commands.append(command)
        if command == "":
            self.pending += b"/tsfs/usr/bin # "
        elif command.startswith("printf"):
            self.pending += b"printf '__GX_SERIAL_READY__\\n'\r\n__GX_SERIAL_READY__\r\n/tsfs/usr/bin # "
        elif command.startswith("cd"):
            self.pending += b"client api start...\nOption >"
        elif command == "15":
            self.pending += ("15\n20 : INIT\n25 : GET_SIGNAL_STRENGTH" + PROMPT).encode()
        elif command == "20":
            self.pending += ("20\ntest_nw_init()\nOK\n" + PROMPT).encode()
        elif command == "25":
            self.queries += 1
            text = SIGNAL if self.queries == 1 else SIGNAL.replace("rsrp=-98", "rsrp=-81")
            self.pending += ((PROMPT + "\n" if self.repeated else "") + text + PROMPT).encode()
            if self.late and self.queries == 1:
                self.ready_at = time.monotonic() + 0.03
        elif command == "e":
            self.pending = b"/tsfs/usr/bin # "

    def read(self):
        if self.disconnect and self.queries:
            raise OSError("device disconnected")
        if time.monotonic() < self.ready_at or not self.pending:
            time.sleep(0.001)
            return b""
        chunk, self.pending = self.pending[:17], self.pending[17:]
        return chunk

    def close(self):
        self.closed = True


class SignalTests(unittest.TestCase):
    def test_raw_units_invalid_rat_and_zero_are_distinct(self):
        row = t.parse_signal(SIGNAL)
        self.assertEqual(row["lte_snr_raw"], 182)
        self.assertIsNone(row["lte_snr_db"])
        self.assertEqual(row["nr5g_valid"], 0)
        self.assertIsNone(row["nr5g_rsrp"])
        self.assertEqual(t.parse_signal(SIGNAL.replace("rsrq=-7", "rsrq=0"))["lte_rsrq"], 0)

    def test_nr_valid_and_missing_rssi(self):
        row = t.parse_signal(SIGNAL.replace("nr5g=0 rsrp=0 rsrq=0 snr=0", "nr5g=1 rsrp=-105 rsrq=-12 snr=88"))
        self.assertEqual(row["nr5g_rsrp"], -105)
        self.assertIsNone(row["nr5g_rssi"])

    def test_partial_error_or_menu_is_not_a_sample(self):
        for response in ("OK\n" + PROMPT, SIGNAL.split("nr5g=")[0], SIGNAL.replace("OK", "ERROR"),
                         SIGNAL.replace(" rsrq=-7", ""), SIGNAL.replace("lte=1", "lte=2")):
            with self.subTest(response=response[:40]), self.assertRaises(ValueError):
                t.parse_signal(response)

    def run_loop(self, wire, late=False, samples=1):
        stop, ready = threading.Event(), threading.Event()
        events = []

        def emit(kind, **row):
            events.append(dict(kind=kind, **row))
            if sum(e["kind"] == "signal" and e["status"] == "OK" for e in events) >= samples:
                stop.set()

        t.signal_loop(wire, arguments(signal_timeout_s=0.02 if late else 0.1, signal_interval_s=0.001),
                      "5G2" if wire.serial else "5G1", stop, emit, ready, wire.serial)
        self.assertTrue(ready.is_set())
        self.assertTrue(wire.closed)
        self.assertEqual(wire.commands[-1], "e")
        return events

    def test_fragmented_output_and_serial_shell_initialization(self):
        wire = FakeWire(serial=True)
        events = self.run_loop(wire)
        rows = [e for e in events if e["kind"] == "signal"]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["lte_rsrp"], -98)
        self.assertEqual(wire.commands.count("20"), 1)
        self.assertTrue(any(cmd.startswith("cd /oemapp/usr/bin") for cmd in wire.commands))

    def test_late_response_is_drained_before_new_query(self):
        wire = FakeWire(late=True)
        events = self.run_loop(wire, late=True)
        rows = [e for e in events if e["kind"] == "signal"]
        self.assertEqual([r["status"] for r in rows], ["QUERY_FAILED", "OK"])
        self.assertEqual(rows[1]["lte_rsrp"], -81)
        self.assertEqual(wire.queries, 2)

    def test_repeated_menu_is_not_accepted_as_query_response(self):
        wire = FakeWire(repeated=True)
        events = self.run_loop(wire, samples=2)
        rows = [e for e in events if e["kind"] == "signal"]
        self.assertEqual([row["status"] for row in rows], ["OK", "OK"])
        self.assertEqual([row["lte_rsrp"] for row in rows], [-98, -81])

    def test_late_reply_after_repeated_menu_still_belongs_to_old_query(self):
        events = self.run_loop(FakeWire(late=True, repeated=True), late=True)
        rows = [e for e in events if e["kind"] == "signal"]
        self.assertEqual([row["status"] for row in rows], ["QUERY_FAILED", "OK"])
        self.assertEqual(rows[-1]["lte_rsrp"], -81)

    def test_disconnect_closes_port(self):
        wire = FakeWire(serial=True, disconnect=True)
        with self.assertRaises(OSError):
            self.run_loop(wire)
        self.assertTrue(wire.closed)

    def test_unknown_terminal_receives_no_menu_or_interrupt(self):
        wire = FakeWire(serial=True)
        wire.write = lambda data: wire.commands.append(data.decode().strip())
        with self.assertRaises(TimeoutError):
            t.signal_loop(wire, arguments(signal_timeout_s=0.01), "5G2", threading.Event(),
                          lambda *a, **k: None, threading.Event(), True)
        self.assertEqual(wire.commands, [""])
        self.assertTrue(wire.closed)

    def test_serial_login_does_not_log_fragmented_password_echo(self):
        class LoginWire(FakeWire):
            def write(self, data):
                command = data.decode().strip()
                if command == "":
                    self.commands.append(command)
                    self.pending += b"module login: "
                elif command == "root":
                    self.commands.append(command)
                    self.pending += b"Password: "
                elif command == "averylongpasswordwithmultiplechunks":
                    self.pending += (command + "\n/tsfs/usr/bin # ").encode()
                else:
                    super().write(data)

        wire = LoginWire(serial=True)
        stop, ready, events = threading.Event(), threading.Event(), []

        def emit(kind, **row):
            events.append(dict(kind=kind, **row))
            if kind == "signal":
                stop.set()

        t.signal_loop(wire, arguments(serial_login_password="averylongpasswordwithmultiplechunks", signal_timeout_s=0.1),
                      "5G2", stop, emit, ready, True)
        raw = "".join(e["text"] for e in events if e["kind"] == "raw")
        self.assertNotIn("averylong", raw)
        self.assertNotIn("multiplechunks", raw)
        self.assertIn("[AUTH_OUTPUT_OMITTED]", raw)


class RatesAndAssociationTests(unittest.TestCase):
    def test_network_stream_clock_and_missing_interface(self):
        wire = FakeWire()
        wire.pending = ("GX_CLOCK boot 100.0\n"
                        "GX_NET boot usb0 21 100 100 0 0\n"
                        "GX_NET boot usb0 21 102 102 2000000 1000000\n"
                        "GX_MISSING usb0 103\n"
                        "GX_NET boot usb0 21 104 104 3000000 2000000\n").encode()
        stop, ready, events = threading.Event(), threading.Event(), []

        def emit(kind, **row):
            events.append(dict(kind=kind, **row))
            if sum(e["kind"] == "network" for e in events) == 4:
                stop.set()

        t.network_loop(wire, 0.5, stop, emit, time.monotonic(), ready)
        rows = [e for e in events if e["kind"] == "network"]
        self.assertEqual([r["status"] for r in rows], ["BASELINE", "OK", "INTERFACE_MISSING", "BASELINE"])
        self.assertEqual(rows[1]["tx_mbps"], 8)
        self.assertEqual(rows[1]["rx_mbps"], 4)
        self.assertGreater(rows[1]["clock_uncertainty_s"], 0)
        self.assertTrue(wire.closed)

    def test_rates_use_actual_elapsed_and_reset_on_recreation(self):
        rates = t.NetRates()
        self.assertEqual(rates.sample("boot", "usb0", 21, 1, 1, 100, 200)["status"], "BASELINE")
        row = rates.sample("boot", "usb0", 21, 3, 3, 2000100, 1000200)
        self.assertEqual(row["tx_mbps"], 8)
        self.assertEqual(row["rx_mbps"], 4)
        self.assertEqual(rates.sample("boot", "usb0", 21, 4, 4, 1, 2)["status"], "COUNTER_RESET")
        self.assertEqual(rates.sample("boot", "usb0", 22, 5, 5, 100, 200)["status"], "COUNTER_RESET")
        self.assertEqual(rates.sample("newboot", "usb0", 22, 5, 5, 100, 200)["status"], "COUNTER_RESET")

    def test_boundary_and_retries_are_not_mixed(self):
        ix = t.AttemptIndex({"a": dict(run_tag="a", start=10, end=20, attempt=1),
                             "b": dict(run_tag="b", start=22, end=30, attempt=2)})
        self.assertEqual(ix.associate(11, 12)["run_tag"], "a")
        self.assertEqual(ix.associate(23, 24)["attempt"], 2)
        for start, end in ((9, 11), (19, 23), (21, 23), (29, 31)):
            self.assertEqual(ix.associate(start, end)["phase"], "BOUNDARY")
        self.assertEqual(ix.associate(20.1, 21)["phase"], "COOLDOWN_OR_CLEANUP")
        self.assertEqual(ix.associate(1, 2)["phase"], "INITIALIZATION")

    def test_paths_use_interface_not_index(self):
        rows = t.path_snapshots("connection multipath=1 paths=2 active=2 srtt_us=10 inflight=75\n"
                               "path[0] id=9 state=1 app=0 iface=usb0 local=x peer=y tx=12 rx=40 srtt_us=12000\n"
                               "path[1] id=4 state=1 app=0 iface=rmnet_data0 local=x peer=y tx=14 rx=41 srtt_us=0\n")
        self.assertEqual(rows[0]["module"], "5G2")
        self.assertEqual(rows[0]["connection_inflight_bytes"], 75)
        self.assertNotIn("inp_packets", rows[0])
        self.assertIsNone(rows[1]["srtt_us"])


def fixture_events(root):
    raw = root / "client.stdout.log"
    raw.write_text("connection multipath=1 paths=2 active=2 srtt_us=10 inflight=75\n"
                   "path[0] id=9 state=1 app=0 iface=usb0 local=192.168.225.35:123 peer=1.2.3.4:555 tx=120 rx=400 srtt_us=12000\n", encoding="utf-8")
    cfg = vars(arguments())
    cfg.pop("serial_login_password")
    transport_path = root / "server.jsonl"
    events = [dict(kind="config", values=cfg), dict(kind="definition", reason=t.UNSUPPORTED),
              dict(kind="attempt_start", run_tag="a", start=10, start_utc="2026-09-07T08:00:00+00:00",
                   round=1, mode="agg", direction="get", attempt=1, raw_stdout_path=str(raw)),
              dict(kind="attempt_end", run_tag="a", end=20, end_utc="2026-09-07T08:00:10+00:00"),
              dict(kind="attempt_result", run_tag="a", success=False, error="=DANGEROUS()", total_bytes=1000,
                   throughput_mbps=0.008, transfer_elapsed_s=1, metric="first_body_to_last_body"),
              dict(kind="signal", source="5G1", start=11, end=12, status="OK", **t.parse_signal(SIGNAL)),
              dict(kind="signal", source="5G2", start=12, end=13, status="OK", **t.parse_signal(SIGNAL)),
              dict(kind="signal", source="5G2", start=19, end=21, status="OK", **t.parse_signal(SIGNAL.replace("rsrp=-98", "rsrp=-20"))),
              dict(kind="network", start=11.5, end=12.5, status="OK", interface="usb0", tx_mbps=8, rx_mbps=4),
              dict(kind="network", start=12.5, end=13, status="OK", interface="usb0", tx_mbps=4, rx_mbps=2),
              dict(kind="network", start=12, end=13, status="OK", interface="rmnet_data0", tx_mbps=4, rx_mbps=8),
              dict(kind="transport", endpoint="board", schema_version=1, attempt_id="a", role="client",
                   direction="get", sample_kind="start", actual_mono_us=1000000,
                   paths=[dict(path_id=9, interface_name="usb0")]),
              dict(kind="transport", endpoint="server", schema_version=1, attempt_id="a", role="server",
                   direction="get", sample_kind="start", actual_mono_us=2000000,
                   connection_credit_bytes=2000, active_stream_credit_bytes=1500, effective_swnd_bytes=1500,
                   paths=[dict(path_id=9, path_state=2, app_status=2, cwnd_bytes=12000,
                               effective_cwnd_bytes=10000, inflight_packets=12, inflight_bytes=9000,
                               srtt_us=12000, sent_bytes=1000)]),
              dict(kind="transport", endpoint="server", schema_version=1, attempt_id="a", role="server",
                   direction="get", sample_kind="periodic", actual_mono_us=2500000,
                   connection_credit_bytes=1800, active_stream_credit_bytes=1300, effective_swnd_bytes=1300,
                   paths=[dict(path_id=9, path_state=2, app_status=2, cwnd_bytes=10000,
                               effective_cwnd_bytes=9000, inflight_packets=10, inflight_bytes=8000,
                               srtt_us=10000, sent_bytes=1001000)]),
              dict(kind="collector", source="5G2", status="FAILED", recorded_mono=18, reason="串口断开"),
              dict(kind="attempt_start", run_tag="b", start=22, round=1, mode="agg", direction="get", attempt=2)]
    journal = root / "events.jsonl"
    journal.write_text("".join(json.dumps(e, ensure_ascii=False) + "\n" for e in events), encoding="utf-8")
    return journal


class WorkbookAndIsolationTests(unittest.TestCase):
    def test_transport_ingest_validates_and_splits_server_jsonl(self):
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            sample = Path(__file__).with_name("fixtures") / "transport_telemetry" / "server_get.jsonl"
            source = root / "server.jsonl"
            source.write_text(sample.read_text(encoding="utf-8") + "{broken\n", encoding="utf-8")
            session = t.TelemetrySession(arguments(), "session", root)
            self.assertEqual(session.ingest_transport(source, "server"), 2)
            session.file.close()
            split = root / "telemetry" / "session" / "transport" / "server" / "fixture-get.jsonl"
            self.assertEqual(len(split.read_text(encoding="utf-8").splitlines()), 2)
            self.assertIn("transport_error", session.journal.read_text(encoding="utf-8"))

    def test_export_preserves_missing_units_boundary_and_failures(self):
        from openpyxl import load_workbook
        with tempfile.TemporaryDirectory() as name:
            root = Path(name)
            journal = fixture_events(root)
            book = t.export_workbook(journal, root / "out.xlsx")
            wb = load_workbook(book)
            self.assertEqual(wb.sheetnames, ["周期采样", "传输汇总", "配置与异常"])

            def rows(sheet):
                data = list(wb[sheet].values)
                return [dict(zip(data[0], r)) for r in data[1:]]

            summary = rows("传输汇总")
            self.assertFalse(summary[0]["success"])
            self.assertEqual(summary[0]["signal_5g2_samples"], 1)
            self.assertEqual(summary[0]["transport_samples"], 2)
            self.assertEqual(summary[0]["collection_status"], "SAMPLED")
            self.assertIsNone(summary[1]["success"])
            self.assertIn("中断", summary[1]["error"])
            periodic = rows("周期采样")
            path_row = next(row for row in periodic if row["path_id"] == 9)
            self.assertEqual(path_row["module"], "5G2")
            self.assertAlmostEqual(path_row["cwnd_rtt_mbps"], 8.0, places=3)
            self.assertAlmostEqual(path_row["inp_rtt_packets_s"], 1000.0, places=3)
            self.assertIsNone(path_row["nr5g_rsrp"])
            network_row = next(row for row in periodic
                               if row["path_id"] is None and row["module"] == "5G2"
                               and row["window_index"] == 2)
            self.assertAlmostEqual(network_row["network_tx_mbps"], 6.0, places=3)
            self.assertAlmostEqual(network_row["network_rx_mbps"], 3.0, places=3)
            self.assertEqual(wb["传输汇总"]["G2"].data_type, "s")
            self.assertTrue(all(s.freeze_panes and s.auto_filter.ref for s in wb))
            wb.close()

    def test_secret_redaction_and_interrupted_export(self):
        with tempfile.TemporaryDirectory() as name:
            s = t.TelemetrySession(arguments(serial_login_password="secret123"), "session", name)
            s.emit("raw", source="5G2", text="echo secret123")
            s.begin_attempt("a", round=1, mode="usb", direction="post", attempt=1)
            s.end_attempt("a")
            s.finish()
            text = s.journal.read_text(encoding="utf-8")
            self.assertNotIn("secret123", text)
            self.assertNotIn("serial_login_password", text)
            self.assertIn("[REDACTED]", text)
            self.assertIn("测试被中断", text)
            self.assertTrue((Path(name) / "experiment_session.xlsx").is_file())

    def test_capture_and_excel_failures_are_nonfatal(self):
        with tempfile.TemporaryDirectory() as name:
            s = t.TelemetrySession(arguments(), "session", name)
            ready = threading.Event()
            s._worker("5G2", Mock(side_effect=OSError("COM11 busy")), ready)
            self.assertTrue(ready.is_set())
            with patch.object(t, "export_workbook", side_effect=PermissionError("locked")):
                self.assertIsNone(s.finish())
            self.assertIn("COM11 busy", s.journal.read_text(encoding="utf-8"))
        self.assertIsNone(t.telemetry_call(Mock(start=Mock(side_effect=RuntimeError("error"))), "start"))

    def test_broken_final_journal_line_is_reported(self):
        with tempfile.TemporaryDirectory() as name:
            p = Path(name) / "events.jsonl"
            p.write_text('{"kind":"raw"}\n{"kind":', encoding="utf-8")
            self.assertEqual(list(t.read_events(p))[-1]["kind"], "journal_error")


class MainFlowTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        spec = importlib.util.spec_from_file_location("gx_benchmark", Path(__file__).with_name("压测脚本.py"))
        cls.main = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.main
        spec.loader.exec_module(cls.main)

    def test_defaults_and_existing_overrides(self):
        a = self.main.parse_gx_args([])
        self.assertEqual(a.order, ["rmnet", "agg", "usb"])
        self.assertEqual(a.serial_port, "COM11")
        self.assertEqual(a.transport_telemetry_interval_ms, 250)
        a = self.main.parse_gx_args(["--order", "usb,agg,rmnet", "--direction", "post", "--serial-port", "COM12",
                                    "--signal-interval-s", "2", "--cc", "cubic"])
        self.assertEqual(a.order, ["usb", "agg", "rmnet"])
        self.assertEqual(a.signal_interval_s, 2)
        self.assertEqual(self.main.transfer_algorithms_for_direction(a, "post")[1], "cubic")

    def test_client_command_adds_optional_transport_telemetry(self):
        args = self.main.parse_gx_args([])
        command = self.main.build_client_command(
            args, "agg", "post", "/tmp/in", "/upload/out", "safe-id", False)
        self.assertIn("--attempt-id safe-id", command)
        self.assertIn("--telemetry-output /tmp/safe-id_transport.jsonl", command)
        args.disable_telemetry = True
        command = self.main.build_client_command(
            args, "agg", "post", "/tmp/in", "/upload/out", "safe-id", False)
        self.assertNotIn("--telemetry-output", command)

    def test_six_modes_and_retry_order_unchanged_when_telemetry_fails(self):
        m = self.main
        seen, outputs = [], []
        remote = Mock()
        remote.is_connected.return_value = True
        remote.run.side_effect = lambda *a, **k: m.CommandResult(output="", exit_code=0, controller_elapsed_s=1)
        parsed = dict(total_bytes=100, transfer_elapsed_s=1, throughput_mbps=0.0008)

        def command(args, mode, direction, *unused):
            seen.append((direction, mode))
            return "mock-client"

        def parse(*unused):
            return len(seen) != 1, parsed, "first attempt failed" if len(seen) == 1 else ""

        noops = ["connect_ssh_with_optional_prompt", "board_preflight", "validate_server_resource_monitor",
                 "cleanup_stale_board_resource_files", "cleanup_stale_server_resource_files", "validate_server_post_targets",
                 "stop_server_resource_monitor", "cleanup_current_session_remote_artifacts", "write_speed_log"]
        mocks = {key: Mock() for key in noops}
        mocks.update(SSHRemote=Mock(return_value=remote), server_preflight=Mock(return_value=("/file", 100)),
                     get_board_post_source_info=Mock(return_value=(100, "")), start_server_quiet=Mock(return_value=999),
                     prepare_server_post_target=Mock(return_value=True), start_server_resource_monitor=Mock(return_value={}),
                     build_client_command=command, parse_client_result=parse,
                     inspect_and_cleanup_server_post=Mock(return_value=("", True)), cleanup_board_attempt=Mock(return_value=True),
                     extract_resource_status=Mock(return_value="OK"), collect_board_resource_gx=Mock(return_value={}),
                     collect_server_resource=Mock(return_value={}), stop_server=Mock(return_value=True),
                     write_aggregation_summary=Mock(return_value=Path("aggregation.csv")),
                     write_gx_final_summary=lambda root, session, records, *a: outputs.extend(records) or Path("summary.csv"),
                     create_telemetry_session=Mock(return_value=Mock(**{k: Mock(side_effect=RuntimeError("collector failure"))
                                                                       for k in ("start", "begin_attempt", "end_attempt", "result", "stop_collectors", "finish")})))
        with tempfile.TemporaryDirectory() as name, patch.multiple(m, **mocks):
            rc = m.gx_main(["--output-root", name, "--cooldown-s", "0"])
        self.assertEqual(rc, 0)
        self.assertEqual(seen, [("get", "rmnet"), ("get", "rmnet"), ("get", "agg"), ("get", "usb"),
                                ("post", "rmnet"), ("post", "agg"), ("post", "usb")])
        self.assertEqual(len(outputs), 6)
        self.assertTrue(all(row["success"] for row in outputs))


if __name__ == "__main__":
    unittest.main()
