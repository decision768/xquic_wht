from __future__ import annotations

import csv
import json
import math
import re
import shutil
import statistics
from collections import defaultdict
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Tuple

import matplotlib.dates as mdates
import matplotlib.pyplot as plt
from matplotlib import font_manager


LOG_PATTERN = "*_summary_*.log"
SERIES_ORDER = ["5G1", "5G2", "Agg"]
MODE_ORDER = ["get", "post"]
COLORS = {"5G1": "#1f77b4", "5G2": "#ff7f0e", "Agg": "#2ca02c"}
OUTPUTS = {"get": "get_line_chart.png", "post": "post_line_chart.png"}
CPU_OUTPUT = "cpu_summary_chart.png"
CPU_CORE_COUNT = 2
RESOURCE_PATTERN = "resources/**/*_resource.csv"
SERVER_RESOURCE_PATTERN = "server_memory_*.csv"
CPU_METRICS = [
    ("client", "客户端 CPU（CPU 核数=2）"),
    ("server_process", "服务器进程 CPU（CPU 核数=2）"),
]
CPU_CONDITIONS = [
    ("get", "5G1", "下载 · 5G1"),
    ("get", "5G2", "下载 · 5G2"),
    ("get", "Agg", "下载 · Agg"),
    ("post", "5G1", "上传 · 5G1"),
    ("post", "5G2", "上传 · 5G2"),
    ("post", "Agg", "上传 · Agg"),
]
RUN_TAG_PATTERN = re.compile(
    r"_r\d+_(?P<category>5G1|5G2|Agg)_(?P<mode>get|post)_"
)

# 新增指标对比配置；留空时仍读取本脚本旁的 log 目录。
# 多次实验并存时，指定某一次，例如 "log/20261024_120000_abcd1234"。
METRIC_INPUT_DIR = ""
# 不同位置的算法结果可在这里指定；相对路径均基于本脚本目录。
# 第一项作为CSV相对差异的基准；图中仍展示各算法的实际指标值。
# ALGORITHM_DIRS = {"adaptive": "log/实验ID/adaptive", "minrtt": "log/实验ID/minrtt"}
ALGORITHM_DIRS: Dict[str, str] = {}
# 按需删减指标。GET、POST分别出图，不合并两种吞吐口径。
COMPARISON_METRICS = (
    "agg_mean_mbps", "aggregation_efficiency_pct", "gain_best_single_pct", "agg_cv_pct",
)
METRIC_DEFINITIONS = {
    "agg_mean_mbps": ("聚合吞吐均值 ↑", "Mbps", "agg_cycles"),
    "aggregation_efficiency_pct": ("聚合率 ↑", "%", "paired_cycles"),
    "gain_best_single_pct": ("相对最佳单链路增益 ↑", "%", "paired_cycles"),
    "agg_cv_pct": ("跨周期吞吐波动 CV ↓", "%", "agg_cycles"),
}


def parse_log_file(path: Path) -> Tuple[str, str, datetime, datetime, float] | None:
    parts = path.stem.split("_")
    if len(parts) < 4:
        return None

    category = parts[0]
    mode = parts[2].lower()
    if category not in SERIES_ORDER or mode not in MODE_ORDER:
        return None

    with path.open("r", encoding="utf-8", newline="") as file:
        reader = csv.DictReader(file)
        rows = list(reader)

    if not rows:
        return None

    average_row = next((row for row in rows if row.get("iter") == "AVERAGE"), rows[-1])
    start_time = (average_row.get("start_time") or "").strip()
    end_time = (average_row.get("end_time") or "").strip()
    mbps = (average_row.get("mbps") or "").strip()

    if not start_time or not end_time or not mbps:
        return None

    start_timestamp = datetime.strptime(start_time, "%Y-%m-%d %H:%M:%S")
    end_timestamp = datetime.strptime(end_time, "%Y-%m-%d %H:%M:%S")
    return category, mode, start_timestamp, end_timestamp, float(mbps)


def collect_series(
    base_dir: Path,
) -> Tuple[Dict[str, Dict[str, List[Tuple[datetime, float]]]], List[Tuple[Path, datetime, datetime]]]:
    data: Dict[str, Dict[str, List[Tuple[datetime, float]]]] = defaultdict(lambda: defaultdict(list))
    used_logs: List[Tuple[Path, datetime, datetime]] = []
    for path in base_dir.glob(LOG_PATTERN):
        parsed = parse_log_file(path)
        if parsed is None:
            continue
        category, mode, start_timestamp, end_timestamp, mbps = parsed
        data[mode][category].append((start_timestamp, mbps))
        used_logs.append((path, start_timestamp, end_timestamp))

    for mode in data:
        for category in data[mode]:
            data[mode][category].sort(key=lambda item: item[0])
    return data, used_logs


def normalize_process_cpu_pct(value: float) -> float:
    """Convert top-style process CPU into its share of effective CPU capacity."""
    return value / CPU_CORE_COUNT


def collect_cpu_series(
    base_dir: Path,
) -> Dict[str, Dict[str, Dict[str, List[float]]]]:
    data: Dict[str, Dict[str, Dict[str, List[float]]]] = defaultdict(
        lambda: defaultdict(lambda: defaultdict(list))
    )
    for path in base_dir.glob(RESOURCE_PATTERN):
        match = RUN_TAG_PATTERN.search(path.stem)
        if match is None:
            continue
        with path.open("r", encoding="utf-8-sig", newline="") as file:
            rows = list(csv.DictReader(file))

        values = []
        for row in rows:
            try:
                value = float((row.get("client_process_cpu_pct") or "").strip())
            except ValueError:
                continue
            if math.isfinite(value) and value >= 0.0:
                values.append(normalize_process_cpu_pct(value))

        if values:
            data[match.group("mode")][match.group("category")]["client"].append(
                sum(values) / len(values)
            )

    server_runs: Dict[str, Dict[str, object]] = {}
    for path in base_dir.glob(SERVER_RESOURCE_PATTERN):
        with path.open("r", encoding="utf-8-sig", newline="") as file:
            rows = csv.DictReader(file)
            for row in rows:
                run_tag = (row.get("run_tag") or "").strip()
                match = RUN_TAG_PATTERN.search(run_tag)
                if match is None:
                    continue
                run = server_runs.setdefault(
                    run_tag,
                    {
                        "mode": match.group("mode"),
                        "category": match.group("category"),
                        "server_process": [],
                    },
                )
                try:
                    value = float((row.get("server_process_cpu_pct") or "").strip())
                except ValueError:
                    continue
                if math.isfinite(value) and value >= 0.0:
                    run["server_process"].append(normalize_process_cpu_pct(value))

    for run in server_runs.values():
        for metric in ("server_process",):
            values = run[metric]
            if values:
                data[run["mode"]][run["category"]][metric].append(
                    sum(values) / len(values)
                )
    return data


def percentile(values: List[float], fraction: float) -> float:
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower_index = math.floor(position)
    upper_index = math.ceil(position)
    if lower_index == upper_index:
        return ordered[lower_index]
    weight = position - lower_index
    return ordered[lower_index] * (1.0 - weight) + ordered[upper_index] * weight


def configure_chinese_font() -> None:
    candidates = ("Microsoft YaHei", "SimHei", "Noto Sans CJK SC")
    installed = {font.name for font in font_manager.fontManager.ttflist}
    selected = next((font for font in candidates if font in installed), None)
    if selected:
        plt.rcParams["font.sans-serif"] = [selected]
    plt.rcParams["axes.unicode_minus"] = False


def plot_cpu_summary(
    base_dir: Path,
    series: Dict[str, Dict[str, Dict[str, List[float]]]],
) -> None:
    configure_chinese_font()
    figure, axes = plt.subplots(1, len(CPU_METRICS), figsize=(12, 7))
    positions = list(range(1, len(CPU_CONDITIONS) + 1))

    for axis, (metric, title) in zip(axes, CPU_METRICS):
        largest_value = 0.0
        for position, (mode, category, _) in zip(positions, CPU_CONDITIONS):
            values = series.get(mode, {}).get(category, {}).get(metric, [])
            if not values:
                continue
            first_quartile = percentile(values, 0.25)
            median = percentile(values, 0.5)
            third_quartile = percentile(values, 0.75)
            largest_value = max(largest_value, third_quartile)
            axis.vlines(
                position,
                first_quartile,
                third_quartile,
                color=COLORS[category],
                linewidth=8,
                alpha=0.72,
            )
            axis.scatter(
                position,
                median,
                s=70,
                color=COLORS[category],
                edgecolor="white",
                linewidth=1.2,
                zorder=3,
            )
            axis.text(position, third_quartile + 0.3, f"{median:.1f}%", ha="center")

        axis.set_xticks(
            positions,
            [condition[2] for condition in CPU_CONDITIONS],
            rotation=25,
            ha="right",
        )
        axis.set_xlim(0.45, len(CPU_CONDITIONS) + 0.55)
        axis.set_title(title)
        axis.set_ylabel("CPU 使用率（%）")
        axis.set_ylim(0.0, max(5.0, math.ceil((largest_value + 2.0) / 2.0) * 2.0))
        axis.grid(True, axis="y", linestyle="--", alpha=0.35)
        axis.set_axisbelow(True)

    figure.suptitle("CPU 平均占用分布", fontsize=15)
    figure.tight_layout(rect=(0.0, 0.0, 1.0, 0.94))
    figure.savefig(base_dir / CPU_OUTPUT, dpi=200)
    plt.close(figure)


def create_output_dir(base_dir: Path, used_logs: List[Tuple[Path, datetime, datetime]]) -> Path | None:
    if not used_logs:
        return None

    start_time = min(item[1] for item in used_logs)
    end_time = max(item[2] for item in used_logs)
    dirname = f"{start_time:%Y%m%d}_{start_time:%H%M%S}-{end_time:%H%M%S}"
    output_dir = base_dir / dirname
    suffix = 1
    while output_dir.exists():
        output_dir = base_dir / f"{dirname}_{suffix:02d}"
        suffix += 1

    output_dir.mkdir()
    return output_dir


def copy_used_logs(
    used_logs: List[Tuple[Path, datetime, datetime]],
    output_dir: Path,
    input_dir: Path,
) -> None:
    log_dir = output_dir / "log"
    log_dir.mkdir()
    for path, _, _ in used_logs:
        shutil.copy2(path, log_dir / path.name)

    for pattern in ("stress_summary_*.csv", "server_memory_*.csv"):
        for path in input_dir.glob(pattern):
            shutil.copy2(path, log_dir / path.name)

    resource_dir = input_dir / "resources"
    if resource_dir.is_dir():
        shutil.copytree(
            resource_dir,
            log_dir / "resources",
            dirs_exist_ok=True,
        )


def calculate_average_aggregation_rate(series: Dict[str, List[Tuple[datetime, float]]]) -> float | None:
    agg_series = series.get("Agg", [])
    single_1 = series.get("5G1", [])
    single_2 = series.get("5G2", [])
    if not agg_series or not single_1 or not single_2:
        return None

    agg_average = sum(value for _, value in agg_series) / len(agg_series)
    single_1_average = sum(value for _, value in single_1) / len(single_1)
    single_2_average = sum(value for _, value in single_2) / len(single_2)
    single_sum_average = single_1_average + single_2_average

    if single_sum_average <= 0:
        return None

    return agg_average / single_sum_average


def plot_mode(
    base_dir: Path,
    mode: str,
    series: Dict[str, List[Tuple[datetime, float]]],
    average_rate: float | None,
) -> None:
    plt.figure(figsize=(15, 7))

    for category in SERIES_ORDER:
        points = series.get(category, [])
        if not points:
            continue
        xs = [point[0] for point in points]
        ys = [point[1] for point in points]
        plt.plot(xs, ys, marker="o", markersize=3, linewidth=1.8, label=category, color=COLORS[category])

    plt.title(f"{mode.upper()} Throughput Trend")
    plt.xlabel("Start Time")
    plt.ylabel("Throughput (Mbps)")
    plt.grid(True, linestyle="--", alpha=0.35)
    plt.legend()

    if average_rate is not None:
        plt.text(
            0.98,
            0.95,
            f"Average aggregation rate: {average_rate * 100:.2f}%",
            transform=plt.gca().transAxes,
            ha="right",
            va="top",
            fontsize=11,
            bbox={"boxstyle": "round", "facecolor": "white", "alpha": 0.85, "edgecolor": "#999999"},
        )

    axis = plt.gca()
    axis.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    axis.xaxis.set_major_locator(mdates.AutoDateLocator(minticks=8, maxticks=16))
    plt.xticks(rotation=30, ha="right")
    plt.tight_layout()
    plt.savefig(base_dir / OUTPUTS[mode], dpi=200)
    plt.close()


def resolve_algorithm_dirs(base_dir: Path, input_dir: Path) -> Dict[str, Path]:
    """只识别一个实验中的直接算法目录，避免混入预热和多次实验。"""
    if ALGORITHM_DIRS:
        directories = {}
        for label, value in ALGORITHM_DIRS.items():
            if not label or label in (".", "..") or any(c in label for c in '/\\:'):
                raise ValueError(f"算法名称不能含路径字符：{label!r}")
            path = Path(value).expanduser()
            path = (path if path.is_absolute() else base_dir / path).resolve()
            if not path.is_dir() or not any(path.glob(LOG_PATTERN)):
                raise ValueError(f"算法 {label} 的目录不存在或没有速率日志：{path}")
            if path in directories.values():
                raise ValueError("不能把同一日志目录重复标为不同算法")
            directories[label] = path
        return directories
    if not input_dir.is_dir():
        return {}

    def profiles(root):
        return {path.name: path for path in sorted(root.iterdir())
                if path.is_dir() and path.name != "warmup" and any(path.glob(LOG_PATTERN))}

    direct = profiles(input_dir)
    if any(input_dir.glob(LOG_PATTERN)):
        if direct:
            raise ValueError("输入目录同时含平铺日志和算法目录，请用 METRIC_INPUT_DIR 或 ALGORITHM_DIRS 明确选择")
        return {}  # 原来的平铺日志流程原样保留。
    if direct:
        return direct
    experiments = [(path, profiles(path)) for path in sorted(input_dir.iterdir()) if path.is_dir()]
    experiments = [(path, items) for path, items in experiments if items]
    if len(experiments) > 1:
        raise ValueError("检测到多次实验，请设置 METRIC_INPUT_DIR 选择其中一次：\n"
                         + "\n".join(str(path) for path, _ in experiments))
    return experiments[0][1] if experiments else {}


def collect_metric_samples(base_dir: Path):
    """按方向/链路/周期取成功速率；AVERAGE不重复计数，重复周期不猜配。"""
    samples = defaultdict(lambda: defaultdict(dict))
    seen, duplicates = set(), set()
    pattern = re.compile(r"^(5G1|5G2|Agg)_summary_(get|post)_iter(\d+)(?:_|$)")
    for path in sorted(base_dir.glob(LOG_PATTERN)):
        match = pattern.match(path.stem)
        if match is None:
            continue
        category, mode, filename_cycle = match.groups()
        with path.open(encoding="utf-8-sig", newline="") as handle:
            rows = list(csv.DictReader(handle))
        numeric_rows = [row for row in rows if (row.get("iter") or "").strip().isdigit()]
        selected = numeric_rows or [row for row in rows if (row.get("iter") or "").strip() == "AVERAGE"]
        for row in selected:
            cycle_text = (row.get("iter") or "").strip()
            cycle = int(cycle_text if cycle_text.isdigit() else filename_cycle)
            try:
                mbps = float(row.get("mbps") or "")
            except ValueError:
                continue
            if cycle < 1 or not math.isfinite(mbps) or mbps < 0:
                continue
            key = (mode, category, cycle)
            if key in seen:
                duplicates.add(key)
                samples[mode][category].pop(cycle, None)
                continue
            seen.add(key)
            samples[mode][category][cycle] = mbps
    return samples, duplicates


def calculate_comparison_metrics(datasets, mode: str) -> List[dict]:
    """算法间对齐共同周期；两个比值逐周期计算，再取平均，缺失值留空。"""
    common_agg, common_pairs = None, None
    for samples, _ in datasets.values():
        links = samples.get(mode, {})
        agg_ids = set(links.get("Agg", {}))
        pair_ids = agg_ids & set(links.get("5G1", {})) & set(links.get("5G2", {}))
        pair_ids = {cycle for cycle in pair_ids
                    if links["5G1"][cycle] + links["5G2"][cycle] > 0}
        common_agg = agg_ids if common_agg is None else common_agg & agg_ids
        common_pairs = pair_ids if common_pairs is None else common_pairs & pair_ids
    agg_ids, pair_ids = sorted(common_agg or []), sorted(common_pairs or [])
    rows = []
    for label, (samples, duplicates) in datasets.items():
        links = samples.get(mode, {})
        values = [links["Agg"][cycle] for cycle in agg_ids]
        mean = statistics.mean(values) if values else None
        efficiencies, gains = [], []
        for cycle in pair_ids:
            agg, first, second = (links[category][cycle] for category in ("Agg", "5G1", "5G2"))
            efficiencies.append(agg / (first + second) * 100)
            gains.append((agg / max(first, second) - 1) * 100)
        rows.append({
            "algorithm": label, "direction": mode,
            "available_agg_cycles": len(links.get("Agg", {})),
            "agg_cycles": len(values), "paired_cycles": len(pair_ids),
            "duplicate_cycle_entries": sum(key[0] == mode for key in duplicates),
            "agg_mean_mbps": mean,
            "aggregation_efficiency_pct": statistics.mean(efficiencies) if efficiencies else None,
            "gain_best_single_pct": statistics.mean(gains) if gains else None,
            "agg_cv_pct": statistics.stdev(values) / mean * 100 if len(values) >= 2 and mean > 0 else None,
        })
    if rows:
        reference = rows[0]
        for row in rows:
            row["reference_algorithm"] = reference["algorithm"]
            first, value = reference["agg_mean_mbps"], row["agg_mean_mbps"]
            row["agg_mean_vs_reference_pct"] = ((value / first - 1) * 100
                                                 if value is not None and first is not None and first > 0 else None)
            for metric, output in (("aggregation_efficiency_pct", "efficiency_vs_reference_pp"),
                                   ("gain_best_single_pct", "gain_vs_reference_pp"),
                                   ("agg_cv_pct", "cv_vs_reference_pp")):
                first, value = reference[metric], row[metric]
                row[output] = value - first if value is not None and first is not None else None
    return rows


def plot_algorithm_metrics(output_dir: Path, mode: str, rows: List[dict], scope_note: str = "") -> None:
    metrics = list(COMPARISON_METRICS)
    if not metrics or any(metric not in METRIC_DEFINITIONS for metric in metrics):
        raise ValueError("COMPARISON_METRICS须选择METRIC_DEFINITIONS中至少一个指标")
    configure_chinese_font()
    columns = min(2, len(metrics))
    figure, axes = plt.subplots(math.ceil(len(metrics) / columns), columns,
                                figsize=(6.2 * columns, 4.1 * math.ceil(len(metrics) / columns) + 1), squeeze=False)
    palette = plt.get_cmap("tab10")
    for axis, metric in zip(axes.flat, metrics):
        title, unit, count_key = METRIC_DEFINITIONS[metric]
        for index, row in enumerate(rows):
            value = row[metric]
            if value is None:
                axis.annotate("N/A", (index, 0), xytext=(0, 8), textcoords="offset points", ha="center")
                continue
            axis.bar(index, value, width=0.6, color=palette(index % 10), alpha=0.88, zorder=3)
            axis.annotate(f"{value:.2f}\nn={row[count_key]}", (index, value),
                          xytext=(0, 7 if value >= 0 else -7), textcoords="offset points",
                          ha="center", va="bottom" if value >= 0 else "top", fontsize=10)
        if metric == "aggregation_efficiency_pct":
            axis.axhline(100, color="#777777", linestyle="--", linewidth=1)
        elif metric == "gain_best_single_pct":
            axis.axhline(0, color="#777777", linestyle="--", linewidth=1)
        axis.set_xticks(range(len(rows)), [row["algorithm"] for row in rows])
        axis.set_title(title)
        axis.set_ylabel(unit)
        axis.grid(axis="y", linestyle="--", alpha=0.3)
        axis.set_axisbelow(True)
        axis.margins(y=0.3)
        axis.set_xlim(-0.6, len(rows) - 0.4)
    for axis in list(axes.flat)[len(metrics):]:
        axis.set_visible(False)
    figure.suptitle(f"{mode.upper()} 聚合效果指标对比（↑越大越好；↓越小越好）", fontsize=14)
    note = "仅使用算法间共同有效周期；聚合率/增益先逐周期计算再取均值。\nCV使用样本标准差，少于2个有效周期时显示N/A。"
    figure.text(0.5, 0.025, note + ("\n" + scope_note if scope_note else ""), ha="center", fontsize=9)
    figure.tight_layout(rect=(0, 0.12, 1, 0.94))
    figure.savefig(output_dir / f"{mode}_algorithm_metrics.png", dpi=200)
    plt.close(figure)


def run_algorithm_comparison(base_dir: Path, directories: Dict[str, Path]) -> None:
    collected = {label: collect_series(path) for label, path in directories.items()}
    used_logs = [log for _, logs in collected.values() for log in logs]
    output_dir = create_output_dir(base_dir, used_logs)
    if output_dir is None:
        print("No valid log files found")
        return
    # 原有折线图、CPU图及日志复制函数保持原样，分别应用于各算法。
    for label, path in directories.items():
        target = output_dir / label
        target.mkdir()
        data, logs = collected[label]
        copy_used_logs(logs, target, path)
        for mode in MODE_ORDER:
            if data.get(mode):
                plot_mode(target, mode, data[mode], calculate_average_aggregation_rate(data[mode]))
        cpu = collect_cpu_series(path)
        if cpu:
            plot_cpu_summary(target, cpu)
    datasets = {label: collect_metric_samples(path) for label, path in directories.items()}
    all_rows = []
    for mode in MODE_ORDER:
        if not any(samples.get(mode) for samples, _ in datasets.values()):
            continue
        rows = calculate_comparison_metrics(datasets, mode)
        scope_note = ""
        if mode == "get":
            for path in directories.values():
                config = path.parent / "experiment_config.json"
                if config.is_file():
                    with config.open(encoding="utf-8-sig") as handle:
                        scope = str(json.load(handle).get("scheduler_scope", ""))
                    if "client argument" in scope:
                        scope_note = "算法标签为客户端请求参数；GET发送端算法仍由现有服务端决定。"
        plot_algorithm_metrics(output_dir, mode, rows, scope_note)
        all_rows.extend(rows)
        print(f"{mode.upper()} 指标：共同Agg周期={rows[0]['agg_cycles']}，共同三链路周期={rows[0]['paired_cycles']}")
        for row in rows:
            if row["duplicate_cycle_entries"]:
                print(f"  {row['algorithm']}：排除{row['duplicate_cycle_entries']}个重复的方向/链路/周期项，请检查是否混入多次实验")
    if all_rows:
        with (output_dir / "algorithm_metrics.csv").open("w", encoding="utf-8-sig", newline="") as handle:
            writer = csv.DictWriter(handle, fieldnames=list(all_rows[0]))
            writer.writeheader()
            writer.writerows(all_rows)
    print(f"Output directory: {output_dir}")


def main() -> None:
    base_dir = Path(__file__).resolve().parent
    input_dir = Path(METRIC_INPUT_DIR).expanduser() if METRIC_INPUT_DIR else base_dir / "log"
    if not input_dir.is_absolute():
        input_dir = base_dir / input_dir
    try:
        directories = resolve_algorithm_dirs(base_dir, input_dir)
    except ValueError as exc:
        print(f"指标对比输入配置错误：{exc}")
        return
    if directories:
        run_algorithm_comparison(base_dir, directories)
        return
    data, used_logs = collect_series(input_dir)
    cpu_data = collect_cpu_series(input_dir)
    output_dir = create_output_dir(base_dir, used_logs)
    if output_dir is None:
        print("No valid log files found")
        return

    copy_used_logs(used_logs, output_dir, input_dir)
    for mode in MODE_ORDER:
        mode_series = data.get(mode, {})
        if not mode_series:
            print(f"{mode.upper()} aggregation rate: no valid matched data")
            continue

        average_rate = calculate_average_aggregation_rate(mode_series)
        plot_mode(output_dir, mode, mode_series, average_rate)
        if average_rate is None:
            print(f"{mode.upper()} aggregation rate: no valid matched data")
        else:
            print(f"{mode.upper()} average aggregation rate: {average_rate:.4f} ({average_rate * 100:.2f}%)")

    if cpu_data:
        plot_cpu_summary(output_dir, cpu_data)
        print(f"CPU chart generated: CPU cores={CPU_CORE_COUNT}")
    else:
        print("CPU chart: no valid resource data")

    print(f"Output directory: {output_dir}")


if __name__ == "__main__":
    main()
