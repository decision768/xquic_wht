#!/usr/bin/env python3
"""Check NetIns Mini API header, version script and optional library exports."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Set


FUNCTION_RE = re.compile(
    r"NETINS_UDP_PUBLIC\s+[^;]*?\b(netins_udp_[A-Za-z0-9_]+)\s*\(",
    re.DOTALL,
)
LDS_FUNCTION_RE = re.compile(r"^\s*(netins_udp_[A-Za-z0-9_]+)\s*;", re.MULTILINE)
NM_FUNCTION_RE = re.compile(r"\b(netins_udp_[A-Za-z0-9_]+)(?:@@?\S+)?$")


def read_functions(path: Path, pattern: re.Pattern[str]) -> Set[str]:
    return set(pattern.findall(path.read_text(encoding="utf-8")))


def library_functions(path: Path) -> Set[str]:
    nm = shutil.which("nm")
    if nm is None:
        raise RuntimeError("nm is required when --library is used")
    command = [nm, "-g", "--defined-only", str(path)]
    if path.suffix == ".so" or ".so." in path.name:
        command.insert(1, "-D")
    completed = subprocess.run(
        command, capture_output=True, text=True, check=False
    )
    if completed.returncode != 0:
        raise RuntimeError(completed.stderr.strip() or "nm failed")
    functions = set()
    for line in completed.stdout.splitlines():
        match = NM_FUNCTION_RE.search(line)
        if match:
            functions.add(match.group(1))
    return functions


def print_set(label: str, values: Iterable[str]) -> None:
    values = sorted(values)
    if values:
        print(f"{label}:")
        for value in values:
            print(f"  {value}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--header", type=Path, default=Path("include/xquic/netins_udp.h")
    )
    parser.add_argument(
        "--version-script", type=Path, default=Path("scripts/xquic.lds")
    )
    parser.add_argument("--library", type=Path)
    args = parser.parse_args()

    header = read_functions(args.header, FUNCTION_RE)
    version_script = read_functions(args.version_script, LDS_FUNCTION_RE)
    missing_from_script = header - version_script
    stale_in_script = version_script - header
    failed = bool(missing_from_script or stale_in_script)
    print_set("missing from version script", missing_from_script)
    print_set("not declared by public header", stale_in_script)

    if args.library is not None:
        exported = library_functions(args.library)
        missing_from_library = header - exported
        print_set("missing from library", missing_from_library)
        failed = failed or bool(missing_from_library)
        print(
            f"library exports: required={len(header)} "
            f"present={len(header - missing_from_library)}"
        )

    if failed:
        print("ABI CHECK: FAIL")
        return 1
    print(f"ABI CHECK: PASS ({len(header)} public functions)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
