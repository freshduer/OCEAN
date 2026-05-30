#!/bin/bash
# Host-side 2-role benchmark: one TCP client reads, another writes (simulates VM1 reader + VM0 writer).
# Default: bulk RPC (256KiB–1MiB per op) for hundreds of MiB/s on localhost TCP.
# Legacy 64B cacheline mode: RW_BENCH_MODE=cacheline
set -euo pipefail

OCEAN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${OCEAN_ROOT}/build"
SERVER="${BUILD_DIR}/cxlmemsim_server"
TOPO="${OCEAN_TOPOLOGY:-${OCEAN_ROOT}/qemu_integration/topology_simple.txt}"
PORT="${CXL_MEMSIM_PORT:-9999}"
HOST="${CXL_MEMSIM_HOST:-127.0.0.1}"
CAPACITY_MB="${CXL_CAPACITY_MB:-1024}"
DURATION="${RW_BENCH_SEC:-15}"
WARMUP_SEC="${RW_WARMUP_SEC:-3}"
BENCH_MODE="${RW_BENCH_MODE:-bulk}"
BULK_BYTES="${RW_BULK_BYTES:-262144}"
OUT_DIR="${OCEAN_ROOT}/results/2host_rw_tcp"
mkdir -p "${OUT_DIR}"

if ! pgrep -f '[c]xlmemsim_server' >/dev/null; then
  echo "[..] starting cxlmemsim_server tcp :${PORT} (${CAPACITY_MB} MiB)..."
  nohup "${SERVER}" --capacity="${CAPACITY_MB}" --comm-mode tcp --port "${PORT}" \
    -t "${TOPO}" > "${OUT_DIR}/server.log" 2>&1 &
  sleep 2
fi

if ! ss -ltn | grep -q ":${PORT} "; then
  echo "[FAIL] TCP ${PORT} not listening (rebuild server after bulk protocol change)"
  exit 1
fi

echo "=== 2-host RW TCP bench (mode=${BENCH_MODE}, ${DURATION}s + ${WARMUP_SEC}s warmup) ==="
echo "  server=${HOST}:${PORT} capacity=${CAPACITY_MB}MiB bulk_bytes=${BULK_BYTES}"
echo "  output=${OUT_DIR}/rw_bench.json"

python3 - "${HOST}" "${PORT}" "${DURATION}" "${WARMUP_SEC}" "${BENCH_MODE}" "${BULK_BYTES}" \
  "${OUT_DIR}/rw_bench.json" <<'PY'
import json, socket, struct, sys, threading, time
from statistics import mean

host, port_s, dur_s, warm_s, mode, bulk_s, out_path = sys.argv[1:8]
port = int(port_s)
duration = float(dur_s)
warmup = float(warm_s)
bulk = int(bulk_s)
CAP_BYTES = 1024 * 1024 * 1024

OP_READ, OP_WRITE = 0, 1
OP_BULK_READ, OP_BULK_WRITE = 8, 9
REQ_CL, RESP_CL = 97, 81
BULK_HDR = 25
BULK_RESP_HDR = 9


def connect():
    return socket.create_connection((host, port), timeout=30)


def recvall(s, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("connection closed")
        buf.extend(chunk)
    return bytes(buf)


def bulk_read(s, addr, size):
    t0 = time.perf_counter_ns()
    s.sendall(struct.pack("<BQQQ", OP_BULK_READ, addr, size, time.time_ns()))
    hdr = recvall(s, BULK_RESP_HDR)
    if hdr[0] != 0:
        raise RuntimeError(f"bulk_read status={hdr[0]}")
    recvall(s, size)
    return time.perf_counter_ns() - t0, size


def bulk_write(s, addr, buf):
    t0 = time.perf_counter_ns()
    s.sendall(struct.pack("<BQQQ", OP_BULK_WRITE, addr, len(buf), time.time_ns()) + buf)
    hdr = recvall(s, BULK_RESP_HDR)
    if hdr[0] != 0:
        raise RuntimeError(f"bulk_write status={hdr[0]}")
    return time.perf_counter_ns() - t0, len(buf)


def cacheline_op(s, op, addr):
    t0 = time.perf_counter_ns()
    s.sendall(struct.pack("<BQQQQQ", op, addr, 64, time.time_ns(), 0, 0) + bytes(64))
    r = s.recv(RESP_CL)
    if len(r) != RESP_CL or r[0] != 0:
        raise RuntimeError(f"cl op={op} status={r[0] if r else -1}")
    return time.perf_counter_ns() - t0, 64


def worker(op, stop_at, measure_from, lat_ns, bytes_acc):
    s = connect()
    addr = 0
    ops = 0
    err = 0
    write_buf = bytes(bulk) if mode == "bulk" and op == OP_WRITE else None
    stride = bulk if mode == "bulk" else 64
    while time.perf_counter() < stop_at:
        try:
            if mode == "bulk":
                if op == OP_READ:
                    dt, n = bulk_read(s, addr, bulk)
                else:
                    dt, n = bulk_write(s, addr, write_buf)
            else:
                dt, n = cacheline_op(s, op, addr)
            if time.perf_counter() >= measure_from:
                lat_ns.append(dt)
                bytes_acc[0] += n
                ops += 1
            addr = (addr + stride) % (CAP_BYTES - stride)
        except (OSError, RuntimeError):
            err += 1
            try:
                s.close()
            except OSError:
                pass
            s = connect()
    try:
        s.close()
    except OSError:
        pass
    return ops, err


def pct(v, p):
    if not v:
        return 0
    s = sorted(v)
    return s[min(len(s) - 1, int(len(s) * p))]


t0 = time.perf_counter()
stop_warm = t0 + warmup
stop_run = stop_warm + duration

read_lat, write_lat = [], []
read_bytes, write_bytes = [0], [0]
results = {}


def run_role(key, op):
    ba = read_bytes if op == OP_READ else write_bytes
    la = read_lat if op == OP_READ else write_lat
    results[key] = worker(op, stop_run, stop_warm, la, ba)


tr = threading.Thread(target=lambda: run_role("reader", OP_READ), daemon=True)
tw = threading.Thread(target=lambda: run_role("writer", OP_WRITE), daemon=True)
tr.start()
tw.start()
tr.join()
tw.join()
read_ops, read_err = results["reader"]
write_ops, write_err = results["writer"]


def summarize(name, lat, ops, err, nbytes):
    sec = duration
    mbs = nbytes / (1024 * 1024) / sec if sec > 0 else 0
    return {
        "role": name,
        "ops": ops,
        "bytes": nbytes,
        "errors": err,
        "throughput_mib_s": round(mbs, 2),
        "throughput_ops_s": round(ops / sec, 1) if sec > 0 else 0,
        "latency_us": {
            "p50": round(pct(lat, 0.50) / 1000, 2),
            "p99": round(pct(lat, 0.99) / 1000, 2),
            "avg": round(mean(lat) / 1000, 2) if lat else 0,
        },
    }


report = {
    "host": host,
    "port": port,
    "mode": mode,
    "bulk_bytes": bulk if mode == "bulk" else 64,
    "duration_s": duration,
    "warmup_s": warmup,
    "reader": summarize("reader", read_lat, read_ops, read_err, read_bytes[0]),
    "writer": summarize("writer", write_lat, write_ops, write_err, write_bytes[0]),
}
with open(out_path, "w") as f:
    json.dump(report, f, indent=2)

print(json.dumps(report, indent=2))
print("")
agg_mib = (read_bytes[0] + write_bytes[0]) / (1024 * 1024) / duration
print(f"AGG   {agg_mib:.1f} MiB/s (read+write bytes / {duration}s)")
print(f"READ  {report['reader']['throughput_mib_s']} MiB/s  p50={report['reader']['latency_us']['p50']}us")
print(f"WRITE {report['writer']['throughput_mib_s']} MiB/s  p50={report['writer']['latency_us']['p50']}us")
PY

echo ""
echo "Saved: ${OUT_DIR}/rw_bench.json"
echo "Tip: RW_BULK_BYTES=1048576 for 1MiB blocks; RW_BENCH_MODE=cacheline for legacy ~1MiB/s."
