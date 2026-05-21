# OCEAN 从零跑通（简明版）

本仓库是 **CXL 内存仿真框架**。下面 4 步是「在本机把核心组件编译好 + Python 工具装好 + 做一次冒烟检查」，**不是** README 里那套 QEMU 双虚拟机完整演示（那需要额外下载镜像，见文末）。

---

## 四步分别干什么

| 步骤 | 命令 | 干什么 |
|------|------|--------|
| 1 | `bash script/setup_build_deps.sh` | 用 apt 装 **C++ 编译依赖**（gcc-13、cmake、spdlog、libbpf 等），只需做一次 |
| 2 | `bash script/setup_python_uv.sh` | 用 **uv** 建空虚拟环境 `.venv`，逐个装 numpy/pandas 等（给 `use_cases/` 里 Python 脚本用） |
| 3 | `bash script/build_ocean.sh` | **编译** 主程序 `build/cxlmemsim_server`（CXL 内存仿真服务端） |
| 4 | `bash script/run_minimal_pipeline.sh` | **冒烟测试**：启动 server → 跑 pytest → 有 `/dev/dax0.0` 才跑 DAX 测试 |

---

## 从 0 开始（复制执行）

```bash
cd /home/wenjun/orion-yuwenjun/OCEAN   # 换成你的仓库路径

# 1. 系统依赖（需 sudo，只做一次）
bash script/setup_build_deps.sh

# 2. Python 环境（uv）
bash script/setup_python_uv.sh
source .venv/bin/activate

# 3. 编译 C++（不要在 conda base 里 cmake，用脚本即可）
bash script/build_ocean.sh

# 4. 冒烟
bash script/run_minimal_pipeline.sh
```

**成功时你会看到：**

- `build/cxlmemsim_server` 存在，且 `./build/cxlmemsim_server --help` 能打印帮助
- 日志写在 `pipeline_smoke.log`
- pytest 大部分通过（少数失败可能是仓库里 Python 小 bug，不影响 server 已启动）

---

## 常见问题

**Q：为什么要单独 `build_ocean.sh`，不能直接在 conda 里 cmake？**  
A：conda 会把旧版 `libstdc++` 链进二进制，运行时报 `GLIBCXX_3.4.32 not found`。`build_ocean.sh` 用系统库编译，避免这个问题。

**Q：`test_cxl_mem` 报 `/dev/dax0.0` 不存在？**  
A：正常。DAX 设备在 **QEMU 虚拟机**里才有；本机冒烟会 **跳过** 这项，只验证 server + Python。

**Q：conda 的 `ocean` 环境还要吗？**  
A：不必。Python 用 `.venv` + uv 即可；`environment.yml` 仅作备用。

---

## 下一步（完整 OCEAN / 双机）

若要 README 里的 **QEMU + 双 VM + `/dev/dax0.0`**：

1. 按 [README.md](../README.md) 执行 `script/setup_host.sh`（会编 QEMU，耗时长）
2. 配置网络、`launch_qemu_cxl*.sh`、下载 `bzImage` / `qemu.img`
3. 在 VM 内确认 `ls /dev/dax0.0`

那是 **完整实验 pipeline**；上面 4 步是 **开发机最小验证**。
