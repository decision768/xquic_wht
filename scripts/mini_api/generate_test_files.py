#!/usr/bin/env python3
"""Generate deterministic Mini API payloads and a SHA-256 manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path


MIB = 1024 * 1024
DEFAULT_SIZES = {
    "empty.bin": 0,
    "one-byte.bin": 1,
    "boundary-4m.bin": 4 * MIB,
    "payload-200m.bin": 200 * MIB,
    "payload-1g.bin": 1024 * MIB,
    "payload-over-1g.bin": 1024 * MIB + 4 * MIB,
}


def deterministic_block(size: int = MIB) -> bytes:
    seed = hashlib.sha256(b"netins-mini-api-v5.4.2").digest()
    repeated = seed * ((size + len(seed) - 1) // len(seed))
    return repeated[:size]


def generate(path: Path, size: int, block: bytes) -> str:
    digest = hashlib.sha256()
    remaining = size
    with path.open("wb") as stream:
        while remaining > 0:
            chunk = block[: min(remaining, len(block))]
            stream.write(chunk)
            digest.update(chunk)
            remaining -= len(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output_dir", type=Path)
    parser.add_argument(
        "--quick",
        action="store_true",
        help="generate only 0, 1 and 4 MiB boundary files",
    )
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    sizes = dict(list(DEFAULT_SIZES.items())[:3]) if args.quick else DEFAULT_SIZES
    block = deterministic_block()
    manifest = {"format_version": 1, "files": []}
    for name, size in sizes.items():
        path = args.output_dir / name
        sha256 = generate(path, size, block)
        manifest["files"].append(
            {"name": name, "size_bytes": size, "sha256": sha256}
        )
        print(f"{sha256}  {path}")

    manifest_path = args.output_dir / "sha256-manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
