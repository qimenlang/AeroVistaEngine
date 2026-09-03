# AeroVistaEngine
Aero Vista Rendering Engine Based on VSG

## 多通道联调（1 Host + 3 IG）一键启动

项目含 1 个 Host 进程（`aerovistaViewHost.exe`，MFC GUI，读其 exe 目录下的 `viewhost.json`）与多个 IG 进程（`vsgEngine.exe`）。联调时需同时启动它们。

启动脚本位于 `scripts/`：

| 环境 | 命令 | 说明 |
| --- | --- | --- |
| Windows cmd / 双击 | `scripts\run_multichannel.bat` | **薄转发壳**，仅转发到 git-bash 版 |
| git-bash / MSYS2 | `scripts/run_multichannel.sh` | **单一事实来源**（exe 路径 / IG 名单 / 日志布局都在此） |

两个脚本**一体两面**：`.bat` 只是 `.sh` 的 Windows 转发壳（经 git-bash 调用），不重复维护任何启动逻辑——改动只改 `.sh`，两者不会分叉。`.bat` 与 `.sh` 都复用 `scripts/launch_vsgengine.ps1` 隐藏启动 IG（`-WindowStyle Hidden`，不弹 console）。

```bash
# git-bash
scripts/run_multichannel.sh            # 启动：1 Host + 3 IG
scripts/run_multichannel.sh stop       # 停止全部

# Windows cmd
scripts\run_multichannel.bat stop      # 等价（转发到 sh）
```

### 启动内容与日志

- 启动：`aerovistaViewHost.exe` + 3 个 `vsgEngine.exe`（配置 `viewhost_ig_main.json` / `viewhost_ig_left.json` / `viewhost_ig_right.json`）。
- IG 的 console 被隐藏，stdout/stderr 重定向到 `logs/ig_<main|left|right>.{out,err}.log`（该目录已 gitignore）。
- 调试 IG 日志：`tail -f logs/ig_main.err.log`（git-bash）。

### 可执行文件路径（如需改构建目录）

在 `scripts/run_multichannel.sh` 顶部修改：

```bash
ENGINE=.../out/build/clang-Ninja-Debug/engine/vsgEngine.exe     # 引擎（clang 构建）
VHOST_DIR=.../out/build/vs2019/thirdparty/sync/examples/viewhost/Debug   # Host（MFC 仅 MSVC/vs2019 构建）
```
