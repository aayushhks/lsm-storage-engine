#!/usr/bin/env python3
"""Render the benchmark results JSON into charts and a markdown table for the
README. Reads bench/results/results.json, writes ops_per_sec.png, latency.png,
and table.md alongside it.

The only third-party dependency is matplotlib (the allowed plotting exception);
the engine itself is standard-library only.

The read path and the write path differ by roughly two orders of magnitude, so
they get one linear panel each rather than a shared log axis: bar length encodes
magnitude from a zero baseline, which a log axis would quietly break. Error bars
span the min and max trial so run-to-run noise stays visible. Latency, where a
zero baseline carries no meaning, is drawn as a dot plot and may use a log axis.
"""

import json
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(HERE, "results", "results.json")

# Validated colorblind-safe pair: read path vs write path. Checked for CVD
# separation and contrast against the light chart surface used below.
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
MUTED = "#52514e"
GRID = "#e3e2df"
READ = "#2a78d6"
WRITE = "#eb6834"

READ_WORKLOADS = ("read-random-uniform", "read-random-zipfian")
PERCENTILES = ("p50_us", "p95_us", "p99_us", "p999_us")
PERCENTILE_LABELS = {"p50_us": "p50", "p95_us": "p95", "p99_us": "p99", "p999_us": "p99.9"}


def short(name):
    return name.replace("read-random-", "").replace("fill-", "fill ").replace("-50-50", " 50/50")


def style_axes(ax):
    ax.set_facecolor(SURFACE)
    ax.grid(axis="x", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "bottom", "left"):
        ax.spines[side].set_visible(False)
    ax.tick_params(colors=MUTED, length=0, labelsize=9)


def plot_throughput(results, out_path):
    reads = [r for r in results if r["workload"] in READ_WORKLOADS]
    writes = [r for r in results if r["workload"] not in READ_WORKLOADS]

    fig, axes = plt.subplots(
        2, 1, figsize=(8, 5.2), facecolor=SURFACE,
        gridspec_kw={"height_ratios": [len(reads), len(writes)], "hspace": 0.55},
    )

    for ax, group, color, title in (
        (axes[0], reads, READ, "read path — point lookups"),
        (axes[1], writes, WRITE, "write path — every write fsynced"),
    ):
        labels = [short(r["workload"]) for r in group]
        values = [r["ops_per_sec"] for r in group]
        # Error bars span the min and max trial, so the spread is visible.
        lower = [r["ops_per_sec"] - r.get("ops_per_sec_min", r["ops_per_sec"]) for r in group]
        upper = [r.get("ops_per_sec_max", r["ops_per_sec"]) - r["ops_per_sec"] for r in group]

        y = range(len(group))
        ax.barh(y, values, height=0.55, color=color, zorder=2)
        ax.errorbar(
            values, y, xerr=[lower, upper], fmt="none", ecolor=INK,
            elinewidth=1.2, capsize=3, zorder=3,
        )
        ax.set_yticks(list(y), labels)
        ax.invert_yaxis()
        ax.set_xlim(0, max(r.get("ops_per_sec_max", r["ops_per_sec"]) for r in group) * 1.32)
        ax.set_title(title, color=INK, fontsize=10.5, loc="left", pad=8)
        style_axes(ax)
        ax.xaxis.set_major_formatter(lambda v, _pos: f"{v / 1000:,.0f}k" if v else "0")
        # Anchor the label past the error bar cap, never on top of it.
        for i, (r, value) in enumerate(zip(group, values)):
            end = max(value, r.get("ops_per_sec_max", value))
            ax.text(
                end + ax.get_xlim()[1] * 0.025, i, f"{value:,.0f}",
                va="center", ha="left", color=INK, fontsize=9.5,
            )

    axes[1].set_xlabel("operations / second  (median of trials; bars span min–max)",
                       color=MUTED, fontsize=9)
    fig.savefig(out_path, dpi=170, bbox_inches="tight", facecolor=SURFACE)
    plt.close(fig)


def plot_latency(results, out_path):
    fig, ax = plt.subplots(figsize=(8, 4.0), facecolor=SURFACE)

    labels = [short(r["workload"]) for r in results]
    y = list(range(len(results)))
    all_values = []
    for i, r in enumerate(results):
        color = READ if r["workload"] in READ_WORKLOADS else WRITE
        xs = [r[p] for p in PERCENTILES if p in r]
        all_values.extend(xs)
        ax.plot(xs, [i] * len(xs), color=color, linewidth=2, alpha=0.35, zorder=2,
                solid_capstyle="round")
        ax.scatter(xs, [i] * len(xs), s=[34, 34, 34, 58][: len(xs)], color=color, zorder=3)
        # p50 sits above its dot and p99.9 to the right of the row, so neither
        # label can collide with a tick label or with the other end.
        ax.text(xs[0], i - 0.26, f"p50 {xs[0]:,.1f}", va="bottom", ha="center",
                color=MUTED, fontsize=8.5)
        ax.text(xs[-1] * 1.3, i, f"{xs[-1]:,.0f} µs", va="center", ha="left",
                color=INK, fontsize=8.5)

    ax.set_yticks(y, labels)
    ax.set_ylim(len(results) - 0.4, -0.85)
    ax.set_xscale("log")
    ax.set_xlim(min(all_values) * 0.55, max(all_values) * 3.2)
    ax.set_xlabel("latency (µs, log) — p50 · p95 · p99 · p99.9 left to right",
                  color=MUTED, fontsize=9)
    style_axes(ax)
    ax.grid(axis="x", color=GRID, linewidth=0.8)

    handles = [
        plt.Line2D([], [], color=READ, marker="o", linestyle="none", label="read path"),
        plt.Line2D([], [], color=WRITE, marker="o", linestyle="none", label="write path"),
    ]
    # Above the plot area, so it can never sit on top of a row.
    legend = ax.legend(handles=handles, frameon=False, ncol=2, fontsize=9,
                       loc="lower right", bbox_to_anchor=(1.0, 1.0))
    for text in legend.get_texts():
        text.set_color(MUTED)

    fig.savefig(out_path, dpi=170, bbox_inches="tight", facecolor=SURFACE)
    plt.close(fig)


def write_table(results, out_path):
    lines = [
        "| workload | ops/sec (median) | trials min–max | p50 (µs) | p95 (µs) | p99 (µs) | p99.9 (µs) |",
        "|---|---:|---:|---:|---:|---:|---:|",
    ]
    for r in results:
        spread = f"{r.get('ops_per_sec_min', 0):,.0f}–{r.get('ops_per_sec_max', 0):,.0f}"
        lines.append(
            f"| {r['workload']} | {r['ops_per_sec']:,.0f} | {spread} | "
            f"{r['p50_us']:,.2f} | {r['p95_us']:,.2f} | {r['p99_us']:,.2f} | "
            f"{r.get('p999_us', 0):,.2f} |"
        )
    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else RESULTS
    with open(path) as f:
        data = json.load(f)
    results = data["results"]
    out_dir = os.path.dirname(os.path.abspath(path))

    plot_throughput(results, os.path.join(out_dir, "ops_per_sec.png"))
    plot_latency(results, os.path.join(out_dir, "latency.png"))
    write_table(results, os.path.join(out_dir, "table.md"))
    print(f"wrote ops_per_sec.png, latency.png, table.md to {out_dir}")


if __name__ == "__main__":
    main()
