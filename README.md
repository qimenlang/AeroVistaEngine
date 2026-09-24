# AeroVistaEngine
Aero Vista Rendering Engine Based on VSG

## 多通道联调（Host + 3 IG）一键启动

三种 IG 连接模式（`scripts/run_multichannel.sh -m`）：

| `-m` | 拓扑 | Host | IG 配置 | IG `target` |
| --- | --- | --- | --- | --- |
| **1** | 正式直连 | `aerovistaPlatform.exe` | `platform_ig_{main,left,right}.json` | 平台 UDP 7000 / TCP 7100 |
| **2** | 本地调试 | `aerovistaViewHost.exe`（自己当 Host） | `viewhost_ig_{main,left,right}.json` | viewhost UDP 8000 / TCP 8100 |
| **3** | 真模拟器中继 | 先平台，再 viewhost（`viewhost_relay.json`），最后 IG | 同模式 2 | 同模式 2；虚 IG 连平台 7000/7100 |

启动脚本位于 `scripts/`：

| 环境 | 命令 | 说明 |
| --- | --- | --- |
| Windows cmd / 双击 | `scripts\run_multichannel.bat` | **薄转发壳**，仅转发到 git-bash 版 |
| git-bash / MSYS2 | `scripts/run_multichannel.sh` | **单一事实来源**（exe 路径 / IG 名单 / 日志布局都在此） |
| Windows cmd / 双击 | `scripts\run_seam_grid.bat` | 接缝网格三通道（`scene_seam_grid_ig_*.json`），转发到 `.sh` |
| git-bash / MSYS2 | `scripts/run_seam_grid.sh` | 同上；端口与模式 2 相同，不要与 `run_multichannel` 同时开 |

两个脚本**一体两面**：`.bat` 只是 `.sh` 的 Windows 转发壳（经 git-bash 调用），不重复维护任何启动逻辑——改动只改 `.sh`，两者不会分叉。`.bat` 与 `.sh` 都复用 `scripts/launch_vsgengine.ps1` 隐藏启动 IG（`-WindowStyle Hidden`，不弹 console）。

```bash
# git-bash
scripts/run_multichannel.sh -m 1       # IG 直连 platform
scripts/run_multichannel.sh -m 2       # IG 直连 viewhost
scripts/run_multichannel.sh -m 3       # platform + viewhost 中继 + IG
scripts/run_multichannel.sh stop       # 停止全部

# Windows cmd
scripts\run_multichannel.bat -m 1
scripts\run_multichannel.bat stop

# 接缝网格三通道（scene_seam_grid_ig_{main,left,right}.json，viewhost 本地 Host）
scripts/run_seam_grid.sh
scripts/run_seam_grid.sh stop
```

### 启动内容与日志

- 模式 1：`aerovistaPlatform.exe` + 3 个 `vsgEngine.exe`（`platform_ig_*.json`）。
- 模式 2：`aerovistaViewHost.exe` + 3 个 `vsgEngine.exe`（`viewhost_ig_*.json`）。
- 模式 3：平台 → viewhost 中继 → 3 IG（IG 仍用 `viewhost_ig_*.json`；脚本把 `viewhost_relay.json` 拷成 exe 旁 `viewhost.json`）。
- 接缝网格：`run_seam_grid.sh` 仍是 viewhost 本地 Host + `scene_seam_grid_ig_*`，日志 `logs/ig_seam_grid_<name>.{out,err}.log`。
- IG 的 console 被隐藏，stdout/stderr 重定向到 `logs/ig_<main|left|right>.{out,err}.log`（该目录已 gitignore）。
- 调试 IG 日志：`tail -f logs/ig_main.err.log`（git-bash）。

启动前脚本会先停掉已有的 platform / viewhost / `vsgEngine`，避免换模式时端口占用。

### 可执行文件路径（如需改构建目录）

在 `scripts/run_multichannel.sh` 顶部修改：

```bash
ENGINE=.../out/build/clang-Ninja-Debug/engine/vsgEngine.exe     # 引擎（clang 构建）
PLATFORM_DIR=.../out/build/vs2019/thirdparty/sync/examples/platform/Debug   # 模拟平台（MFC 仅 MSVC/vs2019）
VHOST_DIR=.../out/build/vs2019/thirdparty/sync/examples/viewhost/Debug      # viewhost（同上）
```
