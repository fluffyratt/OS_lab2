#!/usr/bin/env python3
"""
Visualization script for IPC benchmark results.

Input:
    results/results.csv

Output:
    results/plots/latency_vs_size.png
    results/plots/throughput_vs_size.png
    results/plots/latency_64B.png
    results/plots/throughput_1MiB.png
    results/summary_median.csv

Usage:
    python3 visualize.py
    python3 visualize.py path/to/results.csv
"""

import sys
from pathlib import Path

import pandas as pd
import matplotlib.pyplot as plt


METHOD_LABELS = {
    "mmap-anon-shared": "mmap anon + semaphore",
    "mmap-anon-spin": "mmap anon + spin",
    "mmap-file-shared": "mmap file + semaphore",
    "mmap-file-spin": "mmap file + spin",
    "posix-shm": "POSIX SHM",
    "file-pread-pwrite": "file pread/pwrite",
    "pipe": "pipe",
    "fifo": "FIFO",
    "unix-socket": "Unix socket",
    "posix-mq": "POSIX MQ",
}

SIZE_ORDER = [8, 64, 1024, 4096, 65536, 1048576]
SIZE_LABELS = {
    8: "8 B",
    64: "64 B",
    1024: "1 KiB",
    4096: "4 KiB",
    65536: "64 KiB",
    1048576: "1 MiB",
}


def load_results(path: Path) -> pd.DataFrame:
    if not path.exists():
        raise SystemExit(f"CSV file not found: {path}")

    df = pd.read_csv(path)

    required = {
        "run", "method", "mode", "message_size",
        "iterations", "elapsed_s", "value_name", "value"
    }

    missing = required - set(df.columns)
    if missing:
        raise SystemExit(
            "CSV has an unexpected format. Missing columns: "
            + ", ".join(sorted(missing))
        )

    return df


def build_summary(df: pd.DataFrame) -> pd.DataFrame:
    return (
        df.groupby(["mode", "method", "message_size"], as_index=False)
          .agg(
              median=("value", "median"),
              minimum=("value", "min"),
              maximum=("value", "max"),
              mean=("value", "mean"),
              std=("value", "std"),
              repeats=("value", "count"),
          )
    )


def ordered_methods(summary: pd.DataFrame):
    existing = set(summary["method"].unique())
    result = [m for m in METHOD_LABELS if m in existing]
    result += sorted(existing - set(result))
    return result


def plot_latency(summary: pd.DataFrame, out: Path):
    data = summary[summary["mode"] == "latency"]
    methods = ordered_methods(data)

    fig, ax = plt.subplots(figsize=(11, 6.5))

    for method in methods:
        part = data[data["method"] == method].set_index("message_size")
        xs, ys = [], []

        for size in SIZE_ORDER:
            if size in part.index:
                xs.append(size)
                ys.append(float(part.loc[size, "median"]) / 1000.0)  # ns -> us

        if xs:
            ax.plot(
                xs,
                ys,
                marker="o",
                linewidth=1.7,
                label=METHOD_LABELS.get(method, method),
            )

    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks(SIZE_ORDER)
    ax.set_xticklabels([SIZE_LABELS[s] for s in SIZE_ORDER])
    ax.set_xlabel("Message size")
    ax.set_ylabel("Median latency, μs")
    ax.set_title("IPC latency vs message size")
    ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
    ax.legend(fontsize=8, ncol=2)
    fig.tight_layout()
    fig.savefig(out, dpi=180)
    plt.close(fig)


def plot_throughput(summary: pd.DataFrame, out: Path):
    data = summary[summary["mode"] == "throughput"]
    methods = ordered_methods(data)

    fig, ax = plt.subplots(figsize=(11, 6.5))

    for method in methods:
        part = data[data["method"] == method].set_index("message_size")
        xs, ys = [], []

        for size in SIZE_ORDER:
            if size in part.index:
                xs.append(size)
                ys.append(float(part.loc[size, "median"]))

        if xs:
            ax.plot(
                xs,
                ys,
                marker="o",
                linewidth=1.7,
                label=METHOD_LABELS.get(method, method),
            )

    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xticks(SIZE_ORDER)
    ax.set_xticklabels([SIZE_LABELS[s] for s in SIZE_ORDER])
    ax.set_xlabel("Message size")
    ax.set_ylabel("Median throughput, MiB/s")
    ax.set_title("IPC throughput vs message size")
    ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
    ax.legend(fontsize=8, ncol=2)
    fig.tight_layout()
    fig.savefig(out, dpi=180)
    plt.close(fig)


def horizontal_bar(summary: pd.DataFrame, mode: str, size: int, out: Path):
    data = summary[
        (summary["mode"] == mode)
        & (summary["message_size"] == size)
    ].copy()

    if data.empty:
        return

    # Lower latency is better; higher throughput is better.
    ascending = mode == "latency"
    data = data.sort_values("median", ascending=ascending)

    labels = [
        METHOD_LABELS.get(method, method)
        for method in data["method"]
    ]

    values = data["median"].astype(float)

    if mode == "latency":
        values = values / 1000.0
        xlabel = "Median latency, μs"
        title = f"IPC latency — {SIZE_LABELS[size]}"
    else:
        xlabel = "Median throughput, MiB/s"
        title = f"IPC throughput — {SIZE_LABELS[size]}"

    fig, ax = plt.subplots(figsize=(10, 6))
    y = range(len(data))
    ax.barh(list(y), values)
    ax.set_yticks(list(y))
    ax.set_yticklabels(labels)
    ax.set_xlabel(xlabel)
    ax.set_title(title)
    ax.grid(True, axis="x", linewidth=0.4, alpha=0.5)

    if mode == "latency":
        ax.invert_yaxis()

    for i, value in enumerate(values):
        ax.text(
            value,
            i,
            f" {value:.2f}",
            va="center",
            fontsize=8,
        )

    fig.tight_layout()
    fig.savefig(out, dpi=180)
    plt.close(fig)


def main():
    csv_path = (
        Path(sys.argv[1])
        if len(sys.argv) > 1
        else Path("results/results.csv")
    )

    df = load_results(csv_path)
    summary = build_summary(df)

    out_dir = csv_path.parent / "plots"
    out_dir.mkdir(parents=True, exist_ok=True)

    summary_path = csv_path.parent / "summary_median.csv"
    summary.to_csv(summary_path, index=False)

    plot_latency(summary, out_dir / "latency_vs_size.png")
    plot_throughput(summary, out_dir / "throughput_vs_size.png")
    horizontal_bar(
        summary,
        "latency",
        64,
        out_dir / "latency_64B.png",
    )
    horizontal_bar(
        summary,
        "throughput",
        1048576,
        out_dir / "throughput_1MiB.png",
    )

    print(f"Loaded measurements: {len(df)}")
    print(f"Summary rows: {len(summary)}")
    print(f"Saved: {summary_path}")
    print(f"Plots directory: {out_dir}")
    print("Generated:")
    print("  latency_vs_size.png")
    print("  throughput_vs_size.png")
    print("  latency_64B.png")
    print("  throughput_1MiB.png")


if __name__ == "__main__":
    main()
