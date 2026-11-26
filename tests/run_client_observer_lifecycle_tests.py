"""Run production client/observer lifecycle control flow without TLS or sockets.

Use --source-ref 286db9ca to reproduce the pre-fix close ordering from Git history.
Compiler may be GCC/Clang or the Zig executable. Artifacts stay outside the repo.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--source-ref")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    out = args.output or Path(tempfile.mkdtemp(prefix="xquic-client-observer-"))
    out.mkdir(parents=True, exist_ok=True)
    functions = {
        "mini/mini_observer.c": ["mini_observer_now", "mini_observer_finish", "mini_observer_stop", "mini_observer_resume"],
        "mini/mini_client.c": ["xqc_mini_cli_set_transfer_error", "xqc_mini_cli_maybe_finish_transfer",
                               "xqc_mini_cli_finish_transfer_observation"],
        "mini/mini_client_cb.c": ["xqc_mini_cli_h3_conn_close_notify"],
        "mini/mini_client_backend.c": ["mini_backend_client_abort_transfer", "mini_backend_client_transfer"],
    }
    slices, hashes = [], {}
    for source, names in functions.items():
        raw = (subprocess.check_output(["git", "show", args.source_ref + ":" + source], cwd=ROOT)
               if args.source_ref else (ROOT / source).read_bytes())
        hashes[source] = hashlib.sha256(raw).hexdigest()
        text = raw.decode("utf-8").replace("\r\n", "\n")
        for name in names:
            match = re.search(r"(?m)^" + re.escape(name) + r"\(", text)
            if not match:
                if args.source_ref and name == "xqc_mini_cli_finish_transfer_observation":
                    continue  # The regression tests exercise the callers, not this new helper directly.
                raise ValueError("missing production function: " + name)
            start = text.rfind("\n", 0, match.start() - 1) + 1
            if text[:start].endswith("static\n"):
                start -= len("static\n")
            end = text.index("\n}", match.start()) + 2
            slices.append("/* " + source + " */\n" + text[start:end])
    body = "\n\n".join(slices)
    (out / "client_observer_lifecycle_slices.c").write_text(body, encoding="utf-8")
    cc = [args.cc] + (["cc"] if Path(args.cc).stem.lower() == "zig" else [])
    exe = out / "client_observer_lifecycle.exe"
    cmd = cc + ["-std=gnu11", "-Wall", "-Werror", "-Wno-unused", "-I" + str(out),
                str(ROOT / "tests/test_client_observer_lifecycle.c"), "-o", str(exe)]
    env = os.environ.copy()
    env["PATH"] = str(Path(args.cc).resolve().parent) + os.pathsep + env.get("PATH", "")
    built = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, env=env)
    print(built.stdout + built.stderr, end="")
    run = subprocess.run([str(exe)], capture_output=True, text=True, env=env) if built.returncode == 0 else None
    if run:
        print(run.stdout + run.stderr, end="")
    report = dict(source_ref=args.source_ref, source_sha256=hashes,
                  slice_sha256=hashlib.sha256(body.encode()).hexdigest(), command=cmd,
                  build_exit=built.returncode, test_exit=run.returncode if run else None,
                  output=(run.stdout + run.stderr) if run else built.stderr)
    (out / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print("Results:", out / "results.json")
    return int(bool(built.returncode or (run and run.returncode)))


if __name__ == "__main__":
    raise SystemExit(main())
