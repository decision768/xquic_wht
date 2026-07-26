#!/usr/bin/env python3

"""Static guardrails for the single installed NetIns UDP public API."""

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PUBLIC_HEADER = ROOT / "include/xquic/netins_udp.h"
LEGACY_HEADER = ROOT / "src/netins_udp/netins_udp_legacy.h"
CMAKE = ROOT / "CMakeLists.txt"
VERSION_SCRIPT = ROOT / "scripts/xquic.lds"
DELIVERY_FILES = (
    ROOT / "api/README.md",
    ROOT / "api/NETINS_UDP_API_使用说明.md",
    ROOT / "api/NETINS_UDP_API_设计说明.md",
    *sorted((ROOT / "api/examples").glob("*.c")),
)
IMPLEMENTATIONS = (
    ROOT / "src/netins_udp/netins_udp_api_version.c",
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
    print(f"NetIns UDP API guard: FAIL: {message}", file=sys.stderr)
    return False


def main():
    public = PUBLIC_HEADER.read_text(encoding="utf-8")
    legacy = LEGACY_HEADER.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    version_script = VERSION_SCRIPT.read_text(encoding="utf-8")
    implementations = "\n".join(
        path.read_text(encoding="utf-8") for path in IMPLEMENTATIONS
    )
    ok = True

    if not re.search(
        r"#define\s+NETINS_UDP_API_VERSION\s+2U\b", public
    ):
        ok = fail("the installed header does not declare API version 2")

    forbidden_public_tokens = (
        "NETINS_UDP_APP_RAW_STREAM",
        "NETINS_UDP_OPT_SOCKOPT",
        "netins_udp_client_t",
        "netins_udp_client_config_t",
        "netins_udp_stream_t",
        "netins_udp_stream_info_t",
        "netins_udp_observer_t",
        "NETINS_UDP_EVENT_STREAM_ACCEPTABLE",
        "netins_udp_client_create(",
        "netins_udp_observer_open(",
        "netins_udp_send(",
        "netins_udp_recv(",
        "netins_udp_stream_open(",
    )
    for token in forbidden_public_tokens:
        if token in public:
            ok = fail(f"installed header publishes internal token {token!r}")

    if not re.search(
        r"#define\s+NETINS_UDP_PUBLIC\s+"
        r"__attribute__\(\(visibility\(\"hidden\"\)\)\)",
        legacy,
    ):
        ok = fail("compatibility declarations are not explicitly hidden")

    obsolete_draft = ROOT / "api/draft/netins_udp_v2.h"
    if obsolete_draft.exists():
        ok = fail("the obsolete split public draft still exists")

    delivery_text = "\n".join(
        path.read_text(encoding="utf-8") for path in DELIVERY_FILES
    )
    for pattern, description in (
        (r"\bV1\b|\bV2\b", "split API version terminology"),
        (r"api/draft/netins_udp_v2\.h", "obsolete draft header"),
        (
            r"\bnetins_udp_(?:"
            r"client_(?:config_init|create|connect|wait_connected|"
            r"get_state|get_connection_id|get_snapshot|"
            r"get_last_error_code|get_last_error_msg|close)|"
            r"stream_[A-Za-z0-9_]+|observer_[A-Za-z0-9_]+"
            r")\s*\(",
            "compatibility API call",
        ),
    ):
        if re.search(pattern, delivery_text):
            ok = fail(f"delivery files contain {description}")

    public_symbols = public_functions(public)
    missing_implementations = {
        symbol
        for symbol in public_symbols
        if not re.search(
            rf"(?m)^\s*{re.escape(symbol)}\s*\([^;]*?\)\s*\{{",
            implementations,
            re.DOTALL,
        )
    }
    if missing_implementations:
        ok = fail(
            "public declarations without implementation: "
            + ", ".join(sorted(missing_implementations))
        )

    install_commands = re.findall(r"install\s*\((.*?)\)", cmake, re.DOTALL)
    installed = "\n".join(install_commands)
    if "include/xquic/netins_udp.h" not in installed:
        ok = fail("the final public header is not installed")
    if "netins_udp_legacy.h" in installed or "netins_udp_v2.h" in installed:
        ok = fail("an internal or draft header is included in install()")

    missing_exports = {
        symbol
        for symbol in public_symbols
        if not re.search(rf"\b{re.escape(symbol)}\s*;", version_script)
    }
    if missing_exports:
        ok = fail(
            "public declarations not exported: "
            + ", ".join(sorted(missing_exports))
        )

    exported_netins_symbols = set(
        re.findall(r"(?m)^\s*(netins_udp_[A-Za-z0-9_]+)\s*;",
                   version_script)
    )
    unexpected_exports = exported_netins_symbols - public_symbols
    if unexpected_exports:
        ok = fail(
            "version script exports non-public NetIns symbols: "
            + ", ".join(sorted(unexpected_exports))
        )

    legacy_symbols = public_functions(legacy) - {"netins_udp_get_api_version"}
    leaked_legacy_exports = {
        symbol
        for symbol in legacy_symbols
        if re.search(rf"\b{re.escape(symbol)}\s*;", version_script)
    }
    if leaked_legacy_exports:
        ok = fail(
            "internal compatibility symbols are exported: "
            + ", ".join(sorted(leaked_legacy_exports))
        )

    if not ok:
        return 1
    print(
        "NetIns UDP API guard: PASS "
        "(one installed header, version=2, HTTP/3-only, legacy symbols hidden)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
