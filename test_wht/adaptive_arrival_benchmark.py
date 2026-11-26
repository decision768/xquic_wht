"""adaptive-v2 options and independent artifact collection for the existing GET benchmark."""
import hashlib
import json
import math
from pathlib import Path
import posixpath
import shlex

DEFAULT_MODE = "enforce"

DEFAULTS = dict(predict_horizon_us=20000, max_defer_episode_us=5000,
    max_defer_stream_us=20000, recheck_interval_us=1000, gain_margin_floor_us=2000,
    gain_margin_rtt_ratio=0.10, min_valid_rate_samples=8, rate_sample_capacity=16,
    ack_batch_capacity=16, min_ack_batches=4, max_sim_events_per_path=64,
    max_waiting_streams=16, schedule_scan_budget=256, prediction_log_limit_per_s=100)


def add_arguments(parser):
    parser.add_argument("--adaptive-arrival-mode", choices=("off", "shadow", "enforce"), default=DEFAULT_MODE,
                        help="adaptive-v2 GET sender mode (default: enforce); off/shadow remain available")
    parser.add_argument("--adaptive-arrival-option", action="append", default=[], metavar="KEY=VALUE",
                        help="Override an arrival v1 field; repeat for multiple fields")


def configuration(args):
    result = dict(DEFAULTS)
    for item in getattr(args, "adaptive_arrival_option", []):
        key, sep, value = item.partition("=")
        if not sep or key not in DEFAULTS:
            raise ValueError("Unknown adaptive arrival option: " + item)
        result[key] = float(value) if key == "gain_margin_rtt_ratio" else int(value)
    if any(not math.isfinite(v) or v < 0 for v in result.values()):
        raise ValueError("Arrival values must be finite and nonnegative")
    c = result
    valid = (0 < c["max_defer_episode_us"] <= c["predict_horizon_us"] <= 1000000
        and c["max_defer_stream_us"] > 0 and c["recheck_interval_us"] > 0
        and 2 <= c["min_valid_rate_samples"] <= c["rate_sample_capacity"] <= 16
        and 4 <= c["min_ack_batches"] <= c["ack_batch_capacity"] <= 16
        and 0 < c["max_sim_events_per_path"] <= 4096 and 0 < c["max_waiting_streams"] <= 16
        and c["max_waiting_streams"] < c["schedule_scan_budget"] <= 65536
        and c["gain_margin_rtt_ratio"] <= 10
        and all(v <= 0xFFFFFFFF for k, v in c.items() if k != "gain_margin_rtt_ratio"))
    if not valid:
        raise ValueError("Invalid adaptive arrival v1 limits")
    return dict(result, mode=getattr(args, "adaptive_arrival_mode", DEFAULT_MODE),
                model_version="arrival_proxy_v1", schema_version=1)


def directory(state_dir):
    return posixpath.join(state_dir, "adaptive_arrival")


def server_arguments(args, scheduler, state_dir):
    if scheduler != "adaptive-v2":
        return ""
    config = configuration(args)
    argv = ["--adaptive-arrival-mode", config["mode"], "--adaptive-arrival-output-prefix",
            posixpath.join(directory(state_dir), "events")]
    for key in DEFAULTS:
        argv += ["--adaptive-arrival-" + key.replace("_", "-"), str(config[key])]
    return " ".join(shlex.quote(s) for s in argv) + " "


def collect(remote, state_dir, log_root):
    """Called only after confirmed server stop, independently of P0 being enabled."""
    remote_dir = directory(state_dir)
    q = shlex.quote
    listing = remote.run("if test -d " + q(remote_dir) + "; then find " + q(remote_dir)
                         + " -maxdepth 1 -type f -print; fi", timeout_s=15)
    if listing.exit_code or listing.timed_out or listing.truncated:
        raise OSError("Cannot enumerate adaptive arrival artifacts; remote files retained")
    local_dir = Path(log_root) / "adaptive_arrival"
    sources = listing.output.splitlines()
    if not sources:
        return
    local_dir.mkdir(parents=True, exist_ok=True)
    manifest_file = local_dir / "manifest.json"
    manifest = json.loads(manifest_file.read_text(encoding="utf-8")) if manifest_file.exists() else {"files": {}}
    for source in sources:
        if posixpath.dirname(source) != remote_dir:
            raise OSError("Unexpected arrival artifact path: " + source)
        destination = local_dir / posixpath.basename(source)
        remote.download(source, destination)
        digest = hashlib.sha256(destination.read_bytes()).hexdigest()
        check = remote.run("sha256sum -- " + q(source), timeout_s=15)
        if check.exit_code or check.timed_out or check.truncated or not check.output.split() or check.output.split()[0] != digest:
            raise OSError("Arrival SHA256 mismatch; remote retained: " + source)
        manifest["files"][source] = dict(local_path=destination.name, sha256=digest,
            size=destination.stat().st_size, incomplete=source.endswith(".active"))
        manifest_file.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        removed = remote.run("rm -- " + q(source), timeout_s=15)
        if removed.exit_code or removed.timed_out:
            raise OSError("Could not retire archived arrival artifact: " + source)
    remote.run("rmdir -- " + q(remote_dir), timeout_s=15)
