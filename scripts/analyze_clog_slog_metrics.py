import argparse
import csv
import json
import re
import statistics
from collections import defaultdict
from datetime import datetime
from pathlib import Path


LINE_RE = re.compile(
    r"^\[(?P<ts>\d{4}/\d{2}/\d{2} \d{2}:\d{2}:\d{2} \d{6})\] "
    r"\[(?P<event>[^\]]+)\] (?P<body>.*)$"
)
FIELD_RE = re.compile(r"\|([^|:]+):([^|]*)")


def parse_log_timestamp(raw: str) -> float:
    dt = datetime.strptime(raw, "%Y/%m/%d %H:%M:%S %f")
    return dt.timestamp() * 1000.0


def parse_line(line: str):
    match = LINE_RE.match(line.rstrip("\n"))
    if not match:
        return None

    fields = {}
    for key, value in FIELD_RE.findall(match.group("body")):
        fields[key] = value

    return {
        "time_ms": parse_log_timestamp(match.group("ts")),
        "event": match.group("event"),
        "fields": fields,
        "raw": line.rstrip("\n"),
    }


def load_events(path: Path):
    events = []
    with path.open("r", encoding="utf-8", errors="ignore") as f:
        for line in f:
            parsed = parse_line(line)
            if parsed:
                events.append(parsed)
    return events


def to_int(value):
    if value is None or value == "":
        return None
    try:
        return int(value)
    except ValueError:
        return None


def summarize(values):
    if not values:
        return None
    ordered = sorted(values)
    p95_index = round((len(ordered) - 1) * 0.95)
    return {
        "count": len(ordered),
        "min_ms": ordered[0],
        "avg_ms": statistics.mean(ordered),
        "p50_ms": statistics.median(ordered),
        "p95_ms": ordered[p95_index],
        "max_ms": ordered[-1],
    }


def format_summary(summary):
    if not summary:
        return "count=0"
    return (
        f"count={summary['count']}, "
        f"min={summary['min_ms']:.3f} ms, "
        f"avg={summary['avg_ms']:.3f} ms, "
        f"p50={summary['p50_ms']:.3f} ms, "
        f"p95={summary['p95_ms']:.3f} ms, "
        f"max={summary['max_ms']:.3f} ms"
    )


def write_csv(path: Path, rows, fieldnames):
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def packet_key(fields):
    return (
        to_int(fields.get("path_id")),
        to_int(fields.get("pkt_pns")),
        fields.get("pkt_type"),
        to_int(fields.get("pkt_num")),
    )


def next_packet_sent_after(events):
    next_times = [None] * len(events)
    next_time_ms = None
    for index in range(len(events) - 1, -1, -1):
        if events[index]["event"] == "packet_sent":
            next_time_ms = events[index]["time_ms"]
        next_times[index] = next_time_ms
    return next_times


def prev_packet_received_before(events):
    prev_times = [None] * len(events)
    prev_time_ms = None
    for index, event in enumerate(events):
        if event["event"] == "packet_received":
            prev_time_ms = event["time_ms"]
        prev_times[index] = prev_time_ms
    return prev_times


def analyze_server_sdk(events):
    next_sent = next_packet_sent_after(events)
    prev_recv = prev_packet_received_before(events)

    download_rows = []
    upload_rows = []

    for index, event in enumerate(events):
        if event["event"] != "stream_data_moved":
            continue

        fields = event["fields"]
        direction = (fields.get("from"), fields.get("to"))

        if direction == ("application", "transport"):
            out_time = next_sent[index]
            row = {
                "time_ms": event["time_ms"],
                "stream_id": to_int(fields.get("stream_id")),
                "offset": to_int(fields.get("offset")),
                "length": to_int(fields.get("send_data_size")) or to_int(fields.get("offset")),
                "exit_time_ms": out_time,
                "sdk_time_ms": None if out_time is None else out_time - event["time_ms"],
            }
            download_rows.append(row)

        elif direction == ("transport", "application"):
            in_time = prev_recv[index]
            row = {
                "time_ms": event["time_ms"],
                "stream_id": to_int(fields.get("stream_id")),
                "read_bytes": to_int(fields.get("read")),
                "entry_time_ms": in_time,
                "sdk_time_ms": None if in_time is None else event["time_ms"] - in_time,
            }
            upload_rows.append(row)

    return download_rows, upload_rows


def analyze_client_stream(events):
    rows = []
    packet_batch = []
    last_stream_packet_time = None

    for event in events:
        if event["event"] == "packet_received":
            frame_flag = event["fields"].get("frame_flag", "")
            if "STREAM" in frame_flag:
                last_stream_packet_time = event["time_ms"]
                packet_batch.append(event["time_ms"])
            continue

        if event["event"] != "stream_data_moved":
            continue

        fields = event["fields"]
        if fields.get("from") != "transport" or fields.get("to") != "application":
            continue

        row = {
            "time_ms": event["time_ms"],
            "stream_id": to_int(fields.get("stream_id")),
            "read_bytes": to_int(fields.get("read")),
            "last_packet_time_ms": last_stream_packet_time,
            "batch_start_packet_time_ms": packet_batch[0] if packet_batch else None,
            "last_packet_to_app_ms": None,
            "batch_start_to_app_ms": None,
            "packets_in_batch": len(packet_batch),
        }
        if last_stream_packet_time is not None:
            row["last_packet_to_app_ms"] = event["time_ms"] - last_stream_packet_time
        if packet_batch:
            row["batch_start_to_app_ms"] = event["time_ms"] - packet_batch[0]

        rows.append(row)
        packet_batch = []

    return rows


def analyze_packet_transmission(server_events, client_events):
    server_packets = {}
    for event in server_events:
        if event["event"] != "packet_sent":
            continue
        key = packet_key(event["fields"])
        if None in key:
            continue
        server_packets[key] = event

    rows = []
    for event in client_events:
        if event["event"] != "packet_received":
            continue
        key = packet_key(event["fields"])
        if None in key or key not in server_packets:
            continue

        server_event = server_packets[key]
        raw_delta = event["time_ms"] - server_event["time_ms"]
        rows.append(
            {
                "path_id": key[0],
                "pkt_pns": key[1],
                "pkt_type": key[2],
                "pkt_num": key[3],
                "server_sent_time_ms": server_event["time_ms"],
                "client_received_time_ms": event["time_ms"],
                "raw_delta_ms": raw_delta,
            }
        )

    if rows:
        baseline = min(row["raw_delta_ms"] for row in rows)
        for row in rows:
            row["normalized_delta_ms"] = row["raw_delta_ms"] - baseline
    else:
        baseline = None

    return rows, baseline


def build_report(server_log: Path, client_log: Path):
    server_events = load_events(server_log)
    client_events = load_events(client_log)

    server_download_rows, server_upload_rows = analyze_server_sdk(server_events)
    client_stream_rows = analyze_client_stream(client_events)
    packet_rows, packet_baseline = analyze_packet_transmission(server_events, client_events)

    report = {
        "server_download_sdk": summarize(
            [row["sdk_time_ms"] for row in server_download_rows if row["sdk_time_ms"] is not None]
        ),
        "server_upload_sdk_proxy": summarize(
            [row["sdk_time_ms"] for row in server_upload_rows if row["sdk_time_ms"] is not None]
        ),
        "client_stream_last_packet_to_app": summarize(
            [row["last_packet_to_app_ms"] for row in client_stream_rows if row["last_packet_to_app_ms"] is not None]
        ),
        "client_stream_batch_start_to_app": summarize(
            [row["batch_start_to_app_ms"] for row in client_stream_rows if row["batch_start_to_app_ms"] is not None]
        ),
        "packet_server_to_client_raw": summarize(
            [row["raw_delta_ms"] for row in packet_rows]
        ),
        "packet_server_to_client_normalized": summarize(
            [row["normalized_delta_ms"] for row in packet_rows]
        ),
        "packet_baseline_raw_ms": packet_baseline,
        "server_log": str(server_log),
        "client_log": str(client_log),
    }

    return report, server_download_rows, server_upload_rows, client_stream_rows, packet_rows


def auto_pick_log(candidate: str, fallback_patterns, cwd: Path):
    direct = cwd / candidate
    if direct.exists():
        return direct
    for pattern in fallback_patterns:
        matches = sorted(cwd.glob(pattern))
        if matches:
            return matches[0]
    return direct


def main():
    parser = argparse.ArgumentParser(
        description="Analyze SDK and transport metrics directly from slog/clog line logs."
    )
    parser.add_argument("--server-log", default="slog.log")
    parser.add_argument("--client-log", default="clog.log")
    parser.add_argument("--output-prefix", default="analysis")
    args = parser.parse_args()

    cwd = Path.cwd()
    server_log = auto_pick_log(args.server_log, ["slog*.log"], cwd)
    client_log = auto_pick_log(args.client_log, ["clog*.log"], cwd)

    if not server_log.exists():
        raise SystemExit(f"Server log not found: {server_log}")
    if not client_log.exists():
        raise SystemExit(f"Client log not found: {client_log}")

    report, server_download_rows, server_upload_rows, client_stream_rows, packet_rows = build_report(
        server_log, client_log
    )

    prefix = args.output_prefix
    report_path = cwd / f"{prefix}_summary.json"
    server_download_csv = cwd / f"{prefix}_server_download_sdk.csv"
    server_upload_csv = cwd / f"{prefix}_server_upload_sdk_proxy.csv"
    client_stream_csv = cwd / f"{prefix}_client_stream.csv"
    packet_csv = cwd / f"{prefix}_packet_server_to_client.csv"

    report_path.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
    write_csv(
        server_download_csv,
        server_download_rows,
        ["time_ms", "stream_id", "offset", "length", "exit_time_ms", "sdk_time_ms"],
    )
    write_csv(
        server_upload_csv,
        server_upload_rows,
        ["time_ms", "stream_id", "read_bytes", "entry_time_ms", "sdk_time_ms"],
    )
    write_csv(
        client_stream_csv,
        client_stream_rows,
        [
            "time_ms",
            "stream_id",
            "read_bytes",
            "last_packet_time_ms",
            "batch_start_packet_time_ms",
            "last_packet_to_app_ms",
            "batch_start_to_app_ms",
            "packets_in_batch",
        ],
    )
    write_csv(
        packet_csv,
        packet_rows,
        [
            "path_id",
            "pkt_pns",
            "pkt_type",
            "pkt_num",
            "server_sent_time_ms",
            "client_received_time_ms",
            "raw_delta_ms",
            "normalized_delta_ms",
        ],
    )

    print(f"server_log: {server_log.name}")
    print(f"client_log: {client_log.name}")
    print()
    print("Server download SDK")
    print(f"  {format_summary(report['server_download_sdk'])}")
    print("Server upload SDK proxy")
    print(f"  {format_summary(report['server_upload_sdk_proxy'])}")
    print("Client stream last packet -> app")
    print(f"  {format_summary(report['client_stream_last_packet_to_app'])}")
    print("Client stream batch start -> app")
    print(f"  {format_summary(report['client_stream_batch_start_to_app'])}")
    print("Packet server -> client raw")
    print(f"  {format_summary(report['packet_server_to_client_raw'])}")
    print("Packet server -> client normalized")
    print(f"  {format_summary(report['packet_server_to_client_normalized'])}")
    if report["packet_baseline_raw_ms"] is not None:
        print(f"packet_baseline_raw_ms: {report['packet_baseline_raw_ms']:.3f}")
    print()
    print(f"written: {report_path.name}")
    print(f"written: {server_download_csv.name}")
    print(f"written: {server_upload_csv.name}")
    print(f"written: {client_stream_csv.name}")
    print(f"written: {packet_csv.name}")


if __name__ == "__main__":
    main()
