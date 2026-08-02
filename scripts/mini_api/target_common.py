#!/usr/bin/env python3
"""Shared helpers for Mini API target-machine validation."""

from __future__ import annotations

import hashlib
import json
import re
import shutil
import subprocess
import time
from pathlib import Path
from typing import Any, Optional, Sequence


API_THROUGHPUT_RE = re.compile(r"throughput_mbps=([0-9]+(?:\.[0-9]+)?)")
MINI_THROUGHPUT_RE = re.compile(r"\bspeed=([0-9]+(?:\.[0-9]+)?)\s+Mbps")
API_PATH_COUNT_RE = re.compile(r"\bpaths=([0-9]+)\b")
MINI_PATH_RE = re.compile(r"\[diag_path\].*?\bid=([0-9]+)\b")
RX_BUDGET_RE = re.compile(
    r"\[diag_rx_budget\]\s+active_paths=([0-9]+)"
    r"\s+per_path_limit=([0-9]+)"
)

SINGLE_PATH_RX_LIMIT = 256
DUAL_PATH_RX_LIMIT = 256


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def parse_log(text: str, client_kind: str) -> dict[str, Any]:
    throughput_re = (
        API_THROUGHPUT_RE if client_kind == "api" else MINI_THROUGHPUT_RE
    )
    throughput_matches = throughput_re.findall(text)
    throughput = (
        float(throughput_matches[-1]) if throughput_matches else None
    )

    if client_kind == "api":
        path_matches = API_PATH_COUNT_RE.findall(text)
        path_count = int(path_matches[-1]) if path_matches else None
    else:
        path_ids = set(MINI_PATH_RE.findall(text))
        path_count = len(path_ids) if path_ids else None

    budgets = [
        {"active_paths": int(active), "per_path_limit": int(limit)}
        for active, limit in RX_BUDGET_RE.findall(text)
    ]
    return {
        "throughput_mbps": throughput,
        "observed_path_count": path_count,
        "rx_budgets": budgets,
    }


def expected_rx_limit(expected_paths: int) -> int:
    return (
        DUAL_PATH_RX_LIMIT
        if expected_paths >= 2
        else SINGLE_PATH_RX_LIMIT
    )


def rx_budget_matches(
    budgets: Sequence[dict[str, int]], expected_paths: int
) -> bool:
    limit = expected_rx_limit(expected_paths)
    return any(
        budget["active_paths"] == expected_paths
        and budget["per_path_limit"] == limit
        for budget in budgets
    )


def run_logged_command(
    command: Sequence[str],
    log_path: Path,
    timeout_seconds: float,
) -> dict[str, Any]:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    resource_path = log_path.with_suffix(log_path.suffix + ".resource")
    time_binary = shutil.which("/usr/bin/time")
    actual_command = list(command)
    if time_binary:
        actual_command = [
            time_binary,
            "-f",
            "max_rss_kb=%M user_cpu_s=%U system_cpu_s=%S",
            "-o",
            str(resource_path),
            "--",
            *actual_command,
        ]

    started = time.monotonic()
    timed_out = False
    return_code: Optional[int]
    with log_path.open("w", encoding="utf-8", errors="replace") as log:
        try:
            completed = subprocess.run(
                actual_command,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=timeout_seconds if timeout_seconds > 0 else None,
                check=False,
            )
            return_code = completed.returncode
        except subprocess.TimeoutExpired:
            timed_out = True
            return_code = None
    elapsed = time.monotonic() - started

    resource: dict[str, Any] = {}
    if resource_path.exists():
        resource_text = resource_path.read_text(
            encoding="utf-8", errors="replace"
        )
        for key, value in re.findall(
            r"(max_rss_kb|user_cpu_s|system_cpu_s)=([0-9.]+)",
            resource_text,
        ):
            resource[key] = int(value) if key == "max_rss_kb" else float(value)

    return {
        "return_code": return_code,
        "timed_out": timed_out,
        "wall_seconds": elapsed,
        "resource": resource,
    }


def atomic_write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(value, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )
    temporary.replace(path)
