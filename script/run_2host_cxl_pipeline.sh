#!/bin/bash
# OCEAN 2-host CXL pipeline: server + dual QEMU VMs
set -euo pipefail

OCEAN_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QEMU_DIR="${OCEAN_ROOT}/qemu_integration"
BUILD_DIR="${OCEAN_ROOT}/build"
LOG_DIR="${OCEAN_ROOT}/logs/2host"
mkdir -p "$LOG_DIR"

QEMU_BIN="${QEMU_BIN:-/usr/local/bin/qemu-system-x86_64}"
if [[ ! -x "$QEMU_BIN" ]]; then
  QEMU_BIN="${OCEAN_ROOT}/library/qemu/build/qemu-system-x86_64"
  sudo ln -sf "$QEMU_BIN" /usr/local/bin/qemu-system-x86_64
fi

if [[ ! -f "${OCEAN_ROOT}/qemu.img" ]]; then
  echo "[ERROR] ${OCEAN_ROOT}/qemu.img not found. Download per README."
  exit 1
fi

cd "$QEMU_DIR"
if [[ ! -f qemu.img ]] || [[ ! -f qemu1.img ]]; then
  ln -sf "${OCEAN_ROOT}/qemu.img" qemu.img
  cp -f "${OCEAN_ROOT}/qemu.img" qemu1.img
fi

# Network (idempotent)
if ! ip link show br0 &>/dev/null; then
  sudo bash "${OCEAN_ROOT}/script/setup_network.sh" 2
else
  for i in 0 1; do
    ip link show "tap$i" &>/dev/null || sudo ip tuntap add "tap$i" mode tap
    sudo ip link set "tap$i" up 2>/dev/null || true
    sudo ip link set "tap$i" master br0 2>/dev/null || true
  done
fi

# Shared memory backends
truncate -s 1G /dev/shm/lsa1.raw 2>/dev/null || true

# CXL path: PGAS-SHM (fast) — pairs with QEMU CXL_TRANSPORT_MODE=shm
PGAS_SHM="${PGAS_SHM:-/cxlmemsim_pgas}"
export CXL_TRANSPORT_MODE="${CXL_TRANSPORT_MODE:-shm}"

if ! pgrep -f '[c]xlmemsim_server' >/dev/null; then
  cd "$BUILD_DIR"
  shm_unlink "${PGAS_SHM}" 2>/dev/null || true
  nohup ./cxlmemsim_server --capacity=1024 --comm-mode pgas-shm \
    --pgas-shm-name "${PGAS_SHM}" \
    > "${LOG_DIR}/cxlmemsim_server.log" 2>&1 &
  sleep 2
fi

if ! pgrep -f '[c]xlmemsim_server' >/dev/null; then
  echo "[ERROR] cxlmemsim_server failed to start (see ${LOG_DIR}/cxlmemsim_server.log)"
  exit 1
fi
if ! grep -q "PGAS shared memory" "${LOG_DIR}/cxlmemsim_server.log" 2>/dev/null; then
  echo "[WARN] Server log may not show PGAS mode yet; check ${LOG_DIR}/cxlmemsim_server.log"
fi

# Stop stale QEMU
sudo pkill -f 'launch_qemu_cxl' 2>/dev/null || true
sudo pkill -f 'qemu-system-x86_64.*cxl=on' 2>/dev/null || true
sleep 2

cd "$QEMU_DIR"
nohup sudo -E bash ./launch_qemu_cxl.sh \
  > "${LOG_DIR}/vm0_serial.log" 2>&1 &
sleep 3
nohup sudo -E bash ./launch_qemu_cxl1.sh \
  > "${LOG_DIR}/vm1_serial.log" 2>&1 &

echo "Started 2-host pipeline (CXL_TRANSPORT_MODE=${CXL_TRANSPORT_MODE}, server comm-mode=pgas-shm)."
echo "  Server log: ${LOG_DIR}/cxlmemsim_server.log"
echo "  VM0 log:    ${LOG_DIR}/vm0_serial.log"
echo "  VM1 log:    ${LOG_DIR}/vm1_serial.log"
echo ""
echo "VM serial consoles (telnet):"
echo "  telnet 127.0.0.1 4550   # node0"
echo "  telnet 127.0.0.1 4551   # node1"
echo ""
echo "Login: root / victor129"
echo "After boot (~2-3 min), run: bash ${OCEAN_ROOT}/script/verify_2host_cxl.sh"
