#!/usr/bin/env python3
"""Create deterministic files used by the Mini API baseline tests."""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path


MIB = 1024 * 1024
PATTERN = bytes(index % 251 for index in range(MIB))
BASELINE_FILES = (
    (
        "mini-api-200MiB.bin",
        200 * MIB,
        "db0e6cbbe829827e344133442cb0a34e30cc815880f6f6ffae06721ea703d4fd",
    ),
    (
        "mini-api-1GiB.bin",
        1024 * MIB,
        "e18e3f358b46eae9266ac36a5ff6347f6bf09711dff389597f237d5fe83111d8",
    ),
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(MIB):
            digest.update(chunk)
    return digest.hexdigest()


def write_exact_file(path: Path, size: int) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    remaining = size
    with temporary.open("wb") as target:
        while remaining:
            chunk = PATTERN if remaining >= len(PATTERN) else PATTERN[:remaining]
            target.write(chunk)
            remaining -= len(chunk)
        target.flush()
        os.fsync(target.fileno())
    os.replace(temporary, path)


def verify_file(path: Path, size: int, expected_sha256: str) -> None:
    actual_size = path.stat().st_size
    if actual_size != size:
        raise RuntimeError(
            f"{path}: size mismatch: expected {size}, got {actual_size}"
        )
    actual_sha256 = sha256_file(path)
    if actual_sha256 != expected_sha256:
        raise RuntimeError(
            f"{path}: SHA-256 mismatch: expected {expected_sha256}, "
            f"got {actual_sha256}"
        )
    print(f"{actual_sha256}  {path}  ({actual_size} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("build/mini-api-baseline"),
        help="directory for generated baseline files",
    )
    parser.add_argument(
        "--verify",
        action="store_true",
        help="verify existing files without creating them",
    )
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    for name, size, expected_sha256 in BASELINE_FILES:
        path = args.output_dir / name
        if not args.verify:
            write_exact_file(path, size)
        if not path.is_file():
            raise FileNotFoundError(path)
        verify_file(path, size, expected_sha256)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
