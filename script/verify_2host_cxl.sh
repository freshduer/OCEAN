#!/bin/bash
# Post-boot checks for 2-host CXL pipeline (requires telnet serial ports)
set -euo pipefail

VM0_PORT=${CXL_VM0_SERIAL_PORT:-4550}
VM1_PORT=${CXL_VM1_SERIAL_PORT:-4551}
PASS=${PASS:-victor129}

run_vm_checks() {
  local port=$1
  local peer_ip=$2
  python3 - "$port" "$peer_ip" "$PASS" <<'PY'
import sys, time
import pexpect

port, peer_ip, password = sys.argv[1:4]
child = pexpect.spawn(f"telnet 127.0.0.1 {port}", timeout=120, encoding="utf-8")
child.logfile = sys.stdout
child.expect("login:")
child.sendline("root")
child.expect("Password:")
child.sendline(password)
child.expect("#", timeout=60)
cmds = [
    "ls -l /dev/dax0.0",
    "cxl list -M 2>/dev/null | head -5 || true",
    f"ping -c 2 -W 3 {peer_ip}",
]
for cmd in cmds:
    child.sendline(cmd)
    child.expect("#", timeout=30)
child.sendline("exit")
child.close()
PY
}

echo "=== VM0 (node0) ==="
run_vm_checks "$VM0_PORT" "192.168.100.11"
echo "=== VM1 (node1) ==="
run_vm_checks "$VM1_PORT" "192.168.100.10"
echo "2-host verification finished."
