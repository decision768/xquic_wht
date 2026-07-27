#!/usr/bin/env python3

"""Static guardrails for the single installed NetIns UDP public API."""

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PUBLIC_HEADER = ROOT / "include/xquic/netins_udp.h"
XQUIC_PUBLIC_HEADERS = tuple(sorted((ROOT / "include/xquic").glob("*.h")))
LEGACY_HEADER = ROOT / "src/netins_udp/netins_udp_legacy.h"
CMAKE = ROOT / "CMakeLists.txt"
GENERIC_CMAKE = ROOT / "cmake/CMakeLists.txt"
VERSION_SCRIPT = ROOT / "scripts/xquic.lds"
XQC_FRAME = ROOT / "src/transport/xqc_frame.c"
XQC_CONN = ROOT / "src/transport/xqc_conn.c"
CLIENT_BACKEND = ROOT / "src/netins_udp/netins_udp_client.c"
SERVER_BACKEND = ROOT / "src/netins_udp/netins_udp_server.c"
OBSERVER_BACKEND = ROOT / "src/netins_udp/netins_udp_observer.c"
V2_CLIENT = ROOT / "src/netins_udp/netins_udp_v2_client.c"
V2_H3 = ROOT / "src/netins_udp/netins_udp_v2_h3.c"
XQC_SEND_CTL = ROOT / "src/transport/xqc_send_ctl.c"
XQC_STREAM = ROOT / "src/transport/xqc_stream.c"
MINI_CLIENT = ROOT / "mini/mini_client.c"
MINI_CLIENT_CB = ROOT / "mini/mini_client_cb.c"
MINI_CLIENT_HEADER = ROOT / "mini/mini_client.h"
MINI_SERVER = ROOT / "mini/mini_server_cb.c"
MINI_SERVER_HEADER = ROOT / "mini/mini_server.h"
OBSERVER_TEST = ROOT / "tests/netins_udp/test_netins_udp.c"
V2_CLIENT_TEST = ROOT / "tests/netins_udp/test_netins_udp_v2_client.c"
V2_H3_TEST = ROOT / "tests/netins_udp/test_netins_udp_v2_h3.c"
V2_LOOPBACK_TEST = ROOT / "tests/netins_udp/test_netins_udp_v2_loopback.c"
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


def xquic_public_symbols(headers):
    text = "\n".join(path.read_text(encoding="utf-8") for path in headers)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//.*", "", text)
    text = re.sub(
        r"(?m)^\s*#(?:define|undef)\s+XQC_EXPORT_PUBLIC_API.*$",
        "",
        text,
    )

    symbols = set()
    for match in re.finditer(
        r"XQC_EXPORT_PUBLIC_API\s+([^;]+);", text, re.DOTALL
    ):
        declaration_prefix = match.group(1).split("(", 1)[0]
        names = re.findall(
            r"\b(xqc_[A-Za-z0-9_]+)\b", declaration_prefix
        )
        if names:
            symbols.add(names[-1])
    return symbols


def fail(message):
    print(f"NetIns UDP API guard: FAIL: {message}", file=sys.stderr)
    return False


def dynamic_netins_symbols(library, readelf):
    result = subprocess.run(
        [readelf, "--wide", "--dyn-syms", str(library)],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            result.stderr.strip() or f"{readelf} exited with {result.returncode}"
        )

    symbols = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) < 8 or not fields[0].endswith(":"):
            continue
        bind, visibility, section, name = (
            fields[4],
            fields[5],
            fields[6],
            fields[7],
        )
        if (
            bind in {"GLOBAL", "WEAK"}
            and visibility in {"DEFAULT", "PROTECTED"}
            and section != "UND"
        ):
            name = name.split("@", 1)[0]
            if name.startswith("netins_udp_"):
                symbols.add(name)
    return symbols


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Validate the final NetIns UDP public API and optional ELF ABI"
    )
    parser.add_argument(
        "--library",
        type=Path,
        help="built libxquic.so whose dynamic NetIns symbols must match the header",
    )
    parser.add_argument(
        "--readelf",
        help="readelf executable used with --library (defaults to PATH lookup)",
    )
    args = parser.parse_args(argv)

    public = PUBLIC_HEADER.read_text(encoding="utf-8")
    legacy = LEGACY_HEADER.read_text(encoding="utf-8")
    cmake_files = {
        CMAKE: CMAKE.read_text(encoding="utf-8"),
        GENERIC_CMAKE: GENERIC_CMAKE.read_text(encoding="utf-8"),
    }
    version_script = VERSION_SCRIPT.read_text(encoding="utf-8")
    xqc_frame = XQC_FRAME.read_text(encoding="utf-8")
    xqc_conn = XQC_CONN.read_text(encoding="utf-8")
    client_backend = CLIENT_BACKEND.read_text(encoding="utf-8")
    server_backend = SERVER_BACKEND.read_text(encoding="utf-8")
    observer_backend = OBSERVER_BACKEND.read_text(encoding="utf-8")
    v2_client = V2_CLIENT.read_text(encoding="utf-8")
    v2_h3 = V2_H3.read_text(encoding="utf-8")
    xqc_send_ctl = XQC_SEND_CTL.read_text(encoding="utf-8")
    xqc_stream = XQC_STREAM.read_text(encoding="utf-8")
    mini_client = MINI_CLIENT.read_text(encoding="utf-8")
    mini_client_cb = MINI_CLIENT_CB.read_text(encoding="utf-8")
    mini_client_header = MINI_CLIENT_HEADER.read_text(encoding="utf-8")
    mini_server = MINI_SERVER.read_text(encoding="utf-8")
    mini_server_header = MINI_SERVER_HEADER.read_text(encoding="utf-8")
    observer_test = OBSERVER_TEST.read_text(encoding="utf-8")
    v2_client_test = V2_CLIENT_TEST.read_text(encoding="utf-8")
    v2_h3_test = V2_H3_TEST.read_text(encoding="utf-8")
    v2_loopback_test = V2_LOOPBACK_TEST.read_text(encoding="utf-8")
    implementations = "\n".join(
        path.read_text(encoding="utf-8") for path in IMPLEMENTATIONS
    )
    ok = True

    accept_race_pattern = re.compile(
        r"if\s*\(!server->accepting\)\s*\{"
        r"(?:(?!return\s+-1;).)*"
        r"netins_udp_backend_connection_closed\s*\(\s*"
        r"connection->observer\s*,\s*ECANCELED\s*\)"
        r"(?:(?!return\s+-1;).)*return\s+-1;",
        re.S,
    )
    if not accept_race_pattern.search(server_backend):
        ok = fail(
            "server listener-close admission race must unregister its "
            "already-bound observer"
        )
    refuse_pattern = re.compile(
        r"netins_udp_server_refuse_callback\s*\([^)]*\)"
        r"\s*\{(?:(?!^}).)*"
        r"netins_udp_backend_connection_closed\s*\(\s*"
        r"connection->observer\s*,\s*ECONNABORTED\s*\)",
        re.S | re.M,
    )
    if not refuse_pattern.search(server_backend):
        ok = fail(
            "server_refuse must detach its observer because no H3 close "
            "callback follows"
        )

    if not re.search(
        r"#define\s+NETINS_UDP_API_VERSION\s+2U\b", public
    ):
        ok = fail("the installed header does not declare API version 2")

    if not re.search(
        r"path_recv_effective_bytes\s*\+=\s*stream_frame->data_length",
        xqc_frame,
    ):
        ok = fail(
            "legacy xqc_h3_request_get_stats per-path receive accounting "
            "is not maintained"
        )

    public_request_calls = client_backend[
        client_backend.index("netins_udp_stream_send("):
        client_backend.index("netins_udp_client_config_init(")
    ]
    if re.search(
        r"netins_udp_client_t\s*\*client\s*=\s*stream->client\s*;"
        r"[\s\S]{0,256}?pthread_mutex_unlock\(&stream->lock\)\s*;"
        r"[\s\S]{0,128}?netins_udp_client_wake\(client\)",
        public_request_calls,
    ):
        ok = fail(
            "a child request can use stream->client after releasing the "
            "lifetime-pinning stream lock"
        )
    h3_write = client_backend[
        client_backend.index("netins_udp_stream_h3_write("):
        client_backend.index("netins_udp_stream_h3_closing(")
    ]
    if re.search(
        r"netins_udp_client_t\s*\*client\s*=\s*stream->client\s*;"
        r"[\s\S]{0,128}?pthread_mutex_unlock\(&stream->lock\)\s*;"
        r"[\s\S]{0,128}?netins_udp_client_wake\(client\)",
        h3_write,
    ):
        ok = fail(
            "H3 write notification uses its parent after releasing the "
            "stream lifetime lock"
        )

    start_async = client_backend[
        client_backend.index("netins_udp_client_start_async("):
        client_backend.index("netins_udp_client_set_close_timeout(")
    ]
    for forbidden_call in (
        "netins_udp_client_resolve_peer(",
        "netins_udp_client_create_socket(",
        "netins_udp_client_create_engine(",
        "netins_udp_client_start_connection(",
    ):
        if forbidden_call in start_async:
            ok = fail(
                "NONBLOCK connect performs startup work in the caller: "
                + forbidden_call
            )

    for required_pattern, description in (
        (
            r"netins_udp_stream_parse_decimal_u64\s*\(",
            "strict decimal Content-Length parsing",
        ),
        (
            r"netins_udp_stream_parse_status\s*\(",
            "strict HTTP status parsing",
        ),
        (
            r"bytes_received\s*!=\s*stream->response_content_length",
            "response Content-Length completion validation",
        ),
        (
            r"netins_udp_stream_response_body_forbidden_locked\s*\(",
            "no-body response validation",
        ),
    ):
        if not re.search(required_pattern, client_backend):
            ok = fail(f"client backend lacks {description}")
    send_headers = client_backend[
        client_backend.index("netins_udp_stream_send_headers("):
        client_backend.index("netins_udp_stream_drive_send(")
    ]
    if '":authority"' not in send_headers or re.search(
        r"netins_udp_stream_set_header\s*\([^;]*?\"host\"",
        send_headers,
        re.DOTALL,
    ):
        ok = fail(
            "standalone API client does not emit the HTTP/3 :authority "
            "pseudo-header"
        )
    if not re.search(
        r"if\s*\(\s*is_post\s*&&\s*stream->h3_extension\s*==\s*NULL\s*\)",
        send_headers,
    ):
        ok = fail(
            "public API POST requests inherit legacy x-stream-* headers"
        )
    if not all(
        token in client_backend
        for token in (
            "netins_udp_client_format_api_authority(",
            "client->config.server_port != 443",
            '"[%s]:%u"',
        )
    ):
        ok = fail(
            "empty public API authority does not preserve IPv6/non-default "
            "port semantics"
        )
    if re.search(r"\b(?:atoi|strtoull)\s*\(", client_backend):
        ok = fail("client response metadata uses permissive numeric parsing")
    if not all(
        token in client_backend
        for token in (
            "NETINS_UDP_CLIENT_SEND_QUEUE_BUDGET",
            "NETINS_UDP_CLIENT_RECV_QUEUE_BUDGET",
            "reserved_send_queue_bytes",
            "reserved_recv_queue_bytes",
            "budget_reserved",
        )
    ):
        ok = fail("client H3 queues lack a per-connection aggregate budget")
    if not re.search(
        r"netins_udp_client_has_pending_resolvers\s*\(\s*\)",
        v2_client,
    ) and not re.search(
        r"netins_udp_v2_transport_has_pending_resolvers\s*\(\s*\)",
        v2_client,
    ):
        ok = fail("final cleanup can succeed while a DNS resolver is alive")
    if not all(
        token in client_backend
        for token in (
            "g_netins_udp_abandoned_resolvers",
            "pthread_join(finished->thread",
            "netins_udp_resolver_task_destroy(finished)",
        )
    ):
        ok = fail(
            "aborted DNS tasks are not kept joinable through final cleanup"
        )
    if "pthread_detach(resolver_thread)" in client_backend:
        ok = fail(
            "an abandoned DNS thread can outlive the loaded library image"
        )
    if "netins_udp_v2_scalar_copy(" not in v2_client or re.search(
        r"\*\s*\(\s*const\s+(?:int|uint32_t)\s*\*\s*\)\s*value",
        v2_client,
    ):
        ok = fail(
            "set_opt directly dereferences a potentially unaligned void input"
        )

    if not re.search(
        r"netins_udp_payload_metrics_enabled[\s\S]{0,600}"
        r"ctl_app_payload_bytes_send_unique",
        xqc_send_ctl,
    ):
        ok = fail("send hot path does not gate private NetIns payload metrics")
    if not re.search(
        r"netins_udp_payload_metrics_enabled[\s\S]{0,300}"
        r"ctl_app_payload_bytes_recv_unique",
        xqc_stream,
    ):
        ok = fail("receive hot path does not gate private NetIns payload metrics")
    if not re.search(
        r"if\s*\(\s*conn->netins_udp_payload_metrics_enabled\s*\)"
        r"\s*\{[\s\S]{0,220}?stream_frame->recv_path_id",
        xqc_frame,
    ):
        ok = fail("stream receive attribution runs for the original mini path")
    if not re.search(
        r"netins_udp_payload_metrics_enabled[\s\S]{0,180}"
        r"ctl_reinjected_bytes_recv",
        xqc_conn,
    ):
        ok = fail("private reinjection accounting runs for the original mini path")
    for mini_path, mini_text in (
        (MINI_CLIENT, mini_client),
        (MINI_SERVER, mini_server),
    ):
        if not re.search(
            r"netins_udp_backend_connection_bind\s*\([\s\S]{0,220}?,\s*0\s*\)",
            mini_text,
        ):
            ok = fail(
                f"{mini_path.relative_to(ROOT)} enables standalone API "
                "payload accounting"
            )
    if "ev_netins_udp_observer_refresh" in "\n".join(
        (
            mini_client,
            mini_client_cb,
            mini_client_header,
            mini_server,
            mini_server_header,
        )
    ):
        ok = fail(
            "mini contains observer timer scaffolding even though observer "
            "refresh is callback-driven"
        )

    if "netins_udp_backend_connection_abandoned(" not in server_backend:
        ok = fail(
            "server defensive teardown can leave a bound observer pointing "
            "at a destroyed engine"
        )
    server_request_create = server_backend[
        server_backend.index("netins_udp_server_h3_request_create("):
        server_backend.index("netins_udp_server_h3_request_read(")
    ]
    if re.search(r"!\s*server->accepting", server_request_create):
        ok = fail(
            "closing a listener disables H3 requests on an already accepted "
            "connection"
        )
    if "netins_udp_observer_abandon(" not in observer_backend:
        ok = fail("observer has no engine-safe forced detach path")
    if "netins_udp_transport_payload_metrics_set_enabled(" not in observer_test:
        ok = fail("observer unit test does not mock the private metrics gate")

    if not re.search(
        r"out->stream_system_error\s*=\s*request->stream_system_error",
        server_backend,
    ):
        ok = fail("server H3 transport info drops the stable system error")
    if re.search(
        r"netins_udp_v2_h3_set_error_locked\s*\(\s*request\s*,\s*"
        r"(?:info|transport_info)\.stream_error",
        v2_h3,
    ):
        ok = fail("public H3 last_error exposes a raw XQUIC error number")
    if "g_transport_system_error = ECONNRESET" not in v2_h3_test:
        ok = fail("H3 unit test does not cover raw-to-system error isolation")
    if (
        'strcmp(name, "connection") == 0' not in v2_h3
        or 'strcmp(header->name, "te") == 0' not in v2_h3
        or '"te", "gzip"' not in v2_h3_test
    ):
        ok = fail(
            "public H3 custom headers permit connection-specific fields "
            "or an invalid TE value"
        )
    if "assert(netins_udp_get_last_error_code(client) == 0);" not in v2_client_test:
        ok = fail(
            "client unit test does not verify EINPROGRESS is cleared "
            "after asynchronous connect success"
        )
    if re.search(
        r"response\.status\s*=\s*(?:204|205|304)\s*;"
        r"[\s\S]{0,512}?netins_udp_h3_request_send\s*\(",
        v2_loopback_test,
    ):
        ok = fail(
            "the loopback test sends a body/FIN after a no-body response"
        )
    if not re.search(
        r"netins->state\s*==\s*NETINS_UDP_STATE_CLOSING"
        r"[\s\S]{0,220}?next_state\s*=\s*NETINS_UDP_STATE_CLOSING",
        v2_client,
    ):
        ok = fail(
            "a stale transport state sample can reopen a public handle "
            "after shutdown"
        )
    if (
        "state_thread_result.state == NETINS_UDP_STATE_CLOSING"
        not in v2_client_test
    ):
        ok = fail(
            "client unit test does not cover get_state/shutdown state races"
        )
    if (
        not re.search(
            r"netins_udp_v2_host_text_valid\s*\(\s*tls(?:->|\.)server_name",
            v2_client,
        )
        or '"bad\\rname"' not in v2_client_test
    ):
        ok = fail(
            "TLS server_name can inject control characters into the "
            "HTTP/3 authority fallback"
        )
    request_open = v2_h3[
        v2_h3.index("netins_udp_h3_request_open("):
        v2_h3.index("netins_udp_h3_request_accept(")
    ]
    set_response = v2_h3[
        v2_h3.index("netins_udp_h3_request_set_response("):
        v2_h3.index("netins_udp_h3_request_send(")
    ]
    if "config->status" in request_open:
        ok = fail("response-only status handling leaked into request_open")
    if not re.search(
        r"config->status\s*==\s*204[\s\S]{0,260}"
        r"content_length_unknown\s*=\s*1",
        set_response,
    ):
        ok = fail("204 responses can publish a forbidden Content-Length")
    if "this HTTP response must not contain a message body" not in v2_h3:
        ok = fail("public H3 send permits a body on no-body responses")
    if "g_server_response_config.content_length_unknown == 1" not in v2_h3_test:
        ok = fail("H3 unit test does not cover 204 wire normalization")
    if not re.search(
        r"out->api_version\s*!=\s*NETINS_UDP_API_VERSION[\s\S]{0,320}"
        r"out->struct_size\s*<\s*sizeof\(\*out\)",
        v2_h3,
    ):
        ok = fail(
            "H3 request info output does not follow the public copy-out ABI"
        )

    refresh_state = v2_client[
        v2_client.index("netins_udp_v2_refresh_state("):
        v2_client.index("netins_udp_v2_validate_output(")
    ]
    if refresh_state.count("netins_udp_get_state(") != 1:
        ok = fail(
            "public status APIs recursively enter get_state during close"
        )
    for error_query, next_function in (
        (
            "netins_udp_get_last_error_code(",
            "netins_udp_get_last_error_msg(",
        ),
        (
            "netins_udp_get_last_error_msg(",
            "netins_udp_v2_connection_acquire_h3(",
        ),
    ):
        query_start = v2_client.index(error_query)
        query_end = v2_client.index(next_function, query_start)
        if "netins_udp_v2_refresh_state(netins)" not in v2_client[
            query_start:query_end
        ]:
            ok = fail(
                f"{error_query[:-1]} does not synchronize asynchronous "
                "transport errors"
            )

    nonblock_completion = v2_client[
        v2_client.index("if (nonblock) {"):
        v2_client.index(
            "if (netins_udp_v2_transport_client_wait_connected",
            v2_client.index("if (nonblock) {"),
        )
    ]
    if re.search(
        r"transport_state\s*==\s*NETINS_UDP_STATE_",
        nonblock_completion,
    ):
        ok = fail(
            "nonblocking completion compares private transport state against "
            "public enum constants"
        )

    snapshot_builder = v2_client[
        v2_client.index("netins_udp_v2_build_snapshot("):
        v2_client.index("netins_udp_is_multipath(")
    ]
    if "transport_present && !have_transport_snapshot" not in snapshot_builder:
        ok = fail(
            "status copy-out rejects setup failures that have no transport"
        )

    protected_copy_outs = (
        ("netins_udp_get_path_count(", "netins_udp_get_snapshot("),
        ("netins_udp_get_snapshot(", "netins_udp_get_info("),
        ("netins_udp_get_info(", "netins_udp_get_statis("),
        ("netins_udp_get_statis(", "netins_udp_get_path_statis("),
        ("netins_udp_get_path_statis(", "netins_udp_get_close_reason("),
        ("netins_udp_get_close_reason(", "netins_udp_get_fd("),
        ("netins_udp_get_events(", "netins_udp_get_last_error_code("),
    )
    for function, next_function in protected_copy_outs:
        start = v2_client.index(function)
        end = v2_client.index(next_function, start)
        body = v2_client[start:end]
        begin = body.find("netins_udp_v2_begin_call(netins)")
        null_check = body.find("== NULL")
        if begin < 0 or null_check < 0 or begin > null_check:
            ok = fail(
                f"{function[:-1]} validates output before acquiring its "
                "close-safe active-call reference"
            )

    for backend_name, backend_text, helper_name in (
        ("client", client_backend, "netins_udp_client_make_nonblocking"),
        ("server", server_backend, "netins_udp_server_make_nonblocking"),
    ):
        helper_start = backend_text.index(helper_name + "(")
        helper_end = backend_text.index("\n}", helper_start)
        helper = backend_text[helper_start:helper_end]
        if "F_SETFD" not in helper or "FD_CLOEXEC" not in helper:
            ok = fail(
                f"{backend_name} private descriptors are inherited across exec"
            )

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
    if "创建时快照" not in delivery_text:
        ok = fail("delivery documentation omits runtime option snapshot semantics")
    for pattern, description in (
        (r"\bV1\b|\bV2\b", "split API version terminology"),
        (r"api/draft/netins_udp_v2\.h", "obsolete draft header"),
        (r"SSL_TYPE=openssl", "unsupported SSL_TYPE build option"),
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
    public_xquic_symbols = xquic_public_symbols(XQUIC_PUBLIC_HEADERS)
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

    for cmake_path, cmake in cmake_files.items():
        install_commands = re.findall(
            r"install\s*\((.*?)\)", cmake, re.DOTALL
        )
        installed = "\n".join(install_commands)
        if "include/xquic/netins_udp.h" not in installed:
            ok = fail(
                "the final public header is not installed by "
                f"{cmake_path.relative_to(ROOT)}"
            )
        if "netins_udp_legacy.h" in installed or "netins_udp_v2.h" in installed:
            ok = fail(
                "an internal or draft header is installed by "
                f"{cmake_path.relative_to(ROOT)}"
            )
        if not re.search(
            r"set_property\s*\(\s*TARGET\s+xquic(?!-)\s+"
            r"APPEND_STRING\s+PROPERTY\s+LINK_FLAGS\s+"
            r"\"[^\"]*--version-script=.*?scripts/xquic\.lds[^\"]*\"\s*\)",
            cmake,
        ):
            ok = fail(
                f"the shared xquic target does not consume the export map in "
                f"{cmake_path.relative_to(ROOT)}"
            )
        if "find_package(Python3" in cmake:
            ok = fail(
                f"{cmake_path.relative_to(ROOT)} uses FindPython3 despite "
                "declaring compatibility with CMake 3.6"
            )
        for test_target in (
            "netins_udp_v2_client_unit_test",
            "netins_udp_v2_h3_unit_test",
            "netins_udp_v2_loopback_test",
        ):
            include_directories = re.search(
                rf"target_include_directories\s*\(\s*"
                rf"{re.escape(test_target)}\s+PRIVATE\b(.*?)\)",
                cmake,
                re.DOTALL,
            )
            if (
                include_directories is None
                or "${CMAKE_CURRENT_SOURCE_DIR}/include"
                not in include_directories.group(1)
            ):
                ok = fail(
                    f"{test_target} cannot resolve the installed-style "
                    f"xquic/netins_udp.h include in "
                    f"{cmake_path.relative_to(ROOT)}"
                )

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

    exported_xquic_symbols = set(
        re.findall(
            r"(?m)^\s*(xqc_[A-Za-z0-9_]+)\s*;", version_script
        )
    )
    missing_xquic_exports = public_xquic_symbols - exported_xquic_symbols
    if missing_xquic_exports:
        ok = fail(
            "the export map would hide public XQUIC ABI symbols: "
            + ", ".join(sorted(missing_xquic_exports))
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

    if args.library is not None:
        library = args.library.resolve()
        if not library.is_file():
            ok = fail(f"built shared library does not exist: {library}")
        else:
            readelf = args.readelf or shutil.which("readelf")
            if not readelf:
                ok = fail("readelf is required for compiled ABI verification")
            else:
                try:
                    actual_symbols = dynamic_netins_symbols(library, readelf)
                except (OSError, RuntimeError) as error:
                    ok = fail(f"could not inspect {library}: {error}")
                else:
                    missing_dynamic = public_symbols - actual_symbols
                    unexpected_dynamic = actual_symbols - public_symbols
                    if missing_dynamic:
                        ok = fail(
                            "built library is missing public NetIns symbols: "
                            + ", ".join(sorted(missing_dynamic))
                        )
                    if unexpected_dynamic:
                        ok = fail(
                            "built library exports private NetIns symbols: "
                            + ", ".join(sorted(unexpected_dynamic))
                        )

    if not ok:
        return 1
    scope = (
        "source and compiled ELF ABI"
        if args.library is not None
        else "source ABI contract"
    )
    print(f"NetIns UDP API guard: PASS ({scope})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
