#!/bin/bash
# Run 10 MiB read latency comparison: TCP bulk vs PGAS-SHM (requires built cxlmemsim_server).
set -euo pipefail

OCEAN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SERVER="${OCEAN_ROOT}/build/cxlmemsim_server"
TOPO="${OCEAN_TOPOLOGY:-${OCEAN_ROOT}/qemu_integration/topology_simple.txt}"
TCP_PORT="${CXL_MEMSIM_PORT:-9999}"
PGAS_SHM="${PGAS_SHM:-/cxlmemsim_pgas_bench10}"
CAPACITY="${CXL_CAPACITY_MB:-1024}"
OUT="${OCEAN_ROOT}/results/2host_rw_tcp/bench_10mb_compare.json"
SAMPLES="${BENCH_SAMPLES:-12}"
SKIP_TCP_CL="${SKIP_TCP_CACHELINE:-1}"

mkdir -p "${OCEAN_ROOT}/results/2host_rw_tcp" "${OCEAN_ROOT}/logs/2host"

stop_server() {
  pkill -f '[c]xlmemsim_server' 2>/dev/null || true
  sleep 1
}

if [[ ! -x "${SERVER}" ]]; then
  echo "[ERROR] Build: cmake --build ${OCEAN_ROOT}/build -j --target cxlmemsim_server"
  exit 1
fi

stop_server
shm_unlink "${PGAS_SHM}" 2>/dev/null || true

echo "=== [A] TCP server :${TCP_PORT} ==="
nohup "${SERVER}" --capacity="${CAPACITY}" --comm-mode tcp --port "${TCP_PORT}" \
  -t "${TOPO}" > "${OCEAN_ROOT}/logs/2host/bench10_tcp_server.log" 2>&1 &
sleep 2
if ! ss -ltn | grep -q ":${TCP_PORT} "; then
  echo "[FAIL] TCP server not listening"
  exit 1
fi

TCP_ARGS=(--tcp-host 127.0.0.1 --tcp-port "${TCP_PORT}" --samples "${SAMPLES}" --warmup 2 --out /tmp/bench10_tcp.json)
[[ "${SKIP_TCP_CL}" == "1" ]] && TCP_ARGS+=(--skip-tcp-cl)

python3 "${OCEAN_ROOT}/script/bench_10mb_read_compare.py" "${TCP_ARGS[@]}" | tee "${OCEAN_ROOT}/logs/2host/bench10_tcp_run.log"
TCP_JSON=/tmp/bench10_tcp.json

stop_server
shm_unlink "${PGAS_SHM}" 2>/dev/null || true

echo "=== [B] PGAS-SHM server ${PGAS_SHM} ==="
nohup "${SERVER}" --capacity="${CAPACITY}" --comm-mode pgas-shm --pgas-shm-name "${PGAS_SHM}" \
  -t "${TOPO}" > "${OCEAN_ROOT}/logs/2host/bench10_pgas_server.log" 2>&1 &
for i in $(seq 1 50); do
  if grep -q "server_ready\|PGAS shared memory initialized" "${OCEAN_ROOT}/logs/2host/bench10_pgas_server.log" 2>/dev/null; then
  if [[ -e "/dev/shm${PGAS_SHM}" ]]; then break; fi
  fi
  sleep 0.2
done
sleep 1

python3 "${OCEAN_ROOT}/script/bench_10mb_read_compare.py" \
  --pgas-shm "${PGAS_SHM}" --samples "${SAMPLES}" --warmup 2 --skip-tcp-cl \
  --out /tmp/bench10_pgas.json | tee "${OCEAN_ROOT}/logs/2host/bench10_pgas_run.log"
PGAS_JSON=/tmp/bench10_pgas.json

python3 - "${TCP_JSON}" "${PGAS_JSON}" "${OUT}" <<'PY'
import json, sys
tcp_p, pgas_p, out_p = sys.argv[1:4]
merged = {"transfer_mib": 10}
for label, path in [("tcp_phase", tcp_p), ("pgas_phase", pgas_p)]:
    try:
        with open(path) as f:
            merged[label] = json.load(f)
    except FileNotFoundError:
        merged[label] = {"error": "missing " + path}
# flat summary for tables
m = merged
flat = {
    "transfer_mib": 10,
    "rdma_theory": m.get("tcp_phase", {}).get("rdma_theory") or m.get("pgas_phase", {}).get("rdma_theory"),
}
for phase in ("tcp_phase", "pgas_phase"):
    meas = m.get(phase, {}).get("measurements", {})
    for k, v in meas.items():
        flat[f"{phase}.{k}"] = v
with open(out_p, "w") as f:
    json.dump({"merged": merged, "summary": flat}, f, indent=2)
print("Wrote", out_p)
PY

stop_server
echo "Done. Results: ${OUT}"
