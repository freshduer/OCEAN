#!/bin/bash
# End-to-end: TCP vs PGAS-SHM vs ring-buffer SHM (cacheline READ RTT).
set -euo pipefail

OCEAN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${OCEAN_ROOT}/build"
SERVER="${BUILD_DIR}/cxlmemsim_server"
BENCH_C="${BUILD_DIR}/microbench/cxl_transport_latency_bench"
BENCH_RING="${BUILD_DIR}/microbench/ring_shm_latency_bench"
OUT_DIR="${OCEAN_ROOT}/results/transport_compare"

SAMPLES="${SAMPLES:-20000}"
WARMUP="${WARMUP:-2000}"
PORT="${PORT:-9998}"
PGAS_SHM="${PGAS_SHM:-/cxlmemsim_pgas_bench}"
# Must match hardcoded name in main_server.cc (ShmCommunicationManager("/cxlmemsim_comm"))
RING_SHM="${RING_SHM:-/cxlmemsim_comm}"
TOPO="${TOPO:-${OCEAN_ROOT}/qemu_integration/topology_simple.txt}"
CAPACITY="${CAPACITY:-1024}"

mkdir -p "${OUT_DIR}"

if [[ ! -x "${SERVER}" ]]; then
  echo "[ERROR] Build first: cmake -S ${OCEAN_ROOT} -B ${BUILD_DIR} && cmake --build ${BUILD_DIR} -j"
  exit 1
fi

echo "Building benchmarks..."
cmake --build "${BUILD_DIR}" -j"$(nproc)" --target cxl_transport_latency_bench ring_shm_latency_bench

stop_server() {
  pkill -f "cxlmemsim_server.*comm-mode" 2>/dev/null || true
  sleep 1
}

stop_server
export SPDLOG_LEVEL=warn

echo "=== [1/3] TCP (--comm-mode tcp, port ${PORT}) ==="
nohup "${SERVER}" --capacity="${CAPACITY}" --comm-mode tcp --port "${PORT}" \
  -t "${TOPO}" > "${OUT_DIR}/server_tcp.log" 2>&1 &
for _ in $(seq 1 50); do
  if ss -ltn 2>/dev/null | grep -q ":${PORT} "; then break; fi
  sleep 0.2
done
"${BENCH_C}" --mode tcp --host 127.0.0.1 --port "${PORT}" \
  --samples "${SAMPLES}" --warmup "${WARMUP}" \
  --out "${OUT_DIR}/tcp_rtt.csv"
stop_server

echo "=== [2/3] PGAS-SHM (--comm-mode pgas-shm) ==="
shm_unlink "${PGAS_SHM}" 2>/dev/null || true
nohup "${SERVER}" --capacity="${CAPACITY}" --comm-mode pgas-shm --pgas-shm-name "${PGAS_SHM}" \
  -t "${TOPO}" > "${OUT_DIR}/server_pgas.log" 2>&1 &
for _ in $(seq 1 50); do
  if [[ -e "/dev/shm${PGAS_SHM}" ]] && grep -q "server_ready\|PGAS shared memory initialized" "${OUT_DIR}/server_pgas.log" 2>/dev/null; then
    break
  fi
  sleep 0.2
done
sleep 0.5
"${BENCH_C}" --mode pgas-shm --shm "${PGAS_SHM}" \
  --samples "${SAMPLES}" --warmup "${WARMUP}" \
  --out "${OUT_DIR}/pgas_rtt.csv"
stop_server

echo "=== [3/3] Ring-buffer SHM (--comm-mode shm) ==="
shm_unlink "${RING_SHM}" 2>/dev/null || true
nohup "${SERVER}" --capacity="${CAPACITY}" --comm-mode shm \
  -t "${TOPO}" > "${OUT_DIR}/server_ring.log" 2>&1 &
for _ in $(seq 1 50); do
  if [[ -e "/dev/shm${RING_SHM}" ]] && grep -q "SHM server initialized\|Shared memory communication" "${OUT_DIR}/server_ring.log" 2>/dev/null; then
    break
  fi
  sleep 0.2
done
sleep 0.5
"${BENCH_RING}" --shm "${RING_SHM}" \
  --samples "${SAMPLES}" --warmup "${WARMUP}" \
  --out "${OUT_DIR}/ring_rtt.csv"
stop_server

for f in tcp_rtt pgas_rtt ring_rtt; do
  n=$(($(wc -l < "${OUT_DIR}/${f}.csv") - 1))
  if [[ "${n}" -lt 100 ]]; then
    echo "[ERROR] Too few samples in ${f}.csv (${n})"
    exit 1
  fi
done

if [[ -f "${OCEAN_ROOT}/.venv/bin/activate" ]]; then
  # shellcheck disable=SC1091
  source "${OCEAN_ROOT}/.venv/bin/activate"
fi

python3 "${OCEAN_ROOT}/script/summarize_transport_compare.py" \
  --tcp "${OUT_DIR}/tcp_rtt.csv" \
  --pgas "${OUT_DIR}/pgas_rtt.csv" \
  --ring "${OUT_DIR}/ring_rtt.csv" \
  --out-md "${OUT_DIR}/summary.md" \
  --out-csv "${OUT_DIR}/summary.csv"

if python3 -c "import matplotlib" 2>/dev/null; then
  python3 "${OCEAN_ROOT}/script/plot_transport_compare.py" \
    --tcp "${OUT_DIR}/tcp_rtt.csv" \
    --pgas "${OUT_DIR}/pgas_rtt.csv" \
    --ring "${OUT_DIR}/ring_rtt.csv" \
    --out "${OUT_DIR}/latency_cdf.png" || true
fi

echo ""
echo "Done. Results in ${OUT_DIR}/"
echo "  summary.md   — comparison table"
echo "  summary.csv"
echo "  *_rtt.csv    — raw samples"
echo "  server_*.log"
