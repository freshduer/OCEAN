# 03 — 正式实验

> **状态：暂缓**  
> 在 [02-motivation.md](./02-motivation.md) 的 **M0/M1/M2** 完成并写入 `results/motivation/summary.md` 之前，**不跑**本文件中的任何实验。

---

## 前置（开启 03 时）

- [02-motivation.md](./02-motivation.md) 已交付：CXL vs TCP、读 p99 长尾、reader/update **争用 + BI**
- `embedding_bench` / `embedding_writer` 已在 2-host 跑通
- 1GB CXL 环境仍满足 [01-setup.md](./01-setup.md)

**结束标志（03）**：`results/experiments/p99_summary.md` + G0/G1 对比图。

---

## §1 Task1 补全（2-host + update pipeline）

- [ ] Update pipeline 三阶段记时：`net → assemble → cxl_write` → `update_pipeline.csv`
- [ ] 与 02 M0 衔接：RDMA stub vs CXL L3 端到端（若 02 仅 host transport，此处补 guest 路径）
- [ ] 产出 → `results/experiments/task1/`

---

## §2 Task2 — G0 vs G1（核心）

| 组别 | 行为 |
|------|------|
| **G0** | 每写 → 硬件 BI（02 M2 已观察） |
| **G1** | delta 末广播 `dirty_set` → ack → `version++`；batch 内写不 BI |

负载与 02 B3 相同：**Criteo trace + L1/L2**（见 `script/run_motivation.sh`）。

### 实现

- [ ] `embedding_writer`：delta batch + `dirty_set`
- [ ] Server：`--coherence-mode version`（G1）
- [ ] 指标：`bi_per_delta`、`broadcast_rounds`、p50/p99/p999

### 矩阵

| 写模式 | G0 | G1 |
|--------|----|----|
| W-hot | 跑 | 跑 |
| W-cold | 跑 | 跑 |

### 验收

- [ ] G1 `back_invalidations` ≪ G0
- [ ] G1 每 delta **1** 次广播
- [ ] W-hot：G1 p99 ≤ G0

```text
results/experiments/
├── task1/
├── task2_G0_*.csv
├── task2_G1_*.csv
└── p99_summary.md
```
