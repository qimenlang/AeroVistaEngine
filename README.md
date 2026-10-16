# AeroVistaEngine
Aero Vista Rendering Engine Based on VSG

## 多通道联调（Host + 3 IG）一键启动

三种 IG 连接模式（`scripts/run_multichannel.sh -m`），再加场景（`-s`，默认 1）：

| `-m` | 拓扑 | Host | `-s 1` IG 配置 | IG `target` |
| --- | --- | --- | --- | --- |
| **1** | 正式直连 | `aerovistaPlatform.exe` | `platform_ig_{main,left,right}.json` | 平台 UDP 7000 / TCP 7100 |
| **2** | 本地调试 | `aerovistaViewHost.exe`（自己当 Host） | `viewhost_ig_{main,left,right}.json` | viewhost UDP 8000 / TCP 8100 |
| **3** | 真模拟器中继 | 先平台，再 viewhost（`viewhost_relay.json`），最后 IG | 同模式 2 | 同模式 2；虚 IG 连平台 7000/7100 |

`-s 2` 把三路 IG 换成 `scene_seam_grid_ig_{main,left,right}.json`（接缝网格）。这组 json 写的是 viewhost 8000/8100，只能配 `-m 2` 或 `-m 3`。`-s` 省略等于 1。

启动脚本只有 git-bash：`scripts/run_multichannel.sh`（exe 路径 / IG 名单 / 日志布局都在此），`scripts/run_seam_grid.sh` 转到 `-m 2 -s 2`。

```bash
# git-bash
scripts/run_multichannel.sh -m 1          # IG 直连 platform，无网格
scripts/run_multichannel.sh -m 2          # IG 直连 viewhost，无网格
scripts/run_multichannel.sh -m 2 -s 2     # IG 直连 viewhost，接缝网格
scripts/run_multichannel.sh -m 3          # platform + viewhost 中继 + IG
scripts/run_multichannel.sh stop          # 停止全部

# 同上接缝网格的短命令（内部就是 -m 2 -s 2）
scripts/run_seam_grid.sh
scripts/run_seam_grid.sh stop
```

PowerShell 或 cmd 里用 Git bash 跑同一条：

```powershell
& "C:\Program Files\Git\bin\bash.exe" scripts/run_multichannel.sh -m 2 -s 2
```

### 启动内容与日志

- 模式 1：`aerovistaPlatform.exe` + 3 个 `vsgEngine.exe`（`-s 1` 用 `platform_ig_*.json`）。
- 模式 2：`aerovistaViewHost.exe` + 3 个 `vsgEngine.exe`（`-s 1` 用 `viewhost_ig_*.json`；`-s 2` 用 `scene_seam_grid_ig_*.json`）。
- 模式 3：平台 → viewhost 中继 → 3 IG（`-s 1` 仍用 `viewhost_ig_*.json`，`-s 2` 用 `scene_seam_grid_ig_*.json`；脚本把 `viewhost_relay.json` 拷成 exe 旁 `viewhost.json`）。
- `run_seam_grid.sh` 就是 `run_multichannel.sh -m 2 -s 2`。日志与其它启动一样，写到 `logs/ig_<name>.*`。
- IG 的 console 被隐藏（脚本里一次 `powershell.exe -Command`，没有单独的 `.ps1`），stdout/stderr 重定向到 `logs/ig_<main|left|right>.{out,err}.log`（该目录已 gitignore）。
- 调试 IG 日志：`tail -f logs/ig_main.err.log`（git-bash）。

启动前脚本会先停掉已有的 platform / viewhost / `vsgEngine`，避免换模式时端口占用。

### 编译（config + build all）

`scripts/config_build.sh` 按 `CMakePresets.json` 配置并编译全部 target：

```bash
scripts/config_build.sh                      # clang-Ninja-Debug，然后 vs2019
scripts/config_build.sh clang-Ninja-Debug    # 只编这一套（Ninja Debug）
scripts/config_build.sh vs2019               # 只编 VS2019；走 build preset vs2019-debug
```

`vs2019` 是多配置生成器，脚本编的是 Debug（与下面的 `platform` / `viewhost` 目录一致）。cmd 用 `scripts\config_build.bat`，参数相同。

### 可执行文件路径（如需改构建目录）

在 `scripts/run_multichannel.sh` 顶部修改：

```bash
ENGINE=.../out/build/clang-Ninja-Debug/engine/vsgEngine.exe     # 引擎（clang 构建）
PLATFORM_DIR=.../out/build/vs2019/thirdparty/sync/examples/platform/Debug   # 模拟平台（MFC 仅 MSVC/vs2019）
VHOST_DIR=.../out/build/vs2019/thirdparty/sync/examples/viewhost/Debug      # viewhost（同上）
```

## 测试覆盖率（Clang + llvm-cov）

统计范围与质量门（`lizard_engine.py` / `clang_tidy_engine.py`）同口径：`engine/source` + `thirdparty/sync/src` + `thirdparty/config/src`，排除 vsg / cigi / Catch2 等第三方源码。

配套 `CMakePresets.json` 的 `clang-Ninja-Coverage` preset（编译时插桩 `-fprofile-instr-generate -fcoverage-mapping`），用 `scripts/coverage_engine.py` 一条命令出报告：

```powershell
# 全量：configure + build + 跑全部测试 + 出 HTML 报告
python scripts/coverage_engine.py

```

产物（均在 `out/coverage/`，已被 `.gitignore` 覆盖）：

| 产物 | 路径 |
| --- | --- |
| HTML 报告 | `out/coverage/html/index.html` |
| 汇总数据 | `out/coverage/coverage.profdata` |
| 文本汇总 | 直接打印到终端（按文件列行/分支覆盖） |

前置：`C:/Program Files/LLVM/bin/` 下的 `clang++` / `llvm-profdata` / `llvm-cov`（与 `clang-Ninja-Coverage` preset 同一套工具链）。脚本从 `clang++` 所在目录找 `llvm-*` 工具，保证与编译器同版本。

> 覆盖率收集要全量构建 + 测试（~2 分钟+），**不进 pre-commit 硬门禁**，按需手动跑或 CI 夜跑。覆盖率只找"完全没被碰到的代码"，不证明断言是否有牙（见 `doc/notes/AI+TDD总结.md` §4）——后者由 ATDD + 变异测试负责。

