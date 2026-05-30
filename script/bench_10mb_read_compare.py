#!/usr/bin/env python3
"""Measure wall-clock latency to move 10 MiB (one logical read) over TCP bulk vs PGAS-SHM slot path."""
from __future__ import annotations

import argparse
import json
import mmap
import os
import socket
import struct
import statistics
import sys
import time
from typing import Callable, List, Tuple

MIB = 10 * 1024 * 1024
CACHELINE = 64
CHUNK_1M = 1 << 20  # CXL_BULK_MAX_SIZE

OP_READ = 0
OP_BULK_READ = 8
BULK_HDR = 25
BULK_RESP_HDR = 9
REQ_CL = 97
RESP_CL = 81

CXL_SHM_MAGIC = 0x43584C53484D454D
CXL_SHM_REQ_NONE = 0
CXL_SHM_REQ_READ = 1
CXL_SHM_RESP_NONE = 0
CXL_SHM_RESP_OK = 1


def recvall(sock: socket.socket, n: int) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("connection closed")
        buf.extend(chunk)
    return bytes(buf)


def pct(vals: List[float], p: float) -> float:
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(len(s) * p))]


def summarize_ns(samples_ns: List[int]) -> dict:
    if not samples_ns:
        return {}
    s = sorted(samples_ns)
    return {
        "n": len(s),
        "min_ms": s[0] / 1e6,
        "p50_ms": pct(s, 0.50) / 1e6,
        "p99_ms": pct(s, 0.99) / 1e6,
        "max_ms": s[-1] / 1e6,
        "avg_ms": statistics.mean(s) / 1e6,
        "throughput_mib_s": MIB / (statistics.mean(s) / 1e9) / (1024 * 1024),
    }


def tcp_bulk_10mb(host: str, port: int, addr: int) -> Tuple[int, int]:
    """10 MiB via 10 x 1 MiB bulk RPC (server max chunk = 1 MiB)."""
    sock = socket.create_connection((host, port), timeout=120)
    t0 = time.perf_counter_ns()
    off = 0
    for _ in range(10):
        sock.sendall(struct.pack("<BQQQ", OP_BULK_READ, addr + off, CHUNK_1M, time.time_ns()))
        hdr = recvall(sock, BULK_RESP_HDR)
        if hdr[0] != 0:
            raise RuntimeError(f"bulk status={hdr[0]}")
        recvall(sock, CHUNK_1M)
        off += CHUNK_1M
    dt = time.perf_counter_ns() - t0
    sock.close()
    return dt, MIB


def tcp_cacheline_10mb(host: str, port: int, addr: int) -> Tuple[int, int]:
    sock = socket.create_connection((host, port), timeout=600)
    t0 = time.perf_counter_ns()
    off = 0
    ops = MIB // CACHELINE
    for i in range(ops):
        a = addr + ((i * CACHELINE) % (1024 * 1024 * 1024 - CACHELINE))
        sock.sendall(struct.pack("<BQQQQQ", OP_READ, a, CACHELINE, time.time_ns(), 0, 0) + bytes(64))
        r = sock.recv(RESP_CL)
        if len(r) != RESP_CL or r[0] != 0:
            raise RuntimeError(f"cl read failed at i={i}")
        off += CACHELINE
    dt = time.perf_counter_ns() - t0
    sock.close()
    return dt, MIB


def pgas_shm_path(shm_name: str) -> str:
    """POSIX shm name /foo -> /dev/shm/foo on Linux."""
    name = shm_name if shm_name.startswith("/") else "/" + shm_name
    return "/dev/shm" + name


def pgas_open_mmap(shm_name: str):
    path = pgas_shm_path(shm_name)
    fd = os.open(path, os.O_RDWR)
    st = os.fstat(fd)
    mapped = mmap.mmap(fd, st.st_size, mmap.MAP_SHARED, mmap.PROT_READ | mmap.PROT_WRITE)
    os.close(fd)
    magic = struct.unpack_from("<Q", mapped, 0)[0]
    if magic != CXL_SHM_MAGIC:
        raise RuntimeError(f"bad PGAS magic 0x{magic:x}")
    num_slots = struct.unpack_from("<I", mapped, 12)[0]
    memory_size = struct.unpack_from("<Q", mapped, 32)[0]
    entry_size = struct.unpack_from("<I", mapped, 48)[0]
    # cxl_shm_header_t: 64-byte header + slots
    slot_size = 256  # aligned cxl_shm_slot_t
    header_size = 64 + num_slots * slot_size
    pool = memoryview(mapped)[header_size:]
    return mapped, pool, memory_size, entry_size, header_size


def pgas_slot_read_10mb(mapped, pool, entry_size: int, addr: int, slot_idx: int = 0) -> Tuple[int, int]:
    """Current PGAS protocol: 64 B per slot round-trip."""
    num_slots = struct.unpack_from("<I", mapped, 12)[0]
    slot_off = 64 + slot_idx * 256
    t0 = time.perf_counter_ns()
    ops = MIB // CACHELINE
    for i in range(ops):
        line_addr = addr + (i * CACHELINE) % max(64, struct.unpack_from("<Q", mapped, 32)[0] - CACHELINE)
        cacheline_idx = line_addr // 64
        ent_off = cacheline_idx * entry_size

        # wait slot free
        for _ in range(50_000_000):
            if struct.unpack_from("<I", mapped, slot_off)[0] == CXL_SHM_REQ_NONE:
                break
        else:
            raise RuntimeError("slot busy timeout")

        struct.pack_into("<Q", mapped, slot_off + 8, line_addr)
        struct.pack_into("<Q", mapped, slot_off + 16, CACHELINE)
        struct.pack_into("<I", mapped, slot_off + 4, CXL_SHM_RESP_NONE)
        struct.pack_into("<I", mapped, slot_off + 0, CXL_SHM_REQ_READ)

        for _ in range(50_000_000):
            st = struct.unpack_from("<I", mapped, slot_off + 4)[0]
            if st != CXL_SHM_RESP_NONE:
                if st != CXL_SHM_RESP_OK:
                    raise RuntimeError(f"pgas resp status={st}")
                break
        else:
            raise RuntimeError("pgas response timeout")

        struct.pack_into("<I", mapped, slot_off + 4, CXL_SHM_RESP_NONE)
        struct.pack_into("<I", mapped, slot_off + 0, CXL_SHM_REQ_NONE)

    dt = time.perf_counter_ns() - t0
    return dt, MIB


def pgas_mmap_direct_10mb(pool, entry_size: int, addr: int) -> Tuple[int, int]:
    """Upper bound: memcpy 10 MiB from PGAS pool (no slot RPC, no coherency)."""
    t0 = time.perf_counter_ns()
    start = (addr // 64) * entry_size
    buf = bytearray(MIB)
    view = pool[start : start + MIB + entry_size * (MIB // 64)]
    # copy 10 MiB from entry data portions (64 B each entry)
    pos = 0
    for i in range(MIB // CACHELINE):
        ent = (addr // 64 + i) * entry_size
        buf[pos : pos + CACHELINE] = pool[ent : ent + CACHELINE]
        pos += CACHELINE
    _ = buf[0]  # keep
    dt = time.perf_counter_ns() - t0
    return dt, MIB


def run_series(fn: Callable[[], Tuple[int, int]], warmup: int, samples: int) -> List[int]:
    for _ in range(warmup):
        fn()
    out: List[int] = []
    for _ in range(samples):
        dt, _ = fn()
        out.append(dt)
    return out


def rdma_theory() -> dict:
    """Reference numbers for discussion (not measured here)."""
    gbps_cxl_64 = 64.0
    gbps_ib_100 = 100.0
    us_small_msg = 3.0  # typical RDMA/CXL.mem small READ RTT order-of-magnitude
    return {
        "note": "Literature / product order-of-magnitude; not OCEAN measured",
        "small_msg_rtt_us_typical": us_small_msg,
        "ten_mb_at_64_gbps_cxl_ms": (MIB * 8 / (gbps_cxl_64 * 1e9)) * 1e3,
        "ten_mb_at_100_gbps_rdma_ms": (MIB * 8 / (gbps_ib_100 * 1e9)) * 1e3,
        "ten_mb_if_64b_at_3us_each_ms": (MIB / CACHELINE) * us_small_msg / 1e3,
        "ten_mb_if_64b_at_5us_each_ms": (MIB / CACHELINE) * 5.0 / 1e3,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description="10 MiB read latency: TCP vs PGAS-SHM")
    ap.add_argument("--tcp-host", default="127.0.0.1")
    ap.add_argument("--tcp-port", type=int, default=9999)
    ap.add_argument("--pgas-shm", default="/cxlmemsim_pgas_bench10")
    ap.add_argument("--addr", type=lambda x: int(x, 0), default=0)
    ap.add_argument("--warmup", type=int, default=3)
    ap.add_argument("--samples", type=int, default=15)
    ap.add_argument("--skip-tcp-cl", action="store_true", help="skip slow TCP 64B x 163840")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    report = {
        "transfer_bytes": MIB,
        "transfer_mib": 10,
        "tcp_bulk_chunk": CHUNK_1M,
        "tcp_bulk_ops_per_10mb": 10,
        "pgas_slot_ops_per_10mb": MIB // CACHELINE,
        "rdma_theory": rdma_theory(),
        "measurements": {},
    }

    print(f"=== 10 MiB read benchmark (warmup={args.warmup}, samples={args.samples}) ===")

    print("[1/4] TCP bulk (10 x 1 MiB)...")
    try:
        ns = run_series(
            lambda: tcp_bulk_10mb(args.tcp_host, args.tcp_port, args.addr),
            args.warmup,
            args.samples,
        )
        report["measurements"]["tcp_bulk_10mb"] = summarize_ns(ns)
        print(json.dumps(report["measurements"]["tcp_bulk_10mb"], indent=2))
    except OSError as e:
        report["measurements"]["tcp_bulk_10mb"] = {"error": str(e)}
        print(f"  SKIP/FAIL: {e}")

    if not args.skip_tcp_cl:
        print("[2/4] TCP cacheline (163840 x 64B) — slow...")
        try:
            ns = run_series(
                lambda: tcp_cacheline_10mb(args.tcp_host, args.tcp_port, args.addr),
                min(args.warmup, 1),
                min(args.samples, 5),
            )
            report["measurements"]["tcp_cacheline_10mb"] = summarize_ns(ns)
            print(json.dumps(report["measurements"]["tcp_cacheline_10mb"], indent=2))
        except OSError as e:
            report["measurements"]["tcp_cacheline_10mb"] = {"error": str(e)}
            print(f"  SKIP/FAIL: {e}")
    else:
        report["measurements"]["tcp_cacheline_10mb"] = {"skipped": True}

    print("[3/4] PGAS-SHM slot (163840 x 64B)...")
    try:
        mapped, pool, mem_sz, ent_sz, _ = pgas_open_mmap(args.pgas_shm)
        try:
            ready = struct.unpack_from("<I", mapped, 16)[0]
            if ready == 0:
                print("  WARN: server_ready=0")

            def do_slot():
                return pgas_slot_read_10mb(mapped, pool, ent_sz, args.addr, 0)

            ns = run_series(do_slot, args.warmup, args.samples)
            report["measurements"]["pgas_slot_10mb"] = summarize_ns(ns)
            print(json.dumps(report["measurements"]["pgas_slot_10mb"], indent=2))

            print("[4/4] PGAS mmap direct (10 MiB memcpy, no slot — upper bound)...")
            ns2 = run_series(
                lambda: pgas_mmap_direct_10mb(pool, ent_sz, args.addr),
                args.warmup,
                args.samples,
            )
            report["measurements"]["pgas_mmap_direct_10mb"] = summarize_ns(ns2)
            print(json.dumps(report["measurements"]["pgas_mmap_direct_10mb"], indent=2))
        finally:
            mapped.close()
    except OSError as e:
        report["measurements"]["pgas_slot_10mb"] = {"error": str(e)}
        report["measurements"]["pgas_mmap_direct_10mb"] = {"error": str(e)}
        print(f"  SKIP/FAIL: {e}")

    print("\n=== RDMA / CXL theory (reference) ===")
    print(json.dumps(report["rdma_theory"], indent=2))

    if args.out:
        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        with open(args.out, "w") as f:
            json.dump(report, f, indent=2)
        print(f"\nWrote {args.out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
