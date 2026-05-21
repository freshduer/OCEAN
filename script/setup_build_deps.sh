#!/bin/bash
# Minimal host deps for building/running OCEAN (no QEMU rebuild).
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

sudo apt-get update
sudo apt-get install -y \
  build-essential \
  cmake \
  ninja-build \
  git \
  libspdlog-dev \
  libcxxopts-dev \
  libboost-dev \
  libfmt-dev \
  librdmacm-dev \
  libbpf-dev \
  llvm-dev \
  libclang-dev \
  linux-headers-generic \
  python3-pip

# gcc-13 (README recommends g++-13)
if ! command -v g++-13 >/dev/null 2>&1; then
  sudo apt-get install -y software-properties-common
  sudo add-apt-repository -y ppa:ubuntu-toolchain-r/test || true
  sudo apt-get update
  sudo apt-get install -y gcc-13 g++-13
fi

echo "Build deps OK: $(g++-13 --version 2>/dev/null || g++ --version | head -1)"
echo ""
echo "Tip: build without conda in PATH to avoid libstdc++/libfmt mismatch:"
echo "  env -i HOME=\$HOME PATH=/usr/local/bin:/usr/bin:/bin cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-13"
