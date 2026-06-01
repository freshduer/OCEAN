#!/usr/bin/env python3
"""Plot RTT CDF for TCP vs PGAS-SHM vs ring-buffer SHM."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np
import pandas as pd


def load_rtt(path: Path) -> np.ndarray:
    return pd.read_csv(path)["rtt_ns"].to_numpy(dtype=np.float64)


def cdf(values: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    s = np.sort(values)
    return s, np.arange(1, len(s) + 1) / len(s)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tcp", type=Path, required=True)
    parser.add_argument("--pgas", type=Path, required=True)
    parser.add_argument("--ring", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--unit", choices=["ns", "us"], default="us")
    args = parser.parse_args()

    scale = 1.0 if args.unit == "ns" else 1000.0
    xlab = "Round-trip latency (ns)" if args.unit == "ns" else "Round-trip latency (µs)"

    fig, ax = plt.subplots(figsize=(10, 6))
    for label, path, color in (
        ("TCP", args.tcp, "#e15759"),
        ("PGAS-SHM", args.pgas, "#4e79a7"),
        ("Ring-SHM", args.ring, "#59a14f"),
    ):
        data = load_rtt(path)
        x, y = cdf(data)
        p50 = np.percentile(data, 50) / scale
        ax.plot(x / scale, y, lw=2, color=color, label=f"{label}  p50={p50:.2f}")

    ax.set_xlabel(xlab)
    ax.set_ylabel("CDF")
    ax.set_title("CXLMemSim: TCP vs PGAS-SHM vs ring-buffer SHM (cacheline READ)")
    ax.grid(True, alpha=0.3)
    ax.legend(loc="lower right")
    ax.set_ylim(0, 1.0)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(args.out, dpi=150)
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
