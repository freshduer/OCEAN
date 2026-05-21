#!/usr/bin/env python3
"""Plot RTT latency CDF for TCP vs PGAS-SHM CXLMemSim benchmarks."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


def load_rtt_csv(path: Path) -> np.ndarray:
    df = pd.read_csv(path)
    if "rtt_ns" not in df.columns:
        raise ValueError(f"{path} missing rtt_ns column")
    return df["rtt_ns"].to_numpy(dtype=np.float64)


def cdf(values: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    sorted_v = np.sort(values)
    y = np.arange(1, len(sorted_v) + 1) / len(sorted_v)
    return sorted_v, y


def main() -> None:
    parser = argparse.ArgumentParser(description="Plot latency CDF comparison")
    parser.add_argument("--tcp", type=Path, required=True, help="TCP benchmark CSV")
    parser.add_argument("--pgas", type=Path, required=True, help="PGAS-SHM benchmark CSV")
    parser.add_argument("--out", type=Path, default=Path("results/latency_cdf/latency_cdf.png"))
    parser.add_argument("--unit", choices=["ns", "us"], default="us")
    args = parser.parse_args()

    tcp = load_rtt_csv(args.tcp)
    pgas = load_rtt_csv(args.pgas)
    scale = 1.0 if args.unit == "ns" else 1000.0
    xlab = "Round-trip latency (ns)" if args.unit == "ns" else "Round-trip latency (µs)"

    fig, ax = plt.subplots(figsize=(9, 5.5))
    for label, data, color in (
        ("TCP (socket RPC)", tcp, "#e15759"),
        ("PGAS-SHM (CXL shared memory)", pgas, "#4e79a7"),
    ):
        x, y = cdf(data)
        ax.plot(x / scale, y, label=f"{label}  p50={np.percentile(data, 50)/scale:.2f}", lw=2, color=color)

    ax.set_xlabel(xlab)
    ax.set_ylabel("CDF")
    ax.set_title("CXLMemSim: TCP vs PGAS-SHM client RTT latency")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="lower right")
    ax.set_ylim(0, 1.0)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(args.out, dpi=150)
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
