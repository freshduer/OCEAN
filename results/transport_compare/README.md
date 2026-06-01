# Transport latency comparison (TCP / PGAS-SHM / ring-buffer SHM)

## Run

```bash
# Default: 20000 samples, 2000 warmup
bash script/run_transport_compare.sh

# Faster smoke test
SAMPLES=5000 WARMUP=500 bash script/run_transport_compare.sh
```

Requires: built `cxlmemsim_server`, `microbench/cxl_transport_latency_bench`, `microbench/ring_shm_latency_bench`.

## Outputs

| File | Description |
|------|-------------|
| `summary.md` / `summary.csv` | p50/p90/p99 RTT table (µs) |
| `latency_cdf.png` | CDF plot (needs matplotlib) |
| `tcp_rtt.csv` / `pgas_rtt.csv` / `ring_rtt.csv` | Per-request RTT samples |
| `server_*.log` | Server logs per mode |

## Modes

- **TCP**: `--comm-mode tcp` — socket RPC, full `handle_request` (MESI/BI).
- **PGAS-SHM**: `--comm-mode pgas-shm` — QEMU default path, slot + PGAS pool.
- **Ring-SHM**: `--comm-mode shm` — `/dev/shm/cxlmemsim_comm` ring IPC, same handler as TCP.

Benchmark: 64-byte READ per sample, client-measured round-trip time.
