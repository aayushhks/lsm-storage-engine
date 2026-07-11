#!/usr/bin/env python3
"""Render the benchmark results JSON into charts and a markdown table for the
README. Reads bench/results/results.json, writes ops_per_sec.png, latency.png,
and table.md alongside it.

The only third-party dependency is matplotlib (the allowed plotting exception);
the engine itself is standard-library only.

Colors are the Okabe-Ito colorblind-safe palette; the wide dynamic range between
reads and writes is shown on a log scale (one axis per chart, never dual-axis).
"""

import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(HERE, "results", "results.json")

# Okabe-Ito, validated colorblind-safe.
INK = "#1a1a1a"
MUTED = "#666666"
GRID = "#dddddd"
BAR = "#0072B2"
SERIES = {"p50": "#0072B2", "p95": "#E69F00", "p99": "#009E73"}


def human_ops(value):
    if value >= 1e6:
        return f"{value / 1e6:.2f}M"
    if value >= 1e3:
        return f"{value / 1e3:.1f}k"
    return f"{value:.0f}"


def machine_caption(machine):
    return (
        f"{machine.get('cpu_model', '?')} · {machine.get('cores', '?')} cores · "
        f"{machine.get('os', '?')}"
    )


def style_axes(ax):
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    for spine in ("left", "bottom"):
        ax.spines[spine].set_color(MUTED)
    ax.tick_params(colors=INK)
    ax.yaxis.grid(True, color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)


def plot_ops(results, machine, out_path):
    names = [r["workload"] for r in results]
    ops = [r["ops_per_sec"] for r in results]

    fig, ax = plt.subplots(figsize=(9, 4.5))
    bars = ax.bar(names, ops, color=BAR, width=0.6, zorder=3)
    ax.set_yscale("log")
    ax.set_ylabel("operations / second (log scale)", color=INK)
    ax.set_title("Throughput by workload", color=INK, fontsize=13, fontweight="bold")
    for bar, value in zip(bars, ops):
        ax.annotate(
            human_ops(value),
            (bar.get_x() + bar.get_width() / 2, value),
            ha="center",
            va="bottom",
            fontsize=9,
            color=INK,
        )
    style_axes(ax)
    plt.xticks(rotation=20, ha="right")
    fig.text(0.5, 0.005, machine_caption(machine), ha="center", color=MUTED, fontsize=8)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(out_path, dpi=140)
    plt.close(fig)


def plot_latency(results, machine, out_path):
    names = [r["workload"] for r in results]
    xs = range(len(names))
    width = 0.26

    fig, ax = plt.subplots(figsize=(9, 4.5))
    for offset, key in ((-width, "p50"), (0.0, "p95"), (width, "p99")):
        values = [r[f"{key}_us"] for r in results]
        ax.bar(
            [x + offset for x in xs],
            values,
            width=width,
            label=key,
            color=SERIES[key],
            zorder=3,
        )
    ax.set_yscale("log")
    ax.set_ylabel("latency in microseconds (log scale)", color=INK)
    ax.set_title("Latency percentiles by workload", color=INK, fontsize=13, fontweight="bold")
    ax.set_xticks(list(xs))
    ax.set_xticklabels(names, rotation=20, ha="right")
    ax.legend(frameon=False, ncol=3, loc="upper center")
    style_axes(ax)
    fig.text(0.5, 0.005, machine_caption(machine), ha="center", color=MUTED, fontsize=8)
    fig.tight_layout(rect=(0, 0.04, 1, 1))
    fig.savefig(out_path, dpi=140)
    plt.close(fig)


def write_table(data, out_path):
    machine = data["machine"]
    config = data["config"]
    lines = []
    lines.append(f"Hardware: {machine_caption(machine)}, {machine.get('mem_total', '?')}.")
    lines.append(
        f"Config: {config['num_ops']} ops/workload, {config['num_keys']} keys, "
        f"{config['value_size']}-byte values, {config['key_size']}-byte keys, "
        f"flush at {config['flush_threshold_bytes']} bytes."
    )
    lines.append("")
    lines.append("| workload | ops/sec | p50 (us) | p95 (us) | p99 (us) |")
    lines.append("|---|---:|---:|---:|---:|")
    for r in data["results"]:
        lines.append(
            f"| {r['workload']} | {r['ops_per_sec']:,.0f} | {r['p50_us']:.2f} | "
            f"{r['p95_us']:.2f} | {r['p99_us']:.2f} |"
        )
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else RESULTS
    with open(path) as f:
        data = json.load(f)
    out_dir = os.path.dirname(path)
    plot_ops(data["results"], data["machine"], os.path.join(out_dir, "ops_per_sec.png"))
    plot_latency(data["results"], data["machine"], os.path.join(out_dir, "latency.png"))
    write_table(data, os.path.join(out_dir, "table.md"))
    print(f"wrote ops_per_sec.png, latency.png, table.md to {out_dir}")


if __name__ == "__main__":
    main()
