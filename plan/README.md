# 实验计划（Motivation 阶段）

## 目标（对齐 proposal）

在 **2-host** 模拟 CXL 池上说明两件事（**不跑 03 正式实验**）：

1. **2-host TCP CXL 池**：大块 IO 用 **bulk RPC** 达到 **数百 MiB/s**；embedding 冷读/一致性仍经 Guest **64B cacheline RPC**（看延迟与 BI，不是 bulk 带宽）。
2. **Inference reader 与 update agent 争用同一 CXL 池**：并发 delta 写入时，读延迟长尾上升，且硬件 **back-invalidation** 明显增多（为 Task2 软件一致性埋伏笔）。

```text
01-setup.md                 2-host + CXL（motivation 默认 **10GB 满池**）
    ↓
02-motivation.md            本阶段交付（见下）
    ↓
03-experiments.md           【暂缓】G0/G1、完整 Task1/2 矩阵
```

| 步骤 | 文件 | 产出 |
|------|------|------|
| 环境 | [01-setup.md](./01-setup.md) | `verify_2host_cxl.sh` 通过 |
| **Motivation** | [02-motivation.md](./02-motivation.md) | `results/motivation/summary.md` |
| 正式实验 | [03-experiments.md](./03-experiments.md) | 待 motivation 写完再执行 |

---

## 固定参数（本机 10 GiB 逻辑表 + **10 GiB 满池** + 小 trace + TCP）

| 项 | 值 | 说明 |
|----|-----|------|
| **逻辑 EMT** | **`--table-gib 10`**，`--dim 128` | ≈2.1×10⁷ 行；trace 仅 row_id |
| **CXL 池** | **`--capacity=10240` MiB** | **与逻辑表同大**，全表 backing |
| **Trace** | `--queries 10000000` | **≈40 MiB** |
| **每 host L1 / L2** | **1 / 5 GiB** | Criteo motivation 默认 |
| **Trace** | Criteo Kaggle npz | `script/extract_criteo_trace.py`；26 lookup/广告 |
| **CXL 传输** | **TCP**（+ bulk 带宽 RPC） | 见下表 |
| 拓扑 | 2 VM + 共享池 | VM0 update agent，VM1 reader |
| Lookup | `row_id` → L1 → L2 → L3 | 详见 [02-motivation.md](./02-motivation.md) |

> Motivation trace：**Criteo**（`bash script/run_motivation.sh`），见 [02-motivation.md](./02-motivation.md)。

### 传输栈（host ↔ QEMU）

| 层 | 参数 | 作用 |
|----|------|------|
| **cxlmemsim_server** | `--comm-mode tcp --port 9999` | Host 池 + cacheline/bulk RPC |
| **QEMU guest** | `CXL_TRANSPORT_MODE=tcp` | 与 pipeline 默认一致 |
| **带宽压测** | `script/run_2host_rw_tcp_bench.sh` | `RW_BENCH_MODE=bulk`，目标 **数百 MiB/s** |
| **Guest lookup** | cacheline READ/WRITE | 每次 embedding 访问 ≈ 64B RPC（~µs 级，非 bulk） |

---

## Motivation 要证明什么

| # | 观察 | 怎么证 | 产出 |
|---|------|--------|------|
| **M0** | TCP **bulk** 带宽 **≥ 数百 MiB/s** | `run_2host_rw_tcp_bench.sh`；对照 `RW_BENCH_MODE=cacheline` ~1 MiB/s | `rw_bench.json` |
| **M1** | 80/20 + 小 cache → 冷 miss → **读 p99 长尾** | VM1 reader，`--phase run`，小 L1/L2 | `latency_cdf_read.png`、`p99_read.csv` |
| **M2** | Update agent 写入时与 reader **争用 CXL + BI 风暴** | VM0 持续写 delta；VM1 同时读；对比 idle 写 | `bi_storm.txt`、写期间 p99↑、`server` 带宽/BI 日志 |

> **M0** 对应 proposal「相对 RDMA PS 更低延迟」；**M2** 对应「update 与 inference 竞争 + 硬件 BI 开销」。**M1** 说明为何需要 CXL 承载冷数据（长尾来自冷路径，不是平均延迟）。

---

## 验收顺序（先看访问 CDF，再看 miss 是否「跟着偏斜走」）

**两步门禁**，不可跳步：

1. **访问 CDF 过关**（E + B1）：extract 与 replay 的 access CDF 一致（`max_cdf_gap≤0.01`）。  
2. **Miss 与访问一致**（B2）：在**同一 trace**、小 cache 下，L3 miss **主要集中在冷行/冷请求**，热行以 L1/L2 为主——形态上要能由 ② 推导，而不是均匀随机 miss。

| 类型 | 含义 | 阶段 | 产出 |
|------|------|------|------|
| **① 目标访问 CDF** | extract 时统计 | **E** | `target_access_cdf.png` |
| **② 实测访问 CDF** | 对 `read_trace.bin` 统计的 CCDF | **B1** | `access_skew_cdf.png`（**必须先过**） |
| **③a Cache 层级 CDF** | 每次 lookup 的 tier（L1/L2/L3）CCDF | **B2** | `cache_tier_cdf.png` |
| **③b Miss–访问一致性** | 按行热度分桶的 L3 miss 率 / L3 miss 质量占比 | **B2** | `miss_vs_access_cdf.png` |
| **④ 读延迟 CDF** | 端到端延迟（可按 tier 分色） | **B3** | `latency_cdf_read.png` |
| **TCP bulk 带宽**（M0） | read/write MiB/s | **C** | `rw_bench.json` |

```text
E   extract_criteo_trace → ① target_access_cdf.png
    ↓ 【门禁 1】① 形状 OK；B1 确认 ②≈①
B1  replay-access → ② access_skew_cdf.png
    ↓ 【门禁 1】② ≈ ①，不过则停止，不跑 B2
B2  replay-cache（小 cache）→ ③a cache_tier_cdf + ③b miss_vs_access_cdf
    ↓ 【门禁 2】miss 形态符合 ②（见 02 定量条件）
A   init EMT on CXL（可与 B1/B2 并行，但 B3 前必须完成）
C   M0 传输（可选，与 trace 无关）
B3  2-host replay-run → ④ + BI
```

---

## 跑在哪台 VM

| 子步 | VM |
|------|-----|
| B1、B2（路径/skew 验收） | **仅 VM1**（迭代快） |
| C（传输 M0） | **host** 或 VM1（见 02） |
| B3（M1+M2 竞争） | **VM0 writer + VM1 reader** |

---

## 结果目录

```text
results/setup/
results/motivation/
  trace/                   # read_trace.bin, trace_meta.json, target_access_cdf.png ①
  access_skew_cdf.png      # ②（须与 ① 对比）
  cache_tier_cdf.png       # ③a
  miss_vs_access_cdf.png   # ③b（miss 是否随访问偏斜）
  latency_cdf_read.png     # ④
  cache_miss_report.txt
  summary.md               # 门禁 1/2 通过与否
results/experiments/   # 【暂缓】03 再用
```
