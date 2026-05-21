# 02 — Motivation（2-host；本机 **10GB** 全表 CXL 池 + 等比例 host cache）

> **本机默认**：逻辑表 **10 GiB**，CXL 仿真池 **同样 10 GiB（完整 backing）**；L1/L2 相对 1TiB 生产配置**等比例缩小**。  
> **论文尺度（仅作对照叙述）**：逻辑 **1 TiB** + **1 GiB 稀疏**仿真池 — 见文末「生产尺度参考」。

**前置**：[01-setup.md](./01-setup.md) 已完成。  
**本文件 = 当前阶段终点**；[03-experiments.md](./03-experiments.md) **暂不执行**。

**要说明的两点**（写进 `summary.md`）：

1. **比 RDMA/远程 PS 快**：正式实验路径为 **SHM**（`pgas-shm` + `CXL_TRANSPORT_MODE=shm`）；M0 用 TCP 仅作对照曲线。
2. **Reader 与 update agent 竞争**：VM0 写 embedding delta 时，VM1 读延迟 **p99 升高**，server **`back_invalidations`** 相对无写阶段明显上升。

```text
E  → extract Criteo trace（26 lookup/广告）→ ① target_access_cdf
B1 → replay-access → ②，【门禁 1】②≈①
B2 → replay-cache → ③a③b，【门禁 2】
A  → init EMT（B3 前）
C  → M0 传输（可选）
B3 → 2-host 竞争 → ④ + BI
```

**完成标志**：`summary.md` + 图 **①②③a③b④**；summary 里写明两道门禁是否通过。

### 两道门禁（核心逻辑）

| 门禁 | 何时 | 看什么 | 不过则 |
|------|------|--------|--------|
| **1 访问 CDF** | E 后 + B1 后 | ① extract CDF；② replay 与 ① 重合，`max_cdf_gap≤0.01` | **禁止 B2/B3** |
| **2 Miss∝访问偏斜** | B2b 后 | ③b：热行 L3 率低、冷行/冷请求 L3 率高；L3 miss 质量大部分来自冷侧 | **禁止 B3**（避免在假 trace 上讲长尾） |

---

## Motivation 工作负载（Criteo Kaggle；DLRM 26 lookup/广告）

数据：`~/downloads/kaggleAdDisplayChallenge_processed.npz`。  
**不**再用合成 Zipf / `gen-trace`。

### 默认参数（`script/run_motivation.sh`）

| 项 | 默认值 | 说明 |
|----|--------|------|
| **逻辑表 / CXL 池** | **10 GiB** | `N_logical≈2.1×10⁷`；`--capacity=10240` MiB |
| **Trace Q** | **10,000,000** | **总 embedding lookup 数**（≈384615 广告 × 26） |
| **L1 / L2** | **1 / 5 GiB** | 写入 `trace_meta.json` |
| **trace 文件** | **≈40 MiB** | `uint32_t row_id[Q]` |

> **Server**：`./build/cxlmemsim_server --capacity=10240 --comm-mode pgas-shm ...`

### Lookup 语义（每次 trace 一步 = 1 次 embedding lookup）

```text
row_id = read_trace[t] → L1 → miss → L2 → miss → L3 读 CXL（池内 row_id 槽位）
                              hit        hit
                               └─ 不访问 CXL
```

若实现需要 batch，可增加 `--batch-size`（默认 **1**）：每个 trace 步仍对应 1 个 `row_id`，batch 仅影响 bench 内并发或统计分桶，**不**扩大 trace 文件。

### Cache 模拟（`replay-cache`）

**独占多级 LRU**（`TieredLruCache`）：L1 LRU → miss → L2 LRU → miss → **tier=3**（CXL）并插入 L1；L1 驱逐行落入 L2，L2 满则真正淘汰。

**逻辑表压力（默认开启）**：Criteo trace 实际触达的 slot 远小于 `N_logical`；replay-cache 会把配置的 L1/L2 按 **逻辑表 top-10% 热足迹 / trace 92.6% 热行数** 等比缩小（上限 32×），使 `P(tier2)` 随 L1 缩小而上升、`P(tier3)` 仍主要由冷行贡献（≈5%）。调试旧行为用 `--raw-cache-caps`。

| 参数 | 默认 | 说明 |
|------|------|------|
| `--warmup-queries` | **Q/2**（默认） | 单遍流式：前半 warmup LRU，后半计 `P(tier3)` |

---

## E. Trace 提取（Criteo；先于 B1/B2）

```bash
bash script/run_motivation.sh
# 或：
python3 script/extract_criteo_trace.py --queries 10000000 --l1-gib 1 --l2-gib 5
```

| 项 | 说明 |
|----|------|
| `row_id` | `(vocab_offset[feat]+cat_id) % N_logical`，**每条广告 26 次 lookup** |
| 热行 | trace 内按频率 top-10% distinct slot（`top10_mass` 通常 **~0.9**） |
| 产物 | `read_trace.bin`、`trace_meta.json`、`target_access_cdf.csv` |

```text
results/motivation/trace/
  read_trace.bin
  trace_meta.json      # workload=criteo
  target_access_cdf.csv
```

| 【门禁 1a】 | 目视 `target_access_cdf.csv` / 图；应明显右偏 |

---

## 传输（默认 SHM）

| 组件 | 配置 |
|------|------|
| server | `--comm-mode pgas-shm --pgas-shm-name /cxlmemsim_pgas` |
| QEMU | `export CXL_TRANSPORT_MODE=shm` |
| guest 冷读 | `mmap /dev/dax0.0` → 经 shm 访问 host 池（非 TCP RPC） |

> **A/B/B3 全部在 SHM 栈上跑。** TCP 只出现在 **C（M0）** 对照实验。

---

## 容量模型（本机 10 GiB 满池 + 等比例 host cache）

**逻辑**：全表在 CXL；每 host **L1 + L2** 为 cache，放不下走 **L3**。  
**本机**：逻辑表与 CXL 池 **同为 10 GiB**（`N_sim = N_logical`）；L1/L2 按 `table_gib/1024` 相对 1TiB 生产缩放。

### 逻辑表 + CXL 池（本机默认）

| 项 | 值 | 计算 |
|----|-----|------|
| 表 / 池 | **10 GiB** | `--table-gib 10`；server `--capacity=10240` MiB |
| `N_logical` = `N_sim` | **20,971,520** | `10 GiB ÷ 512 B`；`slot = row_id` |
| 热行 top-10% | ≈ **1 GiB** | **> L1（320 MiB）** → 门禁 2 |

### 每 host cache（等比例）

| 层级 | 生产 @ 1TiB | **本机 @ 10GiB** |
|------|-------------|------------------|
| L1 | 32 GiB | **0.3125 GiB** |
| L2 | 256 GiB | **2.5 GiB** |
| L3 池 | TiB 级 | **10 GiB 满池** |

`--scale-host`：L1/L2 × `(table_gib/1024)`；**池** = `table_gib × 1024` MiB（满表，不缩成 10 MiB）。

### CLI / meta

`--table-gib 10 --scale-host`；`trace_meta` / `init_config` 含 `table_gib, sim_cxl_mb=10240, sim_cxl_rows=n_logical, ...`。

### 生产尺度参考（论文，非本机默认）

1 TiB 逻辑表 + 1 GiB **稀疏**池 + 32/256 GiB L1/L2（论文尺度；本机用 Criteo + 10 GiB 满池）。

---

## A. Embedding 初始化（满池 backing；本机 10 GiB）

**目标**：逻辑表与 CXL 池均为 **10 GiB**；在池内物化全表（或按需填充）；L1/L2 仍为 cache。  
**顺序**：E→B1→B2 可无 CXL；**B3 前** VM1 完成 A。B3 的 L3 走 `/dev/dax0.0`。

- [ ] **A1** 实现/编译 `embedding_bench`（`microbench/embedding_sim/`）
- [ ] **A2** `mmap /dev/dax0.0`；本机 **`N_sim = N_logical`**，`slot = row_id`；**不**把全表复制进 L1/L2
- [ ] **A3** L1=HBM，L2=DDR cache（空启动）
- [ ] **A4** replay trace（见上 Lookup 语义）
- [ ] **A5** 初始化：

```bash
./build/embedding_bench --init-only \
  --table-gib 10 --dim 128 \
  --trace-in results/motivation/trace/read_trace.bin
# server 需已起：--capacity=10240
```

- [ ] **A6** `results/motivation/init_config.json`

---

## B. 验收（**仅 VM1**；**必须** `--trace-in`）

> **顺序**：**B1 → 门禁 1 → B2 → 门禁 2 → B3**。

### B1 — 【门禁 1】访问 CDF

```bash
./build/embedding_bench --phase replay-access \
  --trace-in results/motivation/trace/read_trace.bin \
  --dump-access-cdf --overlay-target results/motivation/trace/target_access_cdf.csv
```

| 图 | `access_skew_cdf.png`：① 与 ② 叠加 |
| 数值 | `max_cdf_gap ≤ 0.01`；`top10_access_frac` 仅报告 |

**未过门禁 1 → 停止**，重新 `extract_criteo_trace.py`。

### B2 — 【门禁 2】miss 与访问偏斜

**前提**：B1 通过。同一 `read_trace.bin`，记录 `(row_id, tier, lat_ns)`。

#### B2a — 结构性

```bash
./build/embedding_bench --phase replay-cache \
  --trace-in results/motivation/trace/read_trace.bin \
  --table-gib 10 --dim 128 --cache-unlimited --dump-cache-cdf
```

| 验收 | `P(tier=3) ≈ 0` |

#### B2b — Motivation cache

```bash
./build/embedding_bench --phase replay-cache \
  --trace-in results/motivation/trace/read_trace.bin \
  --table-gib 10 --dim 128 --l1-gib 1 --l2-gib 5 \
  --dump-cache-cdf --dump-miss-vs-access
```

| 图 | **③a** `cache_tier_cdf.png`；**③b** `miss_vs_access_cdf.png` |

**【门禁 2】**（B2b，流式 warmup 后半段统计）：

| 指标 | 期望 |
|------|------|
| `l3_rate_cold` | **≥ 2×** `l3_rate_hot` |
| `l3_mass_cold_rows` | **≥ 0.60** |
| ③b 形态 | 右高左低（非扁平） |

**未过门禁 2 → 不跑 B3**。

---

## C. M0 — SHM 比 TCP（RDMA stub）快

```bash
bash script/run_latency_cdf_compare.sh
```

| 验收 | `pgas-shm` median/p99 **明显低于** `tcp` |

> 跑完 M0 后恢复 pipeline shm，再跑 B3。

---

## B3 — M1 + M2（2-host 竞争）

| 角色 | 行为 |
|------|------|
| VM1 reader | `--phase replay-run --trace-in .../read_trace.bin` |
| VM0 writer | **W-hot**：`write_hot_rows.bin`；**W-cold**：非 hot 行均匀抽 |

| 轮次 | VM0 | 观察 |
|------|-----|------|
| **R-only** | 不写 | M1 基线 |
| **R+W-hot** | 热行 delta | M2 p99↑、BI↑ |
| **R+W-cold** | 冷行 delta | 对照 |

产出：**④** `latency_cdf_read.png`、`bi_storm.txt`、`p99_read_*.csv`。

---

## summary.md 模板

```markdown
# Motivation（2-host，10 GiB 满池）

## M0 — SHM vs TCP
- TCP p50/p99: ___ / ___
- PGAS-SHM p50/p99: ___ / ___

## 工作负载（Criteo DLRM，10 GiB 满池）
- 逻辑表 / CXL 池: ___ GiB（应相同）, N_logical=___, dim=128
- trace: Q=___, 文件 ≈ ___ MiB（仅 row_id）
- L1 / L2: ___ / ___ GiB（相对 1TiB 等比例）   server --capacity=___ MiB   hosts: 2

## 门禁 1 — 访问 CDF
- top10_mass / max_cdf_gap: ___  → 通过 / 未通过

## 门禁 2 — Miss vs 访问
- l3_rate_hot / l3_rate_cold: ___ / ___
- l3_mass_cold_rows: ___  → 通过 / 未通过

## M1 / M2
- R-only p99: ___
- R+W-hot / R+W-cold p99、BI 倍数: ___
```

---

## 实现优先级

1. `extract_criteo_trace.py` → ①
2. `replay-access`（B1）→ ②，**门禁 1**
3. `replay-cache`（B2）→ ③，**门禁 2**
4. `init-only`（A）
5. `replay-run` + writer（B3）
6. M0（C）

---

## 下一步（暂缓）

→ [03-experiments.md](./03-experiments.md)：motivation 写入 proposal 后再开；可在此阶段换成 Criteo-DLRM 等**真实** workload，仍保持「逻辑大表 + 小 trace + 稀疏物化」原则。
