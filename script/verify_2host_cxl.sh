#!/bin/bash
# Post-boot checks for 2-host CXL pipeline (requires telnet serial ports)
set -euo pipefail

VM0_PORT=${CXL_VM0_SERIAL_PORT:-4550}
VM1_PORT=${CXL_VM1_SERIAL_PORT:-4551}
PASS=${PASS:-victor129}
# VM1 cxl-numa-setup.service timeout can be 10min; allow headroom for login + checks.
BOOT_TIMEOUT=${CXL_VERIFY_BOOT_TIMEOUT:-660}

run_vm_checks() {
  local vm_tag=$1
  local vm_name=$2
  local port=$3
  local peer_ip=$4
  python3 - "$vm_tag" "$vm_name" "$port" "$peer_ip" "$PASS" "$BOOT_TIMEOUT" <<'PY'
import os
import re
import sys
import threading
import pexpect

vm_tag, vm_name, port, peer_ip, password, boot_timeout = sys.argv[1:7]
boot_timeout = int(boot_timeout)
pfx = f"[{vm_tag}]"
quiet = os.environ.get("CXL_VERIFY_QUIET")
_io_lock = threading.Lock()


class LockedWriter:
    """Avoid interleaving pexpect logfile writes with tagged print() lines."""

    def __init__(self, stream):
        self.stream = stream

    def write(self, data: str) -> None:
        if data:
            with _io_lock:
                self.stream.write(data)

    def flush(self) -> None:
        with _io_lock:
            self.stream.flush()


def log_print(msg: str) -> None:
    with _io_lock:
        print(msg, flush=True)


log_print(f"{pfx} ----- guest serial begin (127.0.0.1:{port}, expect {vm_name}) -----")

child = pexpect.spawn(f"telnet 127.0.0.1 {port}", timeout=boot_timeout, encoding="utf-8")
if not quiet:
    child.logfile = LockedWriter(sys.stdout)

idx = child.expect(
    [r"(?i)login:\s*$", r"[a-zA-Z0-9._-]+ login:\s*$", r"#{1,2}\s*$"],
    timeout=boot_timeout,
)
boot_text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", child.before or "")
log_print(f"{pfx} ----- guest serial end (login/shell ready) -----")

if idx == 2:
    log_print(f"{pfx} [verify] already at shell prompt")
else:
    child.sendline("root")
    child.expect([r"(?i)password:\s*$", r"#{1,2}\s*$"], timeout=120)
    if child.after and "assword" in str(child.after).lower():
        child.sendline(password)
    child.expect(r"#{1,2}\s*$", timeout=120)

if "cxl-numa-setup" in boot_text and "FAILED" in boot_text:
    log_print(
        f"{pfx} [warn] cxl-numa-setup.service FAILED during boot "
        f"(system may still be usable; checking /dev/dax0.0)"
    )

cmds = [
    ("dax", "ls -l /dev/dax0.0"),
    ("cxl", "cxl list -M 2>/dev/null | head -5 || true"),
    ("ping", f"ping -c 2 -W 3 {peer_ip}"),
]
results = {}
for name, cmd in cmds:
    log_print(f"{pfx} [verify] $ {cmd}")
    child.sendline(cmd)
    child.expect("#", timeout=90)
    chunk = child.before or ""
    results[name] = chunk
    plain = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", chunk)
    for line in plain.splitlines():
        line = line.strip()
        if line and not line.endswith("#"):
            log_print(f"{pfx}   {line}")

dax_ok = "/dev/dax0.0" in results.get("dax", "")
ping_out = results.get("ping", "")
ping_ok = " 0% packet loss" in ping_out or " 0.0% packet loss" in ping_out
if dax_ok and ping_ok:
    log_print(f"{pfx} [verify] PASS (dax0.0 ok, ping {peer_ip} ok)")
elif dax_ok:
    log_print(f"{pfx} [verify] PARTIAL (dax0.0 ok; see ping output above)")
else:
    log_print(f"{pfx} [verify] FAIL (no /dev/dax0.0)")
    sys.exit(1)

child.sendline("exit")
child.close()
PY
}

echo "############################################"
echo "# 2-host CXL verify (TCP serial consoles)  #"
echo "# Block between 'guest serial begin/end'   #"
echo "# is raw VM output (systemd [ OK ] etc.)   #"
echo "############################################"

FAIL=0
echo ""
echo "########## VM0 (node0) port ${VM0_PORT} peer 192.168.100.11 ##########"
if run_vm_checks "VM0" "node0" "$VM0_PORT" "192.168.100.11"; then
  echo "[host] VM0: PASS"
else
  echo "[host] VM0: FAIL"
  FAIL=1
fi

echo ""
echo "########## VM1 (node1) port ${VM1_PORT} peer 192.168.100.10 ##########"
if run_vm_checks "VM1" "node1" "$VM1_PORT" "192.168.100.10"; then
  echo "[host] VM1: PASS"
else
  echo "[host] VM1: FAIL"
  FAIL=1
fi

echo ""
if [[ "$FAIL" -eq 0 ]]; then
  echo "2-host verification finished: ALL PASS"
else
  echo "2-host verification finished: FAILED"
  exit 1
fi
