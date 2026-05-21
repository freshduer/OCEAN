# 01 — 搭建环境

**目标**：本机 2-host（双 QEMU）+ CXLMemSim（**pgas-shm**）。  
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

- [ ] **启动 2-host pipeline**（PGAS-SHM；motivation 见下「10GB 池」）
  ```bash
  bash script/run_2host_cxl_pipeline.sh
  ```
  脚本默认：
  - server：`--comm-mode pgas-shm --pgas-shm-name /cxlmemsim_pgas`
  - QEMU：`CXL_TRANSPORT_MODE=shm`（经 `sudo -E` 传入 launch 脚本）

  若需手动起 server（**勿用 tcp 跑 motivation**）：
  ```bash
  shm_unlink /cxlmemsim_pgas 2>/dev/null || true
  export CXL_TRANSPORT_MODE=shm
  # Motivation（10 GiB 逻辑表 = 10 GiB 池）：
  ./build/cxlmemsim_server --capacity=10240 --comm-mode pgas-shm \
    --pgas-shm-name /cxlmemsim_pgas &
  # Smoke / 旧 pipeline 可用 --capacity=1024
  ```
  验收 server：`grep "PGAS shared memory" logs/2host/cxlmemsim_server.log`（**不依赖** `:9999` 监听）。

  等待 2–3 分钟。

- [ ] **验收**
  ```bash
  bash script/verify_2host_cxl.sh
  ```

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
