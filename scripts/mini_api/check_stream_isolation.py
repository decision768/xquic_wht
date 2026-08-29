#!/usr/bin/env python3
"""Guard Mini stream API safety and connection-mode isolation contracts."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root", type=Path,
        default=Path(__file__).resolve().parents[2],
    )
    args = parser.parse_args()
    root = args.root.resolve()
    sources = {
        "client": root / "mini/mini_client.c",
        "server": root / "mini/mini_server.c",
        "client_backend": root / "mini/mini_client_backend.c",
        "client_cb": root / "mini/mini_client_cb.c",
        "server_backend": root / "mini/mini_server_backend.c",
        "stream_backend": root / "mini/mini_stream_backend.c",
        "stream_protocol": root / "mini/mini_stream_protocol.h",
        "file_client_demo": root / "demo/netins_udp_mini_api_client.c",
        "stream_client_demo": root / "demo/netins_udp_stream_client.c",
        "stream_server_demo": root / "demo/netins_udp_stream_server.c",
        "stream_acceptance": root / "tests/stream_api_acceptance.py",
        "api": root / "src/netins_udp/netins_udp_api.c",
        "public_header": root / "include/xquic/netins_udp.h",
    }
    try:
        text = {
            name: path.read_text(encoding="utf-8")
            for name, path in sources.items()
        }
    except OSError as error:
        print(f"STREAM ISOLATION CHECK: FAIL: {error}", file=sys.stderr)
        return 1

    checks = (
        (
            "normal client connections retain xqc_h3_connect",
            "client",
            r"if\s*\(\s*args->stream_poc\.enabled\s*\|\|\s*"
            r"args->stream_api_enabled\s*\)\s*\{.*?"
            r"xqc_h3_ext_connect\s*\(.*?\}\s*else\s*\{.*?"
            r"xqc_h3_connect\s*\(",
        ),
        (
            "client enables H3 extension only for stream mode",
            "client",
            r"egn_cfg\.enable_h3_ext\s*=\s*"
            r"args->stream_poc\.enabled\s*\|\|\s*"
            r"args->stream_api_enabled\s*;",
        ),
        (
            "server enables H3 extension only for stream mode",
            "server",
            r"egn_cfg\.enable_h3_ext\s*=\s*"
            r"args->stream_poc\.enabled\s*\|\|\s*"
            r"args->stream_api_enabled\s*;",
        ),
        (
            "file transfer rejects an active stream",
            "api",
            r"netins_udp_transfer\s*\(.*?client->active_stream\s*!=\s*NULL",
        ),
        (
            "file serving rejects an active stream",
            "api",
            r"netins_udp_serve\s*\(.*?connection->active_stream\s*!=\s*NULL",
        ),
        (
            "file transfer rejects every stream-mode connection",
            "api",
            r"netins_udp_transfer\s*\(.*?"
            r"client->backend_config\.stream_mode.*?EOPNOTSUPP",
        ),
        (
            "file serving rejects every stream-mode connection",
            "api",
            r"netins_udp_serve\s*\(.*?"
            r"connection->backend_config\.stream_mode.*?EOPNOTSUPP",
        ),
        (
            "stream creation requires explicit stream mode",
            "api",
            r"netins_udp_stream_create\s*\(.*?"
            r"!connection->backend_config\.stream_mode",
        ),
        (
            "stream open rejects an active file transfer",
            "client_backend",
            r"mini_backend_client_stream_open\s*\(.*?"
            r"client->connection->transfer_active",
        ),
        (
            "server stream ownership is per accepted connection",
            "server_backend",
            r"mini_backend_server_stream_accept\s*\(.*?"
            r"connection->stream_api_state\s*==\s*NULL.*?"
            r"external_event_driver\s*=\s*1",
        ),
        (
            "successful stream close does not send RESET_STREAM",
            "stream_backend",
            r"mini_stream_close_lanes_locked\s*\(.*?int\s+abort.*?"
            r"if\s*\(\s*abort\s*\).*?xqc_h3_ext_bytestream_close",
        ),
        (
            "application delivery callback runs outside the stream lock",
            "stream_backend",
            r"mini_stream_delivery_worker\s*\(.*?"
            r"pthread_mutex_unlock\(&stream->lock\).*?callback\(",
        ),
        (
            "public stream configuration has one fixed four-lane width",
            "public_header",
            r"NETINS_UDP_STREAM_LANE_COUNT\s+4U.*?"
            r"typedef\s+struct\s+netins_udp_stream_config_s\s*\{\s*"
            r"uint32_t\s+struct_size;\s*uint32_t\s+api_version;\s*"
            r".*?uint32_t\s+direction;\s*uint32_t\s+flags;",
        ),
        (
            "server buffers Bytestreams that arrive before stream accept",
            "server_backend",
            r"mini_backend_server_stream_bs_create\s*\(.*?"
            r"stream_api_pending_head.*?xqc_h3_ext_bytestream_set_user_data",
        ),
        (
            "pre-accept buffering is limited to one lane header",
            "server_backend",
            r"mini_backend_server_stream_bs_read\s*\(.*?"
            r"data_len\s*>\s*MINI_STREAM_LANE_HEADER_SIZE\s*-\s*"
            r"pending->data_len.*?\|\|\s*fin",
        ),
        (
            "sender waits for receiver credit before payload and FIN",
            "stream_backend",
            r"mini_stream_pump_sender_lane_locked\s*\(.*?"
            r"!stream->peer_credit_received\s*\|\|\s*"
            r"!chunk->credit_reserved.*?"
            r"!stream->peer_credit_received.*?protocol_fin_sent",
        ),
        (
            "opener finishes each lane header before payload",
            "stream_backend",
            r"mini_stream_pump_sender_lane_locked\s*\(.*?"
            r"stream->role\s*==\s*MINI_STREAM_ENDPOINT_OPEN.*?"
            r"lane->lane_header_sent\s*!=\s*"
            r"MINI_STREAM_LANE_HEADER_SIZE",
        ),
        (
            "sender reserves cumulative credit in global enqueue order",
            "stream_backend",
            r"mini_stream_reserve_send_credit_locked\s*\(.*?"
            r"next\s*=\s*stream->credit_head.*?"
            r"next->offset\s*!=\s*stream->send_credit_reserved.*?"
            r"stream->send_credit_reserved\s*\+=\s*"
            r"next->payload_length.*?next->credit_reserved\s*=\s*1.*?"
            r"stream->credit_head\s*=\s*next->credit_next",
        ),
        (
            "new records join the global credit queue in write order",
            "stream_backend",
            r"mini_backend_stream_write\s*\(.*?"
            r"stream->credit_tail->credit_next\s*=\s*chunk.*?"
            r"stream->credit_tail\s*=\s*chunk",
        ),
        (
            "global credit is reserved before per-lane submission",
            "stream_backend",
            r"mini_stream_process_locked\s*\(.*?"
            r"mini_stream_reserve_send_credit_locked\(stream\).*?"
            r"mini_stream_pump_sender_lane_locked",
        ),
        (
            "credit records carry record limits, not final payload digests",
            "stream_backend",
            r"\.digest\s*=\s*status\s*==\s*"
            r"MINI_STREAM_RECEIPT_CREDIT\s*\?\s*record_credit\s*:\s*"
            r"stream->digest"
            r".*?receipt.status\s*==\s*MINI_STREAM_RECEIPT_CREDIT.*?"
            r"receipt.digest\s*==\s*0.*?"
            r"stream->send_record_credit_limit\s*=\s*receipt.digest",
        ),
        (
            "receiver replenishes cumulative credit after delivery",
            "stream_backend",
            r"mini_stream_pump_receiver_credit_locked\s*\(.*?"
            r"stream->accepted_bytes\s*\+\s*stream->receive_queue_limit"
            r".*?mini_stream_delivery_worker\s*\(.*?callback\(.*?"
            r"stream->accepted_bytes\s*=.*?mini_stream_wake\(stream\)",
        ),
        (
            "tiny writes consume a minimum local queue charge",
            "stream_backend",
            r"chunk->queue_charge\s*=\s*data_len\s*<\s*"
            r"MINI_STREAM_RECORD_MEMORY_CHARGE.*?"
            r"stream->send_queue_limit\s*-\s*stream->send_queue_charge.*?"
            r"stream->send_queue_charge\s*\+=\s*chunk->queue_charge",
        ),
        (
            "receiver enforces cumulative record credit",
            "stream_backend",
            r"record_target\s*=\s*stream->records_delivered.*?"
            r"receive_record_capacity\s*=\s*stream->receive_queue_limit.*?"
            r"stream->receive_records_buffered\s*"
            r">=\s*stream->receive_record_capacity",
        ),
        (
            "stream wire protocol carries independent record credit",
            "stream_protocol",
            r"MINI_STREAM_PROTOCOL_VERSION\s+4U.*?"
            r"MINI_STREAM_RECORD_MEMORY_CHARGE\s+256U",
        ),
        (
            "public stream write initializes accepted before backend errors",
            "api",
            r"netins_udp_stream_write\s*\(.*?accepted\s*==\s*NULL.*?"
            r"\*accepted\s*=\s*0\s*;.*?mini_backend_stream_write",
        ),
        (
            "backend stream write initializes accepted before validation",
            "stream_backend",
            r"mini_backend_stream_write\s*\(.*?accepted\s*==\s*NULL.*?"
            r"\*accepted\s*=\s*0\s*;.*?data\s*==\s*NULL",
        ),
        (
            "stream sender rejects cumulative offset overflow",
            "stream_backend",
            r"next_send_offset\s*>\s*UINT64_MAX\s*-\s*count.*?"
            r"accepted_bytes\s*>\s*UINT64_MAX\s*-\s*count.*?EOVERFLOW",
        ),
        (
            "pending Bytestream cleanup does not touch a stream after close",
            "server_backend",
            r"xqc_h3_ext_bytestream_close\(pending->bs\).*?"
            r"mini_backend_server_pending_free\(pending->connection,\s*"
            r"pending,\s*0\s*\)",
        ),
        (
            "server records only complete headers present before accept",
            "server_backend",
            r"mini_backend_server_stream_accept\s*\(.*?"
            r"pending->data_len\s*==\s*MINI_STREAM_LANE_HEADER_SIZE.*?"
            r"preaccept_lane_count\+\+.*?"
            r"stream_config\.preaccept_lane_count\s*=\s*"
            r"preaccept_lane_count",
        ),
        (
            "delayed-accept acceptance requires all four preaccept headers",
            "stream_acceptance",
            r"case\.require_delayed_accept.*?"
            r"integer\(snapshot,\s*\"preaccept_lanes\"\)\s*"
            r"!=\s*FIXED_LANES",
        ),
        (
            "client state polling yields to the active stream worker",
            "client_backend",
            r"mini_backend_client_poll\s*\(.*?"
            r"client->active_stream\s*!=\s*NULL\s*\)\s*\{\s*"
            r"return\s+0\s*;.*?pthread_mutex_lock\(&client->engine_lock\)",
        ),
        (
            "client stream worker shares engine serialization with snapshots",
            "client_backend",
            r"mini_backend_client_stream_open\s*\(.*?"
            r"&client->engine_lock.*?mini_backend_client_get_snapshot\s*\("
            r".*?mini_backend_client_wake_stream\(client\).*?"
            r"pthread_mutex_lock\(&client->engine_lock\).*?"
            r"xqc_conn_get_stats",
        ),
        (
            "connection queries remain live while the stream worker drives I/O",
            "api",
            r"netins_udp_get_state\s*\(.*?"
            r"handle->active_stream\s*==\s*NULL\s*\)\s*\{\s*"
            r"poll_error\s*=\s*mini_backend_client_poll.*?"
            r"mini_backend_client_is_closed",
        ),
        (
            "client normalizes H3 no-error before notifying stream state",
            "client_cb",
            r"connection_error\s*=\s*h3_error\s*==\s*H3_NO_ERROR\s*"
            r"\?\s*0\s*:\s*h3_error\s*;.*?"
            r"mini_stream_backend_on_connection_close\(.*?connection_error",
        ),
        (
            "internal stream worker serializes event-base entry",
            "stream_backend",
            r"mini_stream_worker\s*\(.*?mini_stream_engine_acquire\(stream\)"
            r".*?event_base_loop\(stream->event_base,\s*EVLOOP_ONCE\).*?"
            r"mini_stream_engine_release\(stream\)",
        ),
        (
            "client stream worker installs a background logger scope",
            "stream_backend",
            r"mini_stream_worker\s*\(.*?"
            r"mini_stream_worker_log_scope_enter\(stream\).*?"
            r"event_base_loop\(stream->event_base,\s*EVLOOP_ONCE\).*?"
            r"mini_backend_log_scope_leave\(\)",
        ),
        (
            "server stream worker installs a background logger scope",
            "server_backend",
            r"mini_backend_server_io_worker\s*\(.*?"
            r"mini_backend_server_worker_log_scope_enter\(server\).*?"
            r"event_base_loop\(server->ctx\.eb,\s*EVLOOP_ONCE\).*?"
            r"mini_backend_log_scope_leave\(\)",
        ),
        (
            "live stream logger replacement waits for worker adoption",
            "stream_backend",
            r"mini_backend_stream_set_logger\s*\(.*?"
            r"target_generation\s*=\s*\+\+stream->logger_generation.*?"
            r"logger_active_generation\s*<\s*target_generation.*?"
            r"pthread_cond_wait",
        ),
        (
            "live server logger replacement waits for worker adoption",
            "server_backend",
            r"mini_backend_server_set_logger\s*\(.*?"
            r"target_generation\s*=\s*\+\+server->logger_generation.*?"
            r"logger_active_generation\s*<\s*target_generation.*?"
            r"pthread_cond_wait",
        ),
        (
            "accepted logger cannot replace the listener worker logger",
            "api",
            r"case\s+NETINS_UDP_OPT_LOGGER\s*:.*?"
            r"handle->role\s*==\s*NETINS_UDP_ROLE_SERVER.*?"
            r"mini_backend_server_set_logger",
        ),
        (
            "stream accepted handles do not retain listener logger user data",
            "api",
            r"accepted->backend_config\s*=\s*server->backend_config.*?"
            r"if\s*\(server->backend_config\.stream_mode\).*?"
            r"accepted->backend_config\.logger\s*=\s*NULL.*?"
            r"accepted->backend_config\.logger_user_data\s*=\s*NULL.*?"
            r"accepted->logger\s*=\s*NULL.*?"
            r"accepted->logger_user_data\s*=\s*NULL",
        ),
        (
            "file accepted handles retain their logger inheritance",
            "api",
            r"else\s*\{\s*/\*\s*Preserve the established file-API.*?"
            r"accepted->logger\s*=\s*server->logger.*?"
            r"accepted->logger_user_data\s*=\s*server->logger_user_data",
        ),
        (
            "listener close detaches its logger from a retained server worker",
            "api",
            r"handle->role\s*==\s*NETINS_UDP_ROLE_SERVER.*?"
            r"if\s*\(handle->backend_config\.stream_mode\).*?"
            r"mini_backend_server_set_logger\(handle->server_backend,\s*"
            r"NULL,\s*NULL,\s*NETINS_UDP_LOG_ERROR\).*?"
            r"mini_backend_server_release",
        ),
        (
            "server accept timeout reuses one bounded elapsed value",
            "server_backend",
            r"mini_backend_server_accept\s*\(.*?"
            r"xqc_usec_t\s+elapsed\s*=.*?elapsed\s*>=\s*timeout_us.*?"
            r"remaining_us\s*=\s*timeout_us\s*-\s*elapsed",
        ),
        (
            "server worker exit stops every external stream driver",
            "server_backend",
            r"io_worker_running\s*=\s*0;.*?"
            r"mini_stream_backend_external_driver_stopped",
        ),
        (
            "server connection statistics are serialized with engine drive",
            "server_backend",
            r"pthread_mutex_lock\(&server->engine_lock\);.*?"
            r"xqc_conn_get_stats.*?"
            r"pthread_mutex_unlock\(&server->engine_lock\);",
        ),
        (
            "server connection close publishes errors under the backend lock",
            "server_backend",
            r"mini_backend_server_on_connection_closed\s*\(.*?"
            r"pthread_mutex_lock\(&server->lock\);\s*"
            r"connection->last_error_code\s*=\s*error_code;",
        ),
        (
            "fault acceptance waits for delivered stream progress",
            "stream_acceptance",
            r"stream_api_ready.*?stream_api_progress.*?"
            r"if\s+not\s+established.*?stop_process\(server\)",
        ),
        (
            "stream server emits the fault-test progress marker after data",
            "stream_server_demo",
            r"receiver->received\s*=\s*offset\s*\+\s*data_len.*?"
            r"stream_api_progress.*?fflush\(stdout\)",
        ),
        (
            "stream client emits the GET fault-test progress marker after data",
            "stream_client_demo",
            r"receiver->received\s*=\s*offset\s*\+\s*data_len.*?"
            r"stream_api_progress.*?direction=get.*?fflush\(stdout\)",
        ),
        (
            "file demo defaults POST to balanced and normalizes GET adaptive",
            "file_client_demo",
            r"options->scheduler\s*=\s*NETINS_UDP_SCHEDULER_BALANCED.*?"
            r"options->direction\s*==\s*NETINS_UDP_TRANSFER_GET.*?"
            r"options->scheduler\s*=\s*NETINS_UDP_SCHEDULER_ADAPTIVE",
        ),
        (
            "stream demo defaults POST to balanced and GET to adaptive",
            "stream_client_demo",
            r"options->direction\s*=\s*"
            r"NETINS_UDP_STREAM_DIRECTION_CLIENT_TO_SERVER.*?"
            r"options->scheduler\s*=\s*NETINS_UDP_SCHEDULER_BALANCED.*?"
            r"NETINS_UDP_STREAM_DIRECTION_SERVER_TO_CLIENT.*?"
            r"options->scheduler\s*=\s*NETINS_UDP_SCHEDULER_ADAPTIVE",
        ),
        (
            "acceptance covers both half-duplex directions by default",
            "stream_acceptance",
            r"--direction.*?choices=\(\"both\",\s*\"post\",\s*\"get\"\)"
            r".*?default=\"both\".*?directions\s*=\s*\(\"post\",\s*\"get\"\)"
            r".*?case\.direction",
        ),
        (
            "release acceptance refuses a single transfer direction",
            "stream_acceptance",
            r"args\.release\s+and\s+args\.direction\s*!=\s*\"both\".*?"
            r"--release requires --direction both",
        ),
    )

    failures = []
    for label, source_name, pattern in checks:
        if re.search(pattern, text[source_name], re.DOTALL) is None:
            failures.append(label)
    if failures:
        for label in failures:
            print(f"missing invariant: {label}", file=sys.stderr)
        print("STREAM ISOLATION CHECK: FAIL", file=sys.stderr)
        return 1
    print(f"STREAM ISOLATION CHECK: PASS ({len(checks)} invariants)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
