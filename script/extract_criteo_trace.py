#!/usr/bin/env python3
"""
Build motivation read_trace.bin from Criteo Kaggle npz (temporal order).

DLRM: each ad row → 26 categorical embedding lookups (one per feature table).
row_id = (vocab_offset[feat] + category_id) % N_logical  (slot in 10 GiB CXL pool)

Outputs: read_trace.bin, trace_meta.json, target_access_cdf.csv
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from collections import Counter
from pathlib import Path

import numpy as np

NUM_FEATURES = 26


def n_logical_rows(table_gib: float, dim: int) -> int:
    bytes_per_row = dim * 4
    return int(table_gib * (1024**3) // bytes_per_row)


def vocab_offsets(counts: np.ndarray) -> np.ndarray:
    off = np.zeros(NUM_FEATURES, dtype=np.uint64)
    for f in range(1, NUM_FEATURES):
        off[f] = off[f - 1] + int(counts[f - 1])
    return off


def embedding_row_id(feat: int, cat_id: int, offsets: np.ndarray, n_logical: int) -> int:
    global_id = int(offsets[feat]) + int(cat_id)
    return global_id % n_logical


def hot_rows_for_mass(counts: Counter[int], mass: float) -> int:
    items = sorted(counts.items(), key=lambda x: -x[1])
    total = sum(counts.values())
    target = int(math.ceil(mass * total))
    cum = 0
    n = 0
    for _, c in items:
        cum += c
        n += 1
        if cum >= target:
            break
    return max(1, n)


def top10_access_frac(counts: Counter[int]) -> float:
    if not counts:
        return 0.0
    items = sorted(counts.items(), key=lambda x: -x[1])
    n_hot = max(1, math.ceil(0.10 * len(items)))
    hot_mass = sum(c for _, c in items[:n_hot])
    total = sum(counts.values())
    return hot_mass / total


def access_cdf_by_frequency(counts: Counter[int], points: int = 512) -> tuple[list[float], list[float]]:
    items = sorted(counts.items(), key=lambda x: -x[1])
    total = sum(counts.values())
    xs: list[float] = []
    ys: list[float] = []
    n = len(items)
    for i in range(points):
        frac = (i + 1) / points
        cutoff = max(1, int(math.ceil(frac * n)))
        cum = sum(c for _, c in items[:cutoff])
        xs.append(frac)
        ys.append(cum / total)
    return xs, ys


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--npz",
        type=Path,
        default=Path.home() / "downloads" / "kaggleAdDisplayChallenge_processed.npz",
    )
    ap.add_argument("--out-dir", type=Path, default=Path("results/motivation/trace"))
    ap.add_argument("--table-gib", type=float, default=10.0)
    ap.add_argument("--dim", type=int, default=128)
    ap.add_argument(
        "--queries",
        type=int,
        default=10_000_000,
        help="Total embedding lookups Q (rounded down to multiple of 26)",
    )
    ap.add_argument("--offset", type=int, default=0, help="First ad row index in dataset")
    ap.add_argument("--stride", type=int, default=1, help="Ad row stride (1 = consecutive)")
    ap.add_argument("--l1-gib", type=float, default=1.0)
    ap.add_argument("--l2-gib", type=float, default=5.0)
    args = ap.parse_args()

    args.out_dir.mkdir(parents=True, exist_ok=True)
    n_logical = n_logical_rows(args.table_gib, args.dim)

    num_samples = args.queries // NUM_FEATURES
    total_lookups = num_samples * NUM_FEATURES
    if total_lookups == 0:
        raise SystemExit("--queries must be >= 26")
    if total_lookups != args.queries:
        print(f"Note: Q rounded {args.queries} → {total_lookups} ({num_samples} ad rows × 26)")

    print(f"Loading mmap {args.npz} ...")
    z = np.load(args.npz, mmap_mode="r")
    x_cat = z["X_cat"]
    counts_vocab = z["counts"]
    offsets = vocab_offsets(counts_vocab)
    n_rows = x_cat.shape[0]
    last_ad = args.offset + (num_samples - 1) * args.stride
    if last_ad >= n_rows:
        raise SystemExit(f"Need ad row {last_ad} but dataset has {n_rows}")

    trace_path = args.out_dir / "read_trace.bin"
    freq: Counter[int] = Counter()
    print(
        f"Extract {num_samples} ad rows × {NUM_FEATURES} = {total_lookups} lookups "
        f"(N_logical={n_logical}) ..."
    )
    written = 0
    with open(trace_path, "wb") as fout:
        for s in range(num_samples):
            ri = args.offset + s * args.stride
            cats = x_cat[ri]
            for f in range(NUM_FEATURES):
                rid = embedding_row_id(f, int(cats[f]), offsets, n_logical)
                freq[rid] += 1
                fout.write(struct.pack("<I", rid))
                written += 1
            if (s + 1) % 200_000 == 0:
                print(f"  {s + 1}/{num_samples} ad rows ({written} lookups)")

    top10 = top10_access_frac(freq)
    u = len(freq)
    xs, ys = access_cdf_by_frequency(freq)
    cdf_path = args.out_dir / "target_access_cdf.csv"
    with open(cdf_path, "w") as f:
        f.write("rank_frac,cum_access_frac\n")
        for x, y in zip(xs, ys):
            f.write(f"{x},{y}\n")

    meta = {
        "workload": "criteo",
        "source_npz": str(args.npz.resolve()),
        "table_gib": args.table_gib,
        "dim": args.dim,
        "n_logical": n_logical,
        "queries": total_lookups,
        "num_ad_rows": num_samples,
        "lookups_per_ad_row": NUM_FEATURES,
        "vocab_total": int(offsets[-1] + counts_vocab[-1]),
        "offset": args.offset,
        "stride": args.stride,
        "row_id_formula": "offset[feat]+cat_id mod n_logical",
        "unique_slots": u,
        "unique_frac": u / total_lookups,
        "top10_mass_actual": top10,
        "hot_rows_mass926": hot_rows_for_mass(freq, 0.926),
        "logical_hot_rows": n_logical // 10,
        "l1_gib": args.l1_gib,
        "l2_gib": args.l2_gib,
        "sim_cxl_mb": args.table_gib * 1024,
        "l1_rows": int(args.l1_gib * (1024**3) // (args.dim * 4)),
        "l2_rows": int(args.l2_gib * (1024**3) // (args.dim * 4)),
        "hot_mode": "freq",
        "warmup_passes_default": 0,
        "warmup_queries_default": total_lookups // 2,
    }
    meta_path = args.out_dir / "trace_meta.json"
    meta_path.write_text(json.dumps(meta, indent=2) + "\n")
    print(f"Wrote {trace_path} ({written * 4} bytes)")
    print(f"unique_slots={u} top10_mass={top10:.4f} meta={meta_path}")


if __name__ == "__main__":
    main()
