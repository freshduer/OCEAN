#!/bin/bash
# Compare client RTT latency CDF: TCP vs PGAS-SHM (CXL shared-memory path).
set -euo pipefail

OCEAN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${OCEAN_ROOT}/build"
BENCH="${BUILD_DIR}/microbench/cxl_transport_latency_bench"
SERVER="${BUILD_DIR}/cxlmemsim_server"
OUT_DIR="${OCEAN_ROOT}/results/latency_cdf"
SAMPLES="${SAMPLES:-20000}"
WARMUP="${WARMUP:-2000}"
PGAS_SHM="${PGAS_SHM:-/cxlmemsim_pgas_latency}"
PORT="${PORT:-9999}"

mkdir -p "${OUT_DIR}"

if [[ ! -x "${SERVER}" ]]; then
  echo "[ERROR] Build first: bash ${OCEAN_ROOT}/script/build_ocean.sh"
  exit 1
fi

if [[ ! -x "${BENCH}" ]]; then
  echo "Building latency benchmark..."
  cmake --build "${BUILD_DIR}" -j"$(nproc)" --target cxl_transport_latency_bench
fi

stop_server() {
  pkill -f "${SERVER}" 2>/dev/null || true
  sleep 1
}

stop_server

echo "=== TCP mode (QEMU CXL_TRANSPORT_MODE=tcp path) ==="
nohup "${SERVER}" --capacity=1024 --port "${PORT}" --comm-mode tcp \
  > "${OUT_DIR}/server_tcp.log" 2>&1 &
sleep 2
"${BENCH}" --mode tcp --host 127.0.0.1 --port "${PORT}" \
  --samples "${SAMPLES}" --warmup "${WARMUP}" \
  --out "${OUT_DIR}/tcp_rtt.csv"
stop_server

echo "=== PGAS-SHM mode (QEMU CXL_TRANSPORT_MODE=shm path) ==="
shm_unlink "${PGAS_SHM}" 2>/dev/null || true
nohup "${SERVER}" --capacity=1024 --comm-mode pgas-shm --pgas-shm-name "${PGAS_SHM}" \
  > "${OUT_DIR}/server_pgas.log" 2>&1 &
sleep 2
"${BENCH}" --mode pgas-shm --shm "${PGAS_SHM}" \
  --samples "${SAMPLES}" --warmup "${WARMUP}" \
  --out "${OUT_DIR}/pgas_rtt.csv" || true
stop_server

TCP_N=$(($(wc -l < "${OUT_DIR}/tcp_rtt.csv") - 1))
PGAS_N=$(($(wc -l < "${OUT_DIR}/pgas_rtt.csv") - 1))
if [[ "${TCP_N}" -lt 100 || "${PGAS_N}" -lt 100 ]]; then
  echo "[ERROR] Too few samples (tcp=${TCP_N}, pgas=${PGAS_N})"
  exit 1
fi
echo "Samples collected: tcp=${TCP_N}, pgas=${PGAS_N}"

if [[ -f "${OCEAN_ROOT}/.venv/bin/activate" ]]; then
  # shellcheck disable=SC1091
  source "${OCEAN_ROOT}/.venv/bin/activate"
fi

python3 "${OCEAN_ROOT}/script/plot_latency_cdf.py" \
  --tcp "${OUT_DIR}/tcp_rtt.csv" \
  --pgas "${OUT_DIR}/pgas_rtt.csv" \
  --out "${OUT_DIR}/latency_cdf.png"

echo ""
echo "Done."
echo "  CSV:  ${OUT_DIR}/tcp_rtt.csv  ${OUT_DIR}/pgas_rtt.csv"
echo "  Plot: ${OUT_DIR}/latency_cdf.png"
echo "  Logs: ${OUT_DIR}/server_tcp.log  ${OUT_DIR}/server_pgas.log"
