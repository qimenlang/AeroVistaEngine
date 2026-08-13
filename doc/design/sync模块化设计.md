# sync 模块化设计（拆独立库）

面向「将 sync 多通道同步模块做成一个库，单独编译，供本项目 vsgEngine 及其他项目使用」的重构规划。
基础行为与协议见 [多通道同步模块设计.md](./多通道同步模块设计.md)；坐标/位姿语义见 [lla位姿传输设计.md](./lla位姿传输设计.md)。
本文件是**重构执行契约**：分阶段、可独立验收、每阶段有明确边界。

## 1. 目标与边界

### 1.1 目标

1. sync 模块整体作为独立库 `aerovistaSync`（工程内 target，`add_subdirectory` 引用形态）单独编译。
2. 依赖方向单向：`vsgEngine(vsgEngineLib) → aerovistaSync`；**库不得反向依赖引擎**。
3. 保留现有协议、线程模型、配置与测试行为不变（重构不改语义）。

### 1.2 库边界（目标）

```text
vsgEngine (exe)
  └→ vsgEngineLib           （引擎：scene / viewer / 相机 / 配置解析）
       └→ aerovistaSync     （sync 库：传输层 + 门面）
            ├─ 传输层：UdpSocket / CigiWire / EventProcess / HostSync / IgSync
            │           / SyncConfig / SyncProtocol
            ├─ 门面层：SynchronSystem（纯数据流：场景模式注入 + 采样喂入 + 产出位姿）
            └─ 外部依赖：cigicl-static、ws2_32、vsg（仅门面层，见 §4.0）
```

- `SynchronSystem` 在 Phase 2 解耦后并入库；Phase 1 阶段库只含传输层。
- 库允许依赖 vsg（**仅门面层**，传输层零 vsg）；但 **不得依赖 `Engine`**。vsg 依赖策略见 §4.0。

### 1.3 非目标

- 不改变握手 / 数据面协议与线格式（`sync_proto`、CIGI V4）。
- 不改变线程模型与命令面时序（主线程执行场景、命令读循环线程回执）。
- 不做 Host 独立进程改造（那是 [多通道同步模块设计.md](./多通道同步模块设计.md) §1.1 的远期项）。

## 2. 现状与遗留障碍

### 2.1 已完成

- `Network`（Boeing MPV，GPL）→ `UdpSocket`（自有实现）替换：GPL 依赖清除，API 收缩为实际使用的 5 个方法。
- 传输层（`UdpSocket`/`CigiWire`/`EventProcess`/`HostSync`/`IgSync`/`SyncConfig`/`SyncProtocol`）**零 vsg、零 Engine 依赖**，时钟用 `std::chrono::steady_clock`。
- 门禁/文档中的 Network 排除与引用已清理。

### 2.2 遗留障碍（整体入库的拦路虎）

| 障碍 | 位置 | 影响 |
|---|---|---|
| `SynchronSystem ↔ Engine` 双向依赖 | `SynchronSystem.cpp` `#include "engine.h"`；`engine.h` 持有 `vsg::ref_ptr<SynchronSystem>` | 库反向依赖引擎，无法独立编译/分发 |
| 配置结构归属混乱 | `EngineConfig.h` 反向 `#include "function/sync/SyncConfig.h"`；`SynchronSystem.h` 又 `#include "function/config/EngineConfig.h"`（取 `OffsetDeg`/`HostEyeStalePolicy`） | 目录耦合，库头文件不干净 |
| 相机交互耦合 | `SynchronSystem` 直接驱动 `Engine` 相机（写）并读相机（采样/防回声/模式判定） | 需要数据流解耦（注入 + 喂入 + 产出） |

## 3. 实施计划

### Phase 1 — 传输层独立库（低风险，可单独验收）

| 步骤 | 内容 | 涉及 | 验收 |
|---|---|---|---|
| 1.1 | 新建 `aerovistaSync` STATIC 库 target，收编传输层 7 文件 | 新 `engine/sync/CMakeLists.txt`（或同级新目录） | `cmake --preset clang-Ninja` 配置成功 |
| 1.2 | target 链接 `cigicl-static` + `ws2_32`；`vsgEngineLib` 由编译源文件改为链接 `aerovistaSync` | `engine/CMakeLists.txt` | 链接通过 |
| 1.3 | 行为回归 | — | `vsgEngineTests` 全绿（含真实 UDP E2E） |
| 1.4 | 复杂度门禁 | — | `lizard_engine.py` / `clang_tidy_engine.py` 通过 |

> 这一步完成后，传输层已是可分发形态（仅依赖 CIGI + Winsock）。

### Phase 2 — `SynchronSystem` 解耦（整体入库的前提）

| 步骤 | 内容 | 涉及 | 验收 |
|---|---|---|---|
| 2.1 | 数据流 API：`SynchronSystem` 改为注入场景模式/椭球/channelId、`captureAuthorityEye(lookAt)` 喂入采样、`update()` + `takePendingCameraPose()` 产出位姿；**删除 `#include "engine.h"`** | `SynchronSystem.h/.cpp` | 无 `engine.h` 引用 |
| 2.2 | `Engine` 不再继承相机接口；`stepSync()`（决策 + 应用）+ `update()` 内采样 + 应用；`EngineConfig.h` 不再 include `SyncConfig.h` | `engine.h/.cpp` | 依赖方向单向 |
| 2.3 | 回归：测试 `update(engine)` → `engine.stepSync()` | `HostIGTests.cpp` | 全测试绿 |
| 2.4 | 回归 + 门禁 + 设计文档同步 | 全部 + `doc/design` | 见 §4 |

### Phase 3 — 门面并入 + 对外交付

| 步骤 | 内容 | 涉及 |
|---|---|---|
| 3.1 | `SynchronSystem` 文件并入 `aerovistaSync`，公开 API 收敛为库门面（`namespace aerovista` 下） | sync 库 CMake、头文件 |
| 3.2 | 库头文件布局整理、`#pragma once`、VSG 包含规范 | 各头文件 |
| 3.3 | 提供其他项目最小接入示例（初始化 + 每帧调用顺序） | 新 `examples/` 或 README |

### Phase 4 — 验收与文档

| 步骤 | 内容 | 涉及 |
|---|---|---|
| 4.1 | `doc/design` 全量核查：库边界 / API 签名 / 构建方式与实现一致 | `doc/design/*.md` |
| 4.2 | 全量测试 + lizard/tidy + CI 门禁 | CI |

## 4. 关键设计决策

### 4.0 vsg 依赖策略（写死）

**Phase 2 消除的是对 `Engine`（宿主引擎类）的依赖，不是对 vsg 的依赖。** 分两层：

- **传输层**（`UdpSocket`/`CigiWire`/`EventProcess`/`HostSync`/`IgSync`/`SyncConfig`/`SyncProtocol`）：**零 vsg、零 Engine**，纯 C++ + Winsock + CIGI。可被任意项目（含非 vsg 宿主）复用。
- **门面层**（`SynchronSystem`）：**依赖 vsg，不依赖 Engine**。场景模式/椭球/channelId 由宿主注入，权威 LookAt 由宿主采样喂入，产出位姿由宿主应用——门面不触碰宿主的相机对象（数据流，见 §4.1）。

理由（消费方均为 vsg 生态，本项目 vsg 亦为 submodule 引入）：

1. 类型直通：门面产出 `HostEyePose`（`vsg::dvec3`），宿主直接应用，零适配。
2. 椭球数学归属 vsg：LLA↔ECEF、ENU 方向换算建立在 `vsg::EllipsoidModel` 上（`SynchronSystem.cpp` 的 `convertECEFToLatLongAltitude` / `computeLocalToWorldTransform` / `convertLatLongAltitudeToECEF`）。彻底消除需自实现 WGS-84 或推回引擎，成本高收益低。
3. submodule 依赖 vsg 与本项目 `thirdparty/vsg` 现状同构，管理成熟。

**隔离要求**：vsg 依赖不得扩散出门面层；新代码禁止在传输层文件引入 `<vsg/...>`。
**可选演进（不阻塞）**：若未来出现非 vsg 消费方，将门面层 `HostEyePose` 的 vsg 字段替换为自有 POD（`SyncPose` + 坐标帧枚举），传输层不动。

### 4.1 相机交互：纯数据流（写死）

**不用接口回调（`SyncCameraTarget` 已否决**——`Engine` 继承相机目标接口语义不搭，且运行期「你传我、我调你」有回环感）。改为纯数据流：

```cpp
// SynchronSystem（sync 库）：
void setSceneIsEllipsoid(bool sceneIsEllipsoid);                    // 宿主场景确定/重建后注入
void setEllipsoidModel(vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid); // LLA 采样/防回声需要
void setChannelId(int channelId);                                   // 错误日志

void captureAuthorityEye(const vsg::LookAt& lookAt);                // Host 引擎：handleEvents 后喂当前相机 LookAt
void update();                                                       // 收包 + 决策，产出本帧位姿
std::optional<HostEyePose> takePendingCameraPose();                 // 取走本帧应写相机的位姿
void postFrame(double simTimeMs);

// 宿主每帧：
//   采样（仅 Host）→ update() → takePendingCameraPose() → 按 frame 自己写相机（每帧一次）
//   WorldLocal → setCameraPose；LLA → setCameraPoseLla
```

- 门面对相机的「读」全部变成**显式输入**：场景模式（`setSceneIsEllipsoid`）、椭球（`setEllipsoidModel`）、采样（`captureAuthorityEye(lookAt)`）。
- 门面对相机的「写」变成**输出数据**：`takePendingCameraPose()` 返回 `HostEyePose`（含 frame），宿主自行应用。
- 依赖方向单一：宿主 → sync 库（注入/拉取），sync 库不持有宿主的任何对象引用。
- 宿主 `Engine` 提供 `stepSync()`（决策 + 应用，无采样）供测试/`tickSync` 使用；真实帧循环在 `update()` 内完成采样 + 应用。

### 4.2 配置结构归属（写死）

- `OffsetDeg`、`HostEyeStalePolicy`、`SyncRoleConfig`、`IgConfig`、`HostConfig`、`SyncPaceConfig` **全部归 sync 库**（`SyncConfig.h`）。
- `EngineConfig.h` 保留引擎侧配置（窗口/模型/实体/相机），跨库引用只走 sync 库公开头。
- **`IgConfig` 合并本地绑定 + 远端 Host 目标**（`bindAddr`/`udpPortSend`/`udpPortRecv` + `targetAddr`/`targetTcpPort`/`targetUdpPortRecv`）；不再有独立的 `AddressConfig`/`hostEndpoint`。见 §8。

### 4.3 命令面桥（写死）

- `Engine::bindSyncCommandHandler` 由「直调 `SynchronSystem`」改为注入式：sync 库通过回调把 IG 命令面事件交给引擎（`setCommandHandler` 已存在，只需把宿主侧签名稳定成接口）。
- `CommandTriggerHandler` 依赖的 `hostSync()` 改经注入的 `HostSync&` 或命令面回调。

## 5. 风险与验收要点

| 风险 | 缓解 |
|---|---|
| Phase 2 是核心障碍，改动面大（engine.h/.cpp + 测试） | 拆小步：2.1→2.2→2.3→2.4 各自可独立编译；每步跑门禁 |
| 测试大量使用 `Engine::initSync` / `synchronSystem()`（如 `HostIGTests`） | 保持 `Engine` 公开 API 兼容；库内部重构不破坏测试可观测行为 |
| 命令面桥解耦后行为回归 | 接受性测试断言可观察结果（命令执行、RECEIVED/RESULT 回执），不受注入方式影响 |
| 重构引入复杂度超标 | 每一步 `python scripts/lizard_engine.py` + `clang_tidy_engine.py` |

### 验收清单（最终）

1. `aerovistaSync` 可独立 `add_subdirectory` + 链接，不反向依赖 `vsgEngineLib`/`Engine`。
2. `vsgEngine` / `vsgEngineTests` 构建通过，全测试绿。
3. `doc/design` 与实现一致（本文件 §1.2 库边界、§4 决策）。
4. 无残留旧符号（`rg "include \"engine.h\"" engine/source/function/sync` 为空）。

## 6. 与实现关系

| 项 | 状态 |
|---|---|
| `Network` → `UdpSocket` 替换 | **已实现** |
| 传输层独立库 `aerovistaSync`（Phase 1） | **已实现** |
| `SynchronSystem` 解耦（Phase 2，数据流：注入/采样喂入/产出位姿） | **已实现** |
| 门面并入库（Phase 3，`SynchronSystem` 并入 `aerovistaSync`） | **已实现** |
| 最小接入示例 `aerovistaSyncExample`（`AEROVISTA_BUILD_SYNC_EXAMPLE` 开关，默认 OFF） | **已实现** |
| `loadHostConfig` 库内入口 + `SyncJson` 解析器（viewhost 独立读配置） | **已实现** |
| `loadIgConfig` 库内入口（独立 IG 进程 / 外部引擎） | **已实现** |
| `syncSystem` 配置组（装配属性分组 + 旧扁平字段兼容回退） | **已实现** |
| 文档同步与最终验收（Phase 4） | **已实现** |

## 7. 实施后记（与初版规划的偏差）

1. **命令面桥无需解耦**：`engine.cpp::bindSyncCommandHandler` 调用 `igSync().setCommandHandler(...)` 与 `CommandTriggerHandler` 依赖 `SynchronSystem::hostSync()` 均为**引擎 → sync 库**方向的调用，不构成库的反向依赖；Phase 2 只做了 `SynchronSystem` 接口化与配置归位，命令面桥保持原样。
2. **相机交互改为纯数据流（`SyncCameraTarget` 接口已否决）**：评审发现「`Engine` 继承相机目标接口」语义不搭（Engine 不是相机目标），且运行期 `update(*this)` → 接口回调有回环感。改为：
   - `SynchronSystem` 不再持有/继承任何相机对象；场景模式、椭球、channelId 由宿主注入（`setSceneIsEllipsoid` / `setEllipsoidModel` / `setChannelId`），权威 LookAt 由宿主采样喂入（`captureAuthorityEye(lookAt)`），产出位姿由宿主取走应用（`takePendingCameraPose()`）。
   - 宿主 `Engine` 不再继承接口；`stepSync()`（决策 + 应用）供测试/`tickSync`，真实帧循环在 `update()` 内采样 + 应用。
   - 测试中的 `synchronSystem().update(engine)` 统一替换为 `engine.stepSync()`。
3. **vsg 依赖只在门面层**：sync 库 `aerovistaSync` 链接 `vsg::vsg`（`SynchronSystem` 需要），传输层 5 个 `.cpp` 仍零 vsg；`vsgEngineLib` 不再直接编译 sync 源文件，仅链接 `aerovistaSync`。

## 8. 配置合并（hostEndpoint → igConfig，写死）

**决策**：`hostEndpoint` 并入 `igLocal`，统一为 `igConfig`；`hostLocal` 改名 `hostConfig`；`AddressConfig` 删除。配置文件只保留两个块：

```jsonc
// viewhost（Host-only）
{ "hostConfig": { "bindAddr": "0.0.0.0", "udpPortSend": 8001, "udpPortRecv": 8000, "tcpPort": 8100 } }

// engine（IG-only）
{
  "igConfig": {
    "bindAddr": "127.0.0.1", "udpPortSend": 8000, "udpPortRecv": 8005,   // 本地绑定
    "targetAddr": "127.0.0.1", "targetTcpPort": 8100, "targetUdpPortRecv": 8000  // 远端 Host
  }
}
```

**动机**：
- `hostEndpoint` 本质是「IG 视角的 Host 地址」，与 `hostLocal` 高度重复（同一套端口两个进程冗余声明）。
- `bindAddr`/`targetAddr` 比 `addr` 语义更清晰（本地绑定 vs 远端目标）。
- 合并后 IG 侧一个配置块自洽，viewhost 侧一个配置块自洽，两端配置显著简化。

**字段命名**：本地字段保原名（`bindAddr`/`udpPortSend`/`udpPortRecv`），远端字段加 `target` 前缀（`targetAddr`/`targetTcpPort`/`targetUdpPortRecv`），避免原 `igLocal.udpPortRecv` 与 `hostEndpoint.udpPortRecv` 同名冲突。

**校验变化**：原「igLocal 需 hostEndpoint 配对」的三个用例删除（合并后无配对概念）；保留 `requireIgConnect` 无 `igConfig` 拒绝；`igConfig` 缺 target 字段、未知键（如 `tcpPort`、`targetUdpPortSend`）拒绝。

**C++ 类型**：`IgConfig`（6 字段）/ `HostConfig`（4 字段）替代 `AddressConfig`；`SyncRoleConfig` = `{enableHost, enableIg, hostConfig, igConfig}`。

### 8.1 host/ig 独立读取配置（viewhost / 独立 IG 进程，写死）

**目标**：viewhost（纯 Host）与独立 IG 进程（外部引擎挂载 sync，不用引擎整体配置）分别从**独立配置文件**读取各自的传输参数初始化，不依赖引擎侧配置。

**实现**：
- `aerovistaSync` 库内置 JSON 解析器（`SyncJson.h`，纯标准库，零 vsg 零引擎依赖）。
- 库内两个对称入口：
  - `loadHostConfig(path, HostConfig&, error)`：解析只含 `hostConfig` 块的文件。
  - `loadIgConfig(path, IgConfig&, error)`：解析只含 `igConfig` 块的文件。
- viewhost 用法：

```cpp
HostConfig host;
loadHostConfig("viewhost.json", host, &error);
SyncRoleConfig role; role.enableHost = true; role.hostConfig = host;
SynchronSystem::create()->initialize(role);
```

- 独立 IG 用法（外部 engine 挂载，可自由选择配置来源）：

```cpp
IgConfig ig;
loadIgConfig("ig.json", ig, &error);
SyncRoleConfig role; role.enableIg = true; role.igConfig = ig;
SynchronSystem::create()->initialize(role);
```

**配置形态**（schema 与 engine 侧块一致，包裹方案）：
```jsonc
{ "hostConfig": { "bindAddr": "0.0.0.0", "udpPortSend": 8001, "udpPortRecv": 8000, "tcpPort": 8100 } }
{ "igConfig": { "bindAddr": "127.0.0.1", "udpPortSend": 8000, "udpPortRecv": 8005,
                "targetAddr": "127.0.0.1", "targetTcpPort": 8100, "targetUdpPortRecv": 8000 } }
```

**解析器单一事实源**：`SyncJson.h` 的 `JsonParser` + 通用辅助（`find`/`requireString`/`requireInt`/`rejectUnknownKeys` 等）全部归 sync 库，`loadHostConfig`/`loadIgConfig` 与引擎侧 `EngineConfig.cpp` **共用**。引擎不再自带 parser 和通用辅助，也不重复实现 `parseHostConfig`/`parseIgConfig`（直接调用 sync 库公开 API）。

**`requireInt` 严格整数（写死）**：整数字段（端口等）拒绝小数，sync 侧与引擎侧行为一致。

**验收测试**：
- `HostIGTests.cpp` 的 `[viewhost]` 场景——`loadHostConfig` 读 host-only 配置 → `SynchronSystem::create()` → `initialize(enableHost=true)` 拉起 HostSync，与带 IG 的 Engine 真实 TCP/UDP 握手 + CIGI IGCtrl→SOF 收发。
- `HostIGTests.cpp` 的 `[standalone]` 场景——**host 与 IG 双侧都走 sync 库独立配置文件**（`loadHostConfig`/`loadIgConfig` → 各自 `SynchronSystem`），IG 侧装配参数程序化注入（`setOffsetDeg`/`setHostEyeStalePolicy`/`setChannelId`），双通道 CIGI 收发。
- `EngineConfigTests.cpp` 的 `loadIgConfig` 单元用例（正常解析 / 未知顶层键拒绝 / 部分对象拒绝）。

### 8.2 `syncSystem` 配置组（SynchronSystem 装配属性，写死）

**动机**：`channelId`/`offsetDeg`/`hostEyeStalePolicy`/`requireIgConnect` 是 **SynchronSystem 的属性**，与 engine 渲染属性、host/ig 传输属性正交。独立成组后 engine 属性 / syncSystem 属性 / ig·host 属性三类分明，配置项可自由组合成不同配置文件。

**配置形态**：
```jsonc
{
  "syncSystem": {
    "channelId": 0,
    "offsetDeg": { "yaw": 0.0, "pitch": 0.0, "roll": 0.0 },
    "hostEyeStalePolicy": "ReuseLast",
    "requireIgConnect": true
  },
  "igConfig": { ... },
  "hostConfig": { ... },
  "model": ..., "window": ...     // engine 渲染属性，不进 syncSystem
}
```

**归属边界**（写死）：
- `syncSystem` 组 = SynchronSystem 装配属性（IG 侧消费为主：offset/stale/requireIgConnect；channelId 两端标识）。
- `hostConfig`/`igConfig` = 传输参数（sync 库，§8.1）。
- `model`/`window`/`entities`/`camera`/`coordFrame` = engine 渲染属性（不进 sync）。

**平滑迁移（写死）**：JSON 顶层保留旧扁平字段（`channelId`/`offsetDeg`/`hostEyeStalePolicy`/`requireIgConnect`）作为**兼容回退**。解析规则：
- 有 `syncSystem` 组 → 用之，并**同步到旧扁平字段**（旧访问点 `config.channelId` 等保持可用）。
- 无 `syncSystem` 组 → 按旧扁平字段解析（旧配置完全兼容）。

**消费路径**：engine 从 `config.syncSystem`（或回退的扁平字段）注入 `SynchronSystem`（`setChannelId`/`setOffsetDeg`/`setHostEyeStalePolicy`/`initialize(requireIgConnect)`）。viewhost 纯 Host 可缺省 `syncSystem` 组（默认值全 0/ReuseLast/false）。
