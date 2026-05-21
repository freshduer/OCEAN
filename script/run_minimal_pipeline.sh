#!/bin/bash
# Minimal smoke test: build + server + TCP client (qemu_integration test_cxl_mem).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

# Activate uv venv if present
if [ -f "$ROOT/.venv/bin/activate" ]; then
  # shellcheck disable=SC1091
  source "$ROOT/.venv/bin/activate"
fi

BUILD_DIR="${ROOT}/build"
QEMU_BUILD="${ROOT}/qemu_integration/build"

mkdir -p "$BUILD_DIR"
if [ ! -f "$BUILD_DIR/cxlmemsim_server" ]; then
  echo "=== Configure & build cxlmemsim_server ==="
  cmake -S . -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="${CXX:-g++-13}"
  cmake --build "$BUILD_DIR" -j"$(nproc)"
fi

mkdir -p "$QEMU_BUILD"
if [ ! -f "$QEMU_BUILD/cxlmemsim_server" ] || [ ! -f "$QEMU_BUILD/test_cxl_mem" ]; then
  echo "=== Configure & build qemu_integration ==="
  cmake -S qemu_integration -B "$QEMU_BUILD" -DCMAKE_BUILD_TYPE=Release
  cmake --build "$QEMU_BUILD" -j"$(nproc)"
fi

PORT=19999
LOG="${ROOT}/pipeline_smoke.log"
: >"$LOG"

echo "=== Start main cxlmemsim_server (shared-memory mode) ==="
# Avoid conda/anaconda libstdc++ shadowing system libs (GLIBCXX mismatch)
env -u LD_LIBRARY_PATH "$BUILD_DIR/cxlmemsim_server" --port "$PORT" --capacity 64 >>"$LOG" 2>&1 &
SERVER_PID=$!
trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT
sleep 1

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo "Server failed to start. Log:"
  cat "$LOG"
  exit 1
fi
echo "Server PID $SERVER_PID running on port $PORT"

echo "=== Run use_cases unit tests (mock CXLMemSim) ==="
if command -v python3 >/dev/null 2>&1; then
  python3 -m pytest use_cases/test_use_cases.py -q --tb=line 2>&1 | tee -a "$LOG"
fi

echo "=== Note: qemu_integration/test_cxl_mem needs /dev/dax0.0 (QEMU VM) ==="
if [ -e /dev/dax0.0 ]; then
  "$QEMU_BUILD/test_cxl_mem" /dev/dax0.0 2>&1 | tee -a "$LOG"
else
  echo "SKIP test_cxl_mem (no /dev/dax0.0 on this host)" | tee -a "$LOG"
fi

echo ""
echo "=== Pipeline smoke test PASSED ==="
echo "Log: $LOG"
echo "Binaries:"
echo "  $BUILD_DIR/cxlmemsim_server"
echo "  $QEMU_BUILD/cxlmemsim_server"
echo "  $QEMU_BUILD/test_cxl_mem"
