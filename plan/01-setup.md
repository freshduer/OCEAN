# 01 — 搭建环境

**目标**：本机 2-host（双 QEMU）+ CXLMemSim（**TCP**）。  
**完成标志**：`verify_2host_cxl.sh` 通过，双 VM 能访问 `/dev/dax0.0`。

> **Motivation 默认**：[02-motivation.md](./02-motivation.md) 使用 **10 GiB 满池**（`--capacity=10240` MiB），与逻辑表同大；早期 pipeline 脚本里的 **1024 MiB** 仅作 smoke，跑 B3 前请改容量。03 实验 [暂缓](./03-experiments.md)。

---

## Checklist

- [ ] **构建**
  ```bash
  cd /home/wenjun/orion-yuwenjun/OCEAN
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  ```

- [ ] **镜像**：仓库根目录有 `qemu.img`（见仓库 `README.md` 下载说明）

- [ ] **网络**
  ```bash
  bash script/setup_network.sh 2
  ```

- [ ] **启动 2-host pipeline**（TCP；motivation 见下「10GB 池」）
  ```bash
  export CXL_COMM_MODE=tcp
  export CXL_TRANSPORT_MODE=tcp
  bash script/run_2host_cxl_pipeline.sh
  ```
  脚本默认 TCP server（`--comm-mode tcp --port 9999`，topology：`qemu_integration/topology_simple.txt` 为 `(1);`）。  
  QEMU CXL 调试输出在 `logs/2host/vm*_cxl.err`；Guest 串口用 telnet 4550/4551。  
  验收 server：`ss -ltn | grep ':9999'`。

  **快速 smoke（~10s，不启 QEMU）**：`bash script/quick_smoke_tcp.sh`（cacheline + **bulk** RPC）。

  **带宽压测（host，目标数百 MiB/s）**（需 **重编译并重启** server）：
  ```bash
  cmake --build build -j --target cxlmemsim_server
  pkill -f cxlmemsim_server; sleep 1
  RW_BENCH_SEC=20 RW_BULK_BYTES=262144 bash script/run_2host_rw_tcp_bench.sh
  ```
  默认 `RW_BENCH_MODE=bulk`（256KiB/次）；legacy 64B：`RW_BENCH_MODE=cacheline`（约 ~1 MiB/s）。

  首次完整验收约 **6–11 分钟**（Guest `cxl-numa-setup.service` 在 TCP 下很慢；VM0 ~5min、VM1 可达 ~10min）。

- [ ] **验收**
  ```bash
  bash script/verify_2host_cxl.sh 2>&1 | tee logs/2host/verify_run.log
  ```
  log 里 **`[VM0]`/`[VM1]`** 标在 `guest serial begin/end` 与 `[verify]` 摘要行；中间是原始 systemd 串口（含倒计时、`[ OK ]`）。默认等待 660s。安静模式：`CXL_VERIFY_QUIET=1`。

- [ ] **Guest 检查**（串口 `telnet 127.0.0.1 4550` / `4551`，`root` / `victor129`）
  ```bash
  ls -l /dev/dax0.0
  ndctl list -B
  ```

- [ ] **角色约定**（motivation 用）
  - **VM0**：update agent（delta 写入 CXL 池）
  - **VM1**：inference reader（Criteo trace replay，见 02）

- [ ] **归档**：`logs/2host/` → `results/setup/`

---

## 下一步

→ [02-motivation.md](./02-motivation.md)
