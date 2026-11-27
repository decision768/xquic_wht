#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""GET 压测：聚合传1GiB，单链路基线传100MiB，每个QUIC连接只下载一份文件。

聚合文件大小为1GiB = 1024MiB = 1,073,741,824字节。
云端预先创建聚合源文件：/root/test_xquic_server/upload/1-1GiB
云端预先创建单链路源文件：/root/test_xquic_server/upload/1-100MiB
板端程序目录：/oemdata/gx/xquic_file/
板端接收文件：/userdata/get-<本次用例的唯一名称>.bin

复用同目录“压测_100MB_FCT.py”的SSH、算法轮换、FCT统计和清理流程；
旧脚本仍默认100MiB/每连接三次，本入口固定GET/每连接一次，不生成云端源文件。
底层原有的文件分片与并发方式保留；“一次”指一份完整文件，不是一个分片。
5G1/5G2单链路发送端统一使用original_minrtt。
单次速率 = 该模式文件字节数 * 8 / GET_FCT_us（Mbps），跨轮取各次速率的算术平均。
聚合率仍按聚合Mbps / (5G1基线Mbps + 5G2基线Mbps)计算；不同大小文件的FCT不直接比较。
下载文件每次尝试后清理，支持中断后的会话清理，避免累积1GiB文件。

先在控制机检查计划（不连接SSH）：
  python 压测_1GiB_GET_FCT.py --iterations 1 --dry-run
实际测试一轮：
  python 压测_1GiB_GET_FCT.py --iterations 1
仅选一个聚合调度器：
  python 压测_1GiB_GET_FCT.py --scheduler original_minrtt --iterations 1
adaptive-v2默认启用enforce模式：
  python 压测_1GiB_GET_FCT.py --scheduler adaptive-v2 --iterations 1
需要只观测、不执行等待策略时，显式切换shadow：
  python 压测_1GiB_GET_FCT.py --scheduler adaptive-v2 --adaptive-arrival-mode shadow --iterations 1

依赖和辅助模块沿用同目录原脚本；实际运行依赖见 requirements-telemetry.txt。
"""

from __future__ import annotations

import importlib.util
import json
import posixpath
import re
import sys
from pathlib import Path


# 用户常用配置区。命令行参数覆盖这些默认值。
DEFAULT_BOARD_SSH_HOST = "192.168.11.1"
DEFAULT_SERVER_SSH_HOST = "124.70.219.194"
DEFAULT_QUIC_HOST = "124.70.219.194"
DEFAULT_QUIC_PORT = 18446
DEFAULT_BOARD_DIR = "/oemdata/gx/xquic_file"
DEFAULT_CLIENT_EXECUTABLE = "./netins_udp_mini_api_client"
DEFAULT_DOWNLOAD_DIR = "/userdata"
DEFAULT_BOARD_RESOURCE_DIR = "/userdata/gx_resource_monitor"
DEFAULT_SERVER_DIR = "/root/test_xquic_server"
DEFAULT_SERVER_EXECUTABLE = "./netins_udp_mini_api_server"
DEFAULT_REMOTE_PATH = "/upload/1-1GiB"
DEFAULT_SINGLE_LINK_REMOTE_PATH = "/upload/1-100MiB"
DEFAULT_OUTPUT_ROOT = str(Path(__file__).resolve().parent / "log_1GB_GET")

# 沿用现有脚本的算法列表和500轮设置；试跑用 --iterations 1。
# minrtt 等其他已注册算法可用 --scheduler/--schedulers 选择。
DEFAULT_SCHEDULERS = ("adaptive", "adaptive-v2", "original_minrtt", "blest", "ecf")
DEFAULT_ITERATIONS = 500
DEFAULT_CC = "bbr"
DEFAULT_CLIENT_TIMEOUT_MS = 600000
DEFAULT_SERVE_TIMEOUT_MS = 600000
DEFAULT_COMMAND_TIMEOUT_S = 690.0
# SSH密码继续读取 MINI_BENCH_BOARD_PASSWORD / MINI_BENCH_SERVER_PASSWORD，或传入原有SSH选项。

FILE_MIB = 1024
FILE_BYTES = FILE_MIB * 1024 * 1024
SINGLE_LINK_FILE_BYTES = 100 * 1024 * 1024
FILES_PER_CONNECTION = 1
DESCRIPTION = (
    "GET压测：聚合每连接传一份1GiB文件，5G1/5G2单链路每连接传一份100MiB文件；"
    "默认程序在/oemdata/gx/xquic_file，文件下载到/userdata。仅GET，固定repeat=1。"
)


def _load_engine():
    """Use a private module instance so the 100MB entry point keeps its defaults."""
    directory = Path(__file__).resolve().parent
    source = directory / "压测_100MB_FCT.py"
    if not source.is_file():
        raise RuntimeError(f"缺少共用压测引擎: {source}；请保留同目录原脚本及辅助模块")
    if str(directory) not in sys.path:
        sys.path.insert(0, str(directory))
    name = "xquic_one_gib_get_engine"
    spec = importlib.util.spec_from_file_location(name, source)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


engine = _load_engine()
_parse_base_args = engine.parse_gx_args
_check_server_base = engine.server_preflight
_write_base_plan = engine.write_experiment_plan


def _apply_defaults():
    for name, value in globals().items():
        if name.startswith("DEFAULT_") and hasattr(engine, name):
            setattr(engine, name, value)
    engine.DEFAULT_RUN_GET = True
    engine.DEFAULT_RUN_POST = False
    engine.DEFAULT_GET_REPEAT = FILES_PER_CONNECTION
    engine.DEFAULT_GET_MIB = FILE_MIB
    engine.DEFAULT_GET_BYTES = FILE_BYTES


def parse_args(argv=None):
    _apply_defaults()
    args = _parse_base_args(
        argv, get_only=True, description=DESCRIPTION,
        remote_path_help=(f"聚合{FILE_MIB}MiB文件URL；默认 {DEFAULT_REMOTE_PATH}，"
                          f"须恰好{FILE_BYTES}字节；不会自动生成或截取副本"),
        single_link_remote_path=DEFAULT_SINGLE_LINK_REMOTE_PATH,
    )
    # These values also appear in the reviewable experiment manifest.
    args.benchmark_profile = "GET_1GiB_aggregate_100MiB_single_link_once_per_connection"
    args.get_file_bytes_by_mode = {
        "agg": FILE_BYTES, "rmnet": SINGLE_LINK_FILE_BYTES, "usb": SINGLE_LINK_FILE_BYTES,
    }
    return args


def server_preflight(server, server_dir, server_executable, remote_path,
                     require_get_source=True, *, expected_bytes=FILE_BYTES):
    # Check the existing executable/TLS prerequisites without invoking the
    # 100MB script's source-copy generator, then check the mode-specific size.
    source, _ = _check_server_base(server, server_dir, server_executable,
                                   remote_path, require_get_source=False)
    q = engine.q
    result = server.run(
        f"test -f {q(source)} && test ! -L {q(source)} && test -r {q(source)}"
        f" && printf '__MINI_SOURCE_SIZE__=%s\\n' \"$(wc -c < {q(source)})\"",
        timeout_s=20,
    )
    match = re.search(r"__MINI_SOURCE_SIZE__=(\d+)", result.output)
    if result.exit_code or result.timed_out or result.truncated or not match:
        raise RuntimeError(
            f"GET源文件预检失败: {source}。请先创建可读的普通文件，要求{expected_bytes}字节；"
            "脚本不会生成、覆盖或裁剪源文件。\n" + result.diagnostic_output
        )
    size = int(match.group(1))
    if size != expected_bytes:
        raise RuntimeError(f"GET源文件大小不符: {source}，实际{size}字节，要求{expected_bytes}字节")
    engine.LOG.info("GET源文件: %s bytes=%s，每连接完整文件数=1", source, size)
    return source, size


def write_experiment_plan(root, session, args, plan):
    _write_base_plan(root, session, args, plan)
    path = Path(root) / "experiment_config.json"
    config = json.loads(path.read_text(encoding="utf-8"))
    config.update(
        benchmark_profile=args.benchmark_profile,
        script_entrypoint=Path(__file__).name,
        file_bytes=None,
        aggregate_file_bytes=FILE_BYTES,
        single_link_file_bytes=SINGLE_LINK_FILE_BYTES,
        file_bytes_by_mode=args.get_file_bytes_by_mode,
        file_size_label="aggregate: 1GiB (1024MiB); single_link: 100MiB",
        files_per_connection=FILES_PER_CONNECTION,
        remote_path=args.remote_path,
        server_source_path=posixpath.join(args.server_dir, args.remote_path.lstrip("/")),
        single_link_remote_path=args.single_link_remote_path,
        single_link_server_source_path=posixpath.join(args.server_dir, args.single_link_remote_path.lstrip("/")),
        measured_planned_get_bytes=sum(c.file_bytes for c in plan if c.phase == "measured"),
        aggregation_comparison="per_case_file_bytes_over_fct_then_rate_ratio",
        source_policy="existing_regular_file_exact_size_no_generation",
        board_program_dir=args.board_dir,
        board_download_dir=args.download_dir,
        download_retention="delete_after_each_attempt",
        get_fct_case_statistic="single_file_per_connection",
        get_rate_case_statistic="single_file_fct_throughput",
        get_sha256_scope="single_file_per_connection",
    )
    path.write_text(json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8")


# Only this private engine instance gets the mixed-size GET profile.
engine.parse_gx_args = parse_args
engine.server_preflight = server_preflight
engine.write_experiment_plan = write_experiment_plan


def main(argv=None):
    return engine.gx_main(argv)


if __name__ == "__main__":
    sys.exit(main())
