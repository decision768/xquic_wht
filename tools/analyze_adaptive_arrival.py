"""Offline arrival-v1 evidence export. Times on different endpoints are never
subtracted without an explicit calibration and error bound.

python tools/analyze_adaptive_arrival.py LOG_DIR --output OUT \
    --fct-csv get_transfers.csv --receiver-events observability/raw/board

Optional --calibration JSON maps transfer_id to server_to_client_offset_us and
error_us. Boundary overlaps are exported as bounds, not exact FCT attribution.
"""
import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path


def records(roots, issues):
    seen = set()
    for root in roots:
        paths = sorted(root.rglob("*.json*")) if root.is_dir() else [root]
        for path in paths:
            if path.resolve() in seen:
                continue
            seen.add(path.resolve())
            if path.name.endswith(".active"):
                issues.append(dict(file=str(path), reason="unsealed_active_file"))
            for n, line in enumerate(path.read_text(encoding="utf-8-sig").splitlines(), 1):
                try:
                    row = json.loads(line)
                except ValueError:
                    if path.suffix in (".jsonl", ".active"):
                        issues.append(dict(file=str(path), line=n, reason="invalid_json"))
                    continue
                if not isinstance(row, dict):
                    continue
                if path.name.endswith(".health.json") and any(row.get(k, 0) for k in
                        ("dropped_records", "dropped", "write_errors", "queue_full_drops", "writer_error")):
                    issues.append(dict(file=str(path), reason="writer_health_failed", health=row))
                yield row


def flatten(row):
    result = {}
    for key, value in row.items():
        if isinstance(value, dict):
            result.update({key + "_" + k: v for k, v in flatten(value).items()})
        elif isinstance(value, list):
            result[key] = json.dumps(value)
        else:
            result[key] = value
    return result


def write_csv(path, rows):
    rows = [flatten(row) for row in rows]
    fields = list(dict.fromkeys(k for row in rows for k in row))
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def union(intervals):
    total, last = 0, None
    for start, end in sorted(intervals):
        if end <= start:
            continue
        if last is None or start >= last:
            total += end-start
        elif end > last:
            total += end-last
        last = max(end, last if last is not None else end)
    return total


def main():
    parser = argparse.ArgumentParser(__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--fct-csv", type=Path)
    parser.add_argument("--receiver-events", type=Path, action="append", default=[])
    parser.add_argument("--calibration", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    issues, predictions, waits, sends, summaries, configs, attempts = [], [], {}, [], {}, {}, []
    for row in records(args.logs, issues):
        if row.get("family") != "adaptive_arrival_events":
            continue
        if row.get("schema_version") != 1:
            issues.append(dict(reason="unsupported_schema", row=row))
            continue
        kind, conn = row.get("event"), row.get("conn_uid")
        if kind == "config":
            configs[conn] = row
            if not row.get("source_build_id") or "unverified" in row["source_build_id"]:
                issues.append(dict(conn_uid=conn, reason="unverified_source_build"))
        elif kind == "prediction":
            predictions.append(row)
        elif kind in ("wait_begin", "wait_recheck", "wait_end", "wait_cancel"):
            key = (conn, row["episode_id"])
            episode = waits.setdefault(key, dict(conn_uid=conn, episode_id=row["episode_id"]))
            if kind == "wait_begin":
                episode.update(row, begin_seen=True, end_seen=False)
            elif kind in ("wait_end", "wait_cancel"):
                episode.update(row, end_seen=True)
            elif episode.get("deadline_us") != row.get("deadline_us"):
                issues.append(dict(conn_uid=conn, episode_id=row["episode_id"], reason="deadline_changed_or_begin_missing"))
        elif kind == "legacy_queued":
            key = (conn, row["episode_id"])
            episode = waits.setdefault(key, dict(conn_uid=conn, episode_id=row["episode_id"]))
            episode.update(final_path_id=row.get("final_path_id"), queued=True,
                           legacy_queued_us=row.get("actual_mono_us"))
        elif kind == "socket_attempt":
            attempts.append(row)
        elif kind == "packet_sent":
            row.setdefault("send_time_basis", "legacy_pre_socket")
            if not isinstance(row["send_time_basis"], str):
                row["send_time_basis"] = "unknown"
            sends.append(row)
        elif kind == "summary":
            summaries[conn] = row
            if row.get("lifecycle_dropped") or row.get("mechanism_complete") is False:
                issues.append(dict(conn_uid=conn, reason="lifecycle_evidence_incomplete"))
    if not configs:
        issues.append(dict(reason="no_arrival_config_event"))
    for conn in configs:
        if conn not in summaries:
            issues.append(dict(conn_uid=conn, reason="connection_summary_missing"))
    for episode in waits.values():
        if not episode.get("begin_seen") or not episode.get("end_seen"):
            issues.append(dict(conn_uid=episode["conn_uid"], episode_id=episode["episode_id"], reason="incomplete_wait_lifecycle"))

    fct = []
    if args.fct_csv:
        with args.fct_csv.open(encoding="utf-8-sig", newline="") as handle:
            fct = list(csv.DictReader(handle))
    windows = {}
    for row in fct:
        tag = row.get("attempt_id") or row.get("transfer_id")
        try:
            lo, hi = int(row["first_get_submit_us"]), int(row["last_body_us"])
            if hi < lo or row.get("get_fct_status") != "ok":
                continue
            windows[tag] = (lo, hi)
        except (ValueError, KeyError, TypeError):
            issues.append(dict(transfer_id=tag, reason="missing_or_invalid_FCT_window"))
    calibration = json.loads(args.calibration.read_text(encoding="utf-8")) if args.calibration else {}
    boundaries = []
    for episode in waits.values():
        tag = episode.get("transfer_id")
        info = calibration.get(tag)
        result = dict(transfer_id=tag, conn_uid=episode["conn_uid"], episode_id=episode["episode_id"])
        if tag not in windows or not info or not episode.get("end_seen"):
            result["reason"] = "no_calibration_or_FCT_window_or_complete_episode"
        else:
            lo, hi = windows[tag]
            offset, error = int(info["server_to_client_offset_us"]), int(info["error_us"])
            if error < 0:
                raise ValueError("Calibration error_us must be nonnegative")
            start, end = episode["first_defer_us"] + offset, episode["actual_mono_us"] + offset
            result.update(overlap_min_us=max(0, min(hi, end-error)-max(lo, start+error)),
                          overlap_max_us=max(0, min(hi, end+error)-max(lo, start-error)), calibration_error_us=error)
            result["reason"] = "exact_overlap" if result["overlap_min_us"] == result["overlap_max_us"] else "boundary_uncertain"
        boundaries.append(result)

    hol, samples, receiver_invalid = defaultdict(list), defaultdict(list), set()
    receiver_end = set()
    for row in records(args.receiver_events, issues):
        tag = row.get("transfer_id") or row.get("attempt_id")
        if row.get("endpoint") not in ("board", "client") or tag not in windows:
            continue
        if row.get("observer_valid") is False or row.get("dropped_records", 0):
            receiver_invalid.add(tag)
        lo, hi = windows[tag]
        if row.get("sample_kind") == "end" and row.get("actual_mono_us", 0) >= hi:
            receiver_end.add(tag)
        if row.get("sample_kind") == "hol_episode":
            hol[tag].append((max(lo, row["start_us"]), min(hi, row["end_us"])))
            if row.get("censored"):
                receiver_invalid.add(tag)
        if isinstance(row.get("sum_B_byte_us"), int) and row.get("aggregate_valid"):
            samples[tag].append(row)
    receiver = []
    for tag, (lo, hi) in windows.items():
        observations = sorted(samples[tag], key=lambda row: row["actual_mono_us"])
        inside = [row for row in observations if lo <= row["actual_mono_us"] <= hi]
        integral, covered = 0, 0
        for a, b in zip(inside, inside[1:]):
            delta = b["sum_B_byte_us"]-a["sum_B_byte_us"]
            if delta < 0:
                receiver_invalid.add(tag)
            else:
                integral += delta
                covered += b["actual_mono_us"]-a["actual_mono_us"]
        complete_B = covered == hi-lo and bool(inside)
        receiver.append(dict(transfer_id=tag, get_fct_us=hi-lo, hol_union_us=union(hol[tag]),
            hol_complete=tag in receiver_end and tag not in receiver_invalid,
            sum_B_integral_known_inside_byte_us=integral, sum_B_integral_exact=complete_B,
            sum_B_peak_sample_lower_bound_B=max((r.get("sum_B", 0) for r in inside), default=None),
            B_boundary_uncovered_us=max(0, hi-lo-covered),
            scope="receiver monotonic GET FCT; boundary slices remain unknown"))
    for name, rows in (("predictions", predictions), ("wait_episodes", waits.values()), ("socket_attempts", attempts),
                       ("packet_sends", sends), ("connection_summaries", summaries.values()),
                       ("file_fct", fct), ("receiver_hol_fct", receiver), ("boundary_uncertainty", boundaries)):
        write_csv(args.output / (name + ".csv"), rows)
    errors_by_basis = defaultdict(list)
    for row in sends:
        if type(row.get("W_error_us")) is int:
            errors_by_basis[row["send_time_basis"]].append(row["W_error_us"])
    error_stats = {basis: dict(samples=len(errors), mean_us=sum(errors) / len(errors))
                   for basis, errors in errors_by_basis.items()}
    error_mean = next(iter(error_stats.values()))["mean_us"] if len(error_stats) == 1 else None
    report = dict(chosen_path_W_error_samples=sum(stats["samples"] for stats in error_stats.values()),
        chosen_path_W_error_mean_us=error_mean,
        chosen_path_W_error_by_basis=error_stats,
        schema_version=1, connections=len(configs), modes=sorted({r["mode"] for r in configs.values()}),
        prediction_records=len(predictions), wait_episodes=len(waits), successful_send_mappings=len(sends),
        connection_counters_scope="connection lifetime, not GET FCT", issues=issues,
        uncalibrated_server_events_are_not_attributed_to_receiver_FCT=True)
    (args.output / "analysis.json").write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "issues"}, ensure_ascii=False))
    print("Evidence issues:", len(issues))


if __name__ == "__main__":
    main()
