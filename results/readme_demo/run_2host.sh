#!/bin/bash
# README-style 2-host bring-up (matches exp0: pgas-shm server + SHM transport QEMU).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LOGDIR="$ROOT/results/readme_demo"
QEMU_DIR="$ROOT/qemu_integration"

mkdir -p "$LOGDIR"

echo "[1/4] Stop old processes..."
sudo killall qemu-system-x86_64 2>/dev/null || true
pkill -f cxlmemsim_server 2>/dev/null || true
sleep 2

echo "[2/4] Start cxlmemsim_server (pgas-shm, 1024MB)..."
: > "$LOGDIR/server.log"
"$ROOT/build/cxlmemsim_server" \
  --comm-mode pgas-shm \
  --capacity=1024 \
  -t "$QEMU_DIR/topology_simple.txt" \
  >> "$LOGDIR/server.log" 2>&1 &
echo "  server pid=$!"
# Wait for PGAS shm init; starting QEMU too early can crash the server.
sleep 8
if ! pgrep -f cxlmemsim_server >/dev/null; then
  echo "ERROR: server failed to start. See $LOGDIR/server.log"
  tail -30 "$LOGDIR/server.log"
  exit 1
fi

echo "[3/4] Launch QEMU node0 + node1 (sudo for KVM)..."
: > "$LOGDIR/qemu_node0.log"
: > "$LOGDIR/qemu_node1.log"
sudo bash -c "cd '$QEMU_DIR' && nohup bash '$LOGDIR/launch_qemu_cxl.sh' >> '$LOGDIR/qemu_node0.log' 2>&1 &"
sleep 15
if ! pgrep -f cxlmemsim_server >/dev/null; then
  echo "ERROR: server died after node0 QEMU start"
  tail -20 "$LOGDIR/server.log"
  exit 1
fi
sudo bash -c "cd '$QEMU_DIR' && nohup bash '$LOGDIR/launch_qemu_cxl1.sh' >> '$LOGDIR/qemu_node1.log' 2>&1 &"
sleep 5
if ! pgrep -f cxlmemsim_server >/dev/null; then
  echo "WARN: server died after node1 QEMU start; restart manually:"
  echo "  $ROOT/build/cxlmemsim_server --comm-mode pgas-shm --capacity=1024 -t $QEMU_DIR/topology_simple.txt >> $LOGDIR/server.log 2>&1 &"
fi

echo "[4/4] Status"
pgrep -a cxlmemsim_server || true
pgrep -a qemu-system | head -2 || true
echo ""
echo "Logs:"
echo "  server:    tail -f $LOGDIR/server.log"
echo "  node0:     tail -f $LOGDIR/qemu_node0.log"
echo "  node1:     tail -f $LOGDIR/qemu_node1.log"
echo "  serial:    telnet 127.0.0.1 4550   # node0"
echo "             telnet 127.0.0.1 4551   # node1"
echo "  login:     root / victor129"
echo ""
echo "In VM after boot: ls -l /dev/dax0.0"
