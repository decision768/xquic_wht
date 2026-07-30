#!/usr/bin/env python3
"""Validate completeness and performance thresholds for stage 8 results."""

from __future__ import annotations

import argparse
import csv
import json
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any


COMBOS = ("mini-mini", "api-api", "api-mini", "mini-api")
NETWORKS = ("5G1", "5G2", "Agg")
DIRECTIONS = ("GET", "POST")
SIZES = (200 * 1024 * 1024, 1024 * 1024 * 1024)
ROUNDS = (1, 2, 3, 4, 5)


def key(result: dict[str, Any]) -> tuple[str, str, str, int]:
    return (
        result["combo"],
        result["direction"],
        result["network"],
        int(result["size_bytes"]),
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("results_dir", type=Path)
    parser.add_argument("--minimum-api-ratio", type=float, default=0.95)
    parser.add_argument("--minimum-aggregation-efficiency", type=float, default=0.80)
    parser.add_argument(
        "--minimum-agg-mbps",
        type=float,
        default=None,
        help="optional formal Agg floor, for example 205",
    )
    args = parser.parse_args()

    results = [
        json.loads(path.read_text(encoding="utf-8"))
        for path in sorted(args.results_dir.glob("*.json"))
        if path.name != "summary.json"
    ]
    expected_cases = {
        (combo, direction, network, size, round_number)
        for combo in COMBOS
        for direction in DIRECTIONS
        for network in NETWORKS
        for size in SIZES
        for round_number in ROUNDS
    }
    observed_cases = {
        (
            item["combo"],
            item["direction"],
            item["network"],
            int(item["size_bytes"]),
            int(item["round"]),
        )
        for item in results
        if item.get("stage") == 8
    }
    missing = sorted(expected_cases - observed_cases)
    duplicates = len(observed_cases) != len(
        [item for item in results if item.get("stage") == 8]
    )

    grouped: dict[tuple[str, str, str, int], list[float]] = defaultdict(list)
    failed_cases = []
    for item in results:
        if item.get("stage") != 8:
            continue
        if item.get("status") != "PASS":
            failed_cases.append(item.get("case_id", "unknown"))
        throughput = item.get("throughput_mbps")
        if throughput is not None:
            grouped[key(item)].append(float(throughput))

    averages = {
        group_key: statistics.fmean(values)
        for group_key, values in grouped.items()
        if len(values) == len(ROUNDS)
    }
    checks: list[dict[str, Any]] = []

    for combo in COMBOS[1:]:
        for direction in DIRECTIONS:
            for network in NETWORKS:
                for size in SIZES:
                    candidate_key = (combo, direction, network, size)
                    baseline_key = ("mini-mini", direction, network, size)
                    candidate = averages.get(candidate_key)
                    baseline = averages.get(baseline_key)
                    ratio = (
                        candidate / baseline
                        if candidate is not None
                        and baseline is not None
                        and baseline > 0
                        else None
                    )
                    checks.append(
                        {
                            "check": "api_vs_baseline",
                            "combo": combo,
                            "direction": direction,
                            "network": network,
                            "size_bytes": size,
                            "value": ratio,
                            "threshold": args.minimum_api_ratio,
                            "pass": ratio is not None
                            and ratio >= args.minimum_api_ratio,
                        }
                    )

    for combo in COMBOS:
        for direction in DIRECTIONS:
            for size in SIZES:
                one = averages.get((combo, direction, "5G1", size))
                two = averages.get((combo, direction, "5G2", size))
                aggregate = averages.get((combo, direction, "Agg", size))
                efficiency = (
                    aggregate / (one + two)
                    if aggregate is not None
                    and one is not None
                    and two is not None
                    and one + two > 0
                    else None
                )
                checks.append(
                    {
                        "check": "aggregation",
                        "combo": combo,
                        "direction": direction,
                        "size_bytes": size,
                        "agg_mbps": aggregate,
                        "efficiency": efficiency,
                        "pass": (
                            aggregate is not None
                            and one is not None
                            and two is not None
                            and aggregate > max(one, two)
                            and efficiency is not None
                            and efficiency >= args.minimum_aggregation_efficiency
                            and (
                                args.minimum_agg_mbps is None
                                or aggregate >= args.minimum_agg_mbps
                            )
                        ),
                    }
                )

    summary = {
        "schema_version": 1,
        "stage": 8,
        "status": "PASS",
        "result_count": len(results),
        "expected_result_count": len(expected_cases),
        "missing_cases": missing,
        "duplicates": duplicates,
        "failed_cases": failed_cases,
        "averages": [
            {
                "combo": group_key[0],
                "direction": group_key[1],
                "network": group_key[2],
                "size_bytes": group_key[3],
                "average_mbps": value,
            }
            for group_key, value in sorted(averages.items())
        ],
        "checks": checks,
    }
    if (
        missing
        or duplicates
        or failed_cases
        or any(not check["pass"] for check in checks)
    ):
        summary["status"] = "FAIL"

    summary_path = args.results_dir / "summary.json"
    summary_path.write_text(
        json.dumps(summary, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    with (args.results_dir / "summary.csv").open(
        "w", newline="", encoding="utf-8"
    ) as stream:
        writer = csv.DictWriter(
            stream,
            fieldnames=(
                "combo",
                "direction",
                "network",
                "size_bytes",
                "average_mbps",
            ),
        )
        writer.writeheader()
        writer.writerows(summary["averages"])

    print(
        f"{summary['status']} results={len(results)}/{len(expected_cases)} "
        f"missing={len(missing)} failed={len(failed_cases)}"
    )
    return 0 if summary["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

