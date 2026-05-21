#!/usr/bin/env bash
# Criteo DLRM trace extract + B1/B2. See plan/02-motivation.md
set -euo pipefail

OCEAN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${OCEAN_ROOT}"

BENCH="${OCEAN_ROOT}/build/microbench/embedding_bench"
OUT="${OCEAN_ROOT}/results/motivation"
TRACE_DIR="${OUT}/trace"
NPZ="${CRITEO_NPZ:-${HOME}/downloads/kaggleAdDisplayChallenge_processed.npz}"

TABLE_GIB="${TABLE_GIB:-10}"
QUERIES="${QUERIES:-10000000}"
L1_GIB="${L1_GIB:-1}"
L2_GIB="${L2_GIB:-5}"
CXL_CAPACITY_MB=$((TABLE_GIB * 1024))

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build --target embedding_bench -j "$(nproc)"
mkdir -p "${TRACE_DIR}"

echo "=== Extract trace (Criteo, 26 embedding lookups / ad row) ==="
echo "  npz=${NPZ} Q=${QUERIES} table=${TABLE_GIB}GiB L1=${L1_GIB}GiB L2=${L2_GIB}GiB"
python3 script/extract_criteo_trace.py \
  --npz "${NPZ}" \
  --out-dir "${TRACE_DIR}" \
  --table-gib "${TABLE_GIB}" \
  --queries "${QUERIES}" \
  --l1-gib "${L1_GIB}" \
  --l2-gib "${L2_GIB}"

echo "=== B1: replay-access ==="
set +e
"${BENCH}" --phase replay-access \
  --table-gib "${TABLE_GIB}" --dim 128 \
  --trace-in "${TRACE_DIR}/read_trace.bin" \
  --out-dir "${OUT}" \
  --dump-access-cdf \
  --overlay-target "${TRACE_DIR}/target_access_cdf.csv"
G1=$?
set -e

echo "=== B2a: replay-cache unlimited ==="
set +e
"${BENCH}" --phase replay-cache \
  --trace-in "${TRACE_DIR}/read_trace.bin" \
  --table-gib "${TABLE_GIB}" --dim 128 \
  --cache-unlimited --out-dir "${OUT}" --dump-cache-cdf
G2A=$?
set -e

echo "=== B2b: replay-cache L1=${L1_GIB}GiB L2=${L2_GIB}GiB ==="
set +e
"${BENCH}" --phase replay-cache \
  --trace-in "${TRACE_DIR}/read_trace.bin" \
  --table-gib "${TABLE_GIB}" --dim 128 \
  --l1-gib "${L1_GIB}" --l2-gib "${L2_GIB}" \
  --dump-cache-cdf --dump-miss-vs-access \
  --out-dir "${OUT}"
G2B=$?
set -e

echo ""
echo "Server: --capacity=${CXL_CAPACITY_MB} (full ${TABLE_GIB}GiB pool)"
cat "${OUT}/access_skew_report.txt" "${OUT}/cache_miss_report.txt" 2>/dev/null || true
echo "Gate summary: G1=${G1} G2a=${G2A} G2b=${G2B} (0=pass)"
exit $(( G1 != 0 || G2A != 0 || G2B != 0 ))
