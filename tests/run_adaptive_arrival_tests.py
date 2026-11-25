"""Compile deterministic tests from production code, without TLS or live sockets.

Linux: python3 tests/run_adaptive_arrival_tests.py --cc cc --include /usr/include
Windows: pass Zig's executable with --cc and socket compatibility headers with --include.
Selected connection/queue/timer functions are extracted verbatim because linking the
whole transport would pull in TLS. The generated file and its hash are retained.
"""
import argparse
import hashlib
import json
import os
import re
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    text = (ROOT / source).read_text(encoding="utf-8")
    match = re.search(r"(?m)^(?:[a-zA-Z_][a-zA-Z0-9_]*[ \t]+)?" + re.escape(name) + r"\(", text)
    if not match:
        raise ValueError("function not found: " + name)
    i = match.start()
    start = i if text[i:].startswith("xqc_gp_timer_id_t ") else text.rfind("\n", 0, i - 1) + 1
    end = text.index("\n}", i) + 2
    return text[start:end]


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    out = args.output or Path(tempfile.mkdtemp(prefix="xquic-arrival-tests-"))
    out.mkdir(parents=True, exist_ok=True)
    source_paths = sorted([p for directory in ("src", "include", "mini", "demo")
                           for pattern in ("*.c", "*.h") for p in (ROOT / directory).rglob(pattern)]
                          + [ROOT / "CMakeLists.txt", ROOT / "cmake/adaptive_arrival_build.cmake"],
                          key=lambda p: p.relative_to(ROOT).as_posix())
    hashes = {p.relative_to(ROOT).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in source_paths}
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=ROOT))
    canonical = head + "\n" + "".join(name + " " + digest + "\n" for name, digest in hashes.items())
    build_id = "adaptive-arrival-v1-" + hashlib.sha256(canonical.encode()).hexdigest()
    provenance = dict(source_build_id=build_id, base_head=head, dirty=dirty, source_sha256=hashes)
    (out / "source_manifest.json").write_text(json.dumps(provenance, indent=2), encoding="utf-8")

    selections = {
        "src/transport/scheduler/xqc_scheduler_common.c": [
            "xqc_scheduler_classify_cwnd_block", "xqc_scheduler_get_cwnd", "xqc_scheduler_evaluate_path"],
        "src/transport/xqc_send_ctl.c": ["xqc_send_ctl_calc_effective_cwnd", "xqc_get_pn_ctl", "xqc_send_ctl_decrease_inflight"],
        "demo/netins_udp_mini_api_server.c": ["parse_scheduler"],
        "src/netins_udp/netins_udp_api.c": ["netins_udp_scheduler_name"],
        "src/transport/xqc_packet_out.c": ["xqc_packet_out_create", "xqc_packet_out_copy",
                                          "xqc_packet_out_get", "xqc_packet_out_destroy"],
        "src/transport/xqc_send_queue.c": ["xqc_send_queue_remove_free", "xqc_send_queue_move_to_high_pri"],
        "src/transport/xqc_multipath.c": ["xqc_path_send_buffer_append", "xqc_path_send_buffer_remove", "xqc_path_recent_loss_rate"],
        "src/transport/xqc_conn.c": ["xqc_conn_schedule_packets", "xqc_conn_next_wakeup_time", "xqc_conn_log_sent_packet",
                                    "xqc_on_packets_send_burst", "xqc_send_packet_with_pn"],
        "src/transport/xqc_timer.c": ["xqc_timer_register_gp_timer", "xqc_timer_unregister_gp_timer",
                                      "xqc_timer_destroy_gp_timer"],
    }
    slices = "\n\n".join("/* " + f + " : " + n + " */\n" + function(f, n)
                           for f, names in selections.items() for n in names)
    (out / "arrival_transport_slices.c").write_text(slices, encoding="utf-8")
    flags = ["-std=gnu11", "-DXQC_DISABLE_LOG", "-Wall", "-Werror", "-Wno-unused",
             "-Wno-pointer-sign", "-Wno-format-security", "-I.", "-Iinclude", "-I" + str(out)]
    flags += ["-I" + i for i in args.include]
    flags += ['-DXQC_ARRIVAL_BUILD_ID="' + build_id + '"']
    compiler = [args.cc] + (["cc"] if Path(args.cc).stem.lower() == "zig" else [])
    results = []
    for kind in ["predictor", "runtime"]:
        exe = out / ("test_" + kind + ".exe")
        command = compiler + flags + ["tests/test_adaptive_arrival_" + kind + ".c",
                  "src/transport/scheduler/adaptive_arrival_predictor.c", "-o", str(exe)]
        if Path(args.cc).stem.lower() != "zig":
            command += ["-lm"]
        built = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        print(built.stdout + built.stderr, end="")
        run = None
        if built.returncode == 0:
            env = dict(os.environ)
            if kind == "runtime":
                events = out / "deterministic_events.jsonl"
                events.write_text("", encoding="utf-8")
                env["XQC_ARRIVAL_TEST_LOG"] = str(events)
            run = subprocess.run([str(exe)], cwd=ROOT, text=True, capture_output=True, env=env)
            if kind == "runtime" and run.returncode == 0:
                for line in events.read_text(encoding="utf-8").splitlines():
                    record = json.loads(line)
                    assert record["schema_version"] == 1
                    assert record["family"] == "adaptive_arrival_events"
                print("PASS T28: emitted event JSON parses with the declared schema")
            print(run.stdout + run.stderr, end="")
        result = {"kind": kind, "command": command, "build_exit": built.returncode,
                  "test_exit": run.returncode if run else None, "output": run.stdout + run.stderr if run else built.stderr,
                  "binary_sha256": hashlib.sha256(exe.read_bytes()).hexdigest() if exe.exists() else None}
        results.append(result)
    report = {"source_build_id": build_id, "production_slice_sha256": hashlib.sha256(slices.encode()).hexdigest(), "tests": results}
    (out / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Results:", out / "results.json")
    return int(any(r["build_exit"] or r["test_exit"] for r in results))


if __name__ == "__main__":
    raise SystemExit(main())
