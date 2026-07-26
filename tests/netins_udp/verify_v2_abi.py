#!/usr/bin/env python3

"""Static guardrails for the non-installed NetIns UDP V2 draft."""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
V1_HEADER = ROOT / "include/xquic/netins_udp.h"
V2_HEADER = ROOT / "api/draft/netins_udp_v2.h"
CMAKE = ROOT / "CMakeLists.txt"
VERSION_SCRIPT = ROOT / "scripts/xquic.lds"
V2_IMPLEMENTATIONS = (
    ROOT / "src/netins_udp/netins_udp_v2_client.c",
    ROOT / "src/netins_udp/netins_udp_v2_h3.c",
)


def public_functions(text):
    pattern = re.compile(
        r"NETINS_UDP_PUBLIC\s+[^;]*?\b(netins_udp_[A-Za-z0-9_]+)\s*\(",
        re.DOTALL,
    )
    return set(pattern.findall(text))


def fail(message):
    print(f"V2 ABI guard: FAIL: {message}", file=sys.stderr)
    return False


def main():
    v1 = V1_HEADER.read_text(encoding="utf-8")
    v2 = V2_HEADER.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    version_script = VERSION_SCRIPT.read_text(encoding="utf-8")
    implementations = "\n".join(
        path.read_text(encoding="utf-8") for path in V2_IMPLEMENTATIONS
    )
    ok = True

    if not re.search(r"#define\s+NETINS_UDP_API_VERSION\s+1U\b", v1):
        ok = fail("the installed V1 header no longer declares API version 1")

    forbidden_v2_tokens = (
        "NETINS_UDP_APP_RAW_STREAM",
        "netins_udp_stream_t",
        "netins_udp_stream_info_t",
        "NETINS_UDP_EVENT_STREAM_ACCEPTABLE",
        "netins_udp_send(",
        "netins_udp_recv(",
        "netins_udp_stream_open(",
    )
    for token in forbidden_v2_tokens:
        if token in v2:
            ok = fail(f"V2 draft still publishes incompatible token {token!r}")

    overlap = public_functions(v1) & public_functions(v2)
    if overlap:
        ok = fail(
            "V1/V2 public declarations overlap: " + ", ".join(sorted(overlap))
        )

    v2_functions = public_functions(v2)
    missing_implementations = {
        symbol
        for symbol in v2_functions
        if not re.search(
            rf"(?m)^[A-Za-z_][A-Za-z0-9_ \t*]*\n"
            rf"{re.escape(symbol)}\s*\([^;]*?\)\s*\{{",
            implementations,
            re.DOTALL,
        )
    }
    if missing_implementations:
        ok = fail(
            "V2 declarations without implementation: "
            + ", ".join(sorted(missing_implementations))
        )

    install_commands = re.findall(r"install\s*\((.*?)\)", cmake, re.DOTALL)
    if any("netins_udp_v2.h" in command for command in install_commands):
        ok = fail("the V2 draft is included in an install() command")

    required_v2_exports = {
        "netins_udp_is_multipath",
        "netins_udp_get_path_count",
        "netins_udp_get_info",
        "netins_udp_get_statis",
        "netins_udp_get_path_statis",
        "netins_udp_get_snapshot",
        "netins_udp_get_close_reason",
        "netins_udp_get_fd",
        "netins_udp_get_events",
        "netins_udp_h3_request_get_fd",
        "netins_udp_h3_request_get_events",
    }
    missing = {
        symbol
        for symbol in required_v2_exports
        if not re.search(rf"\b{re.escape(symbol)}\s*;", version_script)
    }
    if missing:
        ok = fail("version script misses: " + ", ".join(sorted(missing)))

    undeclared_exports = {
        symbol
        for symbol in v2_functions
        if not re.search(rf"\b{re.escape(symbol)}\s*;", version_script)
    }
    if undeclared_exports:
        ok = fail(
            "V2 declarations not exported: "
            + ", ".join(sorted(undeclared_exports))
        )

    forbidden_exports = {"netins_udp_send", "netins_udp_recv"}
    present = {
        symbol
        for symbol in forbidden_exports
        if re.search(rf"\b{re.escape(symbol)}\s*;", version_script)
    }
    if present:
        ok = fail(
            "HTTP/3-only V2 still exports: " + ", ".join(sorted(present))
        )

    if not ok:
        return 1
    print(
        "V2 ABI guard: PASS "
        "(V1 version=1, V2 draft uninstalled, no public-name overlap, H3-only)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
