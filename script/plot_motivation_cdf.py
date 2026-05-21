#!/usr/bin/env python3
"""Plot motivation access / cache / miss CDFs for 02-motivation."""

from __future__ import annotations

import argparse
from pathlib import Path

import matplotlib.pyplot as plt
import pandas as pd


def load_xy_csv(path: Path, xcol: str, ycol: str) -> tuple[list[float], list[float]]:
    df = pd.read_csv(path)
    return df[xcol].tolist(), df[ycol].tolist()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--kind", choices=["access", "tier", "miss"], required=True)
    parser.add_argument("--target", type=Path, help="Target access CDF CSV")
    parser.add_argument("--empirical", type=Path, help="Empirical access CDF CSV")
    parser.add_argument("--tier", type=Path, help="Cache tier CDF CSV")
    parser.add_argument("--miss", type=Path, help="Miss vs access CSV")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--title", type=str, default="")
    args = parser.parse_args()

    fig, ax = plt.subplots(figsize=(9, 5.5))

    if args.kind == "access":
        if args.target:
            x, y = load_xy_csv(args.target, "rank_frac", "cum_access_frac")
            ax.plot(x, y, label="① target (extract CDF)", lw=2.5, color="#4e79a7")
        if args.empirical:
            x, y = load_xy_csv(args.empirical, "rank_frac", "cum_access_frac")
            ax.plot(x, y, label="② replay (trace histogram)", lw=2, color="#e15759", linestyle="--")
        ax.set_xlabel("Row rank fraction (hottest → coldest)")
        ax.set_ylabel("Cumulative access probability")
        ax.set_title(args.title or "Access skew CDF")
        ax.legend(loc="lower right")
        if args.target and args.empirical:
            tx, ty = load_xy_csv(args.target, "rank_frac", "cum_access_frac")
            ex, ey = load_xy_csv(args.empirical, "rank_frac", "cum_access_frac")
            gap = max(abs(a - b) for a, b in zip(ty, ey))
            ax.text(0.02, 0.02, f"max |①-②| = {gap:.4f}", transform=ax.transAxes, fontsize=10)
    elif args.kind == "tier":
        df = pd.read_csv(args.tier)
        ax.step(df["tier"], df["cum_frac"], where="post", lw=2.5, color="#59a14f")
        ax.set_xlabel("Cache tier (1=L1 HBM, 2=L2 DDR, 3=L3 CXL)")
        ax.set_ylabel("CDF of lookups")
        ax.set_title("③a Cache tier CDF")
        ax.set_xticks([1, 2, 3])
    elif args.kind == "miss":
        df = pd.read_csv(args.miss)
        ycol = "l3_rate" if "l3_rate" in df.columns else "l3_rate_cold"
        ax.plot(df["rank_frac"], df[ycol], label="L3 miss rate by row-rank bucket", lw=2, color="#76b7b2")
        ax.set_xlabel("Row rank fraction (hottest → coldest)")
        ax.set_ylabel("L3 miss rate")
        ax.set_title("③b Miss vs access heat")
        ax.legend(loc="upper left")

    ax.grid(True, alpha=0.3)
    ax.set_ylim(0, 1.05)
    args.out.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(args.out, dpi=150)
    print(f"Wrote {args.out}")


if __name__ == "__main__":
    main()
