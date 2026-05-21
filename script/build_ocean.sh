#!/bin/bash
# Build C++ targets without picking up conda/anaconda libraries.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:${HOME}/.local/bin:${PATH:-}"

CXX="${CXX:-g++-13}"
if ! command -v "$CXX" >/dev/null 2>&1; then
  CXX=g++
fi

mkdir -p build
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_PREFIX_PATH=/usr

cmake --build build -j"$(nproc)"
echo "Built: $ROOT/build/cxlmemsim_server"
