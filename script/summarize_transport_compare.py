#!/usr/bin/env python3
"""Summarize TCP vs PGAS-SHM vs ring-buffer SHM latency CSVs into a table."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import pandas as pd


def stats(path: Path) -> dict[str, float]:
    df = pd.read_csv(path)
    rtt = df["rtt_ns"].to_numpy(dtype=np.float64)
    return {
        "n": float(len(rtt)),
        "mean_us": float(np.mean(rtt) / 1000.0),
        "p50_us": float(np.percentile(rtt, 50) / 1000.0),
        "p90_us": float(np.percentile(rtt, 90) / 1000.0),
        "p99_us": float(np.percentile(rtt, 99) / 1000.0),
        "min_us": float(np.min(rtt) / 1000.0),
        "max_us": float(np.max(rtt) / 1000.0),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tcp", type=Path, required=True)
    parser.add_argument("--pgas", type=Path, required=True)
    parser.add_argument("--ring", type=Path, required=True)
    parser.add_argument("--out-md", type=Path, default=Path("results/transport_compare/summary.md"))
    parser.add_argument("--out-csv", type=Path, default=Path("results/transport_compare/summary.csv"))
    args = parser.parse_args()

    rows = []
    for label, path in (
        ("TCP", args.tcp),
        ("PGAS-SHM", args.pgas),
        ("Ring-SHM", args.ring),
    ):
        if not path.is_file():
            raise SystemExit(f"missing: {path}")
        s = stats(path)
        s["transport"] = label
        rows.append(s)

    df = pd.DataFrame(rows)[
        ["transport", "n", "mean_us", "p50_us", "p90_us", "p99_us", "min_us", "max_us"]
    ]

    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    args.out_csv.parent.mkdir(parents=True, exist_ok=True)
    df.to_csv(args.out_csv, index=False)

    tcp_p50 = df.loc[df["transport"] == "TCP", "p50_us"].iloc[0]
    try:
        table = df.to_markdown(index=False, floatfmt=".2f")
    except ImportError:
        table = df.to_string(index=False, float_format=lambda x: f"{x:.2f}")

    md_lines = [
        "# CXLMemSim transport latency comparison",
        "",
        "Cacheline READ RTT (client-measured, includes server simulated latency in response).",
        "",
        "```",
        table,
        "```",
        "",
        "## Relative to TCP p50",
        "",
    ]
    for _, row in df.iterrows():
        ratio = row["p50_us"] / tcp_p50 if tcp_p50 > 0 else float("nan")
        md_lines.append(f"- **{row['transport']}**: p50 = {row['p50_us']:.2f} µs ({ratio:.2f}× TCP)")
    md_lines.append("")

    args.out_md.write_text("\n".join(md_lines), encoding="utf-8")

    print(df.to_string(index=False, float_format=lambda x: f"{x:.2f}"))
    print(f"\nWrote {args.out_csv}")
    print(f"Wrote {args.out_md}")


if __name__ == "__main__":
    main()
