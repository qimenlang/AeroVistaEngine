# sync 模块化设计（设计基线）

面向「将 sync 多通道同步模块做成一个库，单独编译，供本项目 vsgEngine 及其他项目使用」的设计基线。
基础行为与协议见 [多通道同步模块设计.md](./多通道同步模块设计.md)；坐标/位姿语义见 [lla位姿传输设计.md](./lla位姿传输设计.md)。

> 本文档描述当前实现的**现状与设计理由**，不记录变更历史。外部接入与构建方式见 [接入说明](../../thirdparty/sync/README.md)。

## 1. 目标与边界

### 1.1 目标

1. sync 模块整体作为独立库 `aerovistaSync`（`thirdparty/sync` submodule，`add_subdirectory` 引用形态）单独编译。
2. 依赖方向单向：`vsgEngine(vsgEngineLib) → aerovistaSync`；**库不反向依赖引擎**。
3. 保留协议、线程模型、配置与测试行为。

### 1.2 库边界

```text
vsgEngine (exe)
  └→ vsgEngineLib           （引擎：scene / viewer / 相机 / 配置解析）
       └→ aerovistaSync     （sync 库：thirdparty/sync；传输层 + IG 决策层）
            ├─ 传输层：UdpSocket / TcpSocket / CigiWire / EventProcess / HostSync / IgSync
            │           / SyncConfig / SyncProtocol
            ├─ IG 决策层：SynchronSystem（收包 + frame 校验 + offset 合成 + 产出位姿）
            └─ 外部依赖：cigicl-static、ws2_32（vsg 仅作构建期依赖，见 §3.0）
```

- 库同时含传输层与 IG 决策层；Host 采样/扇出由宿主（Engine）直接持有 `HostSync` 完成，不经 `SynchronSystem`。
- 库公开接口零 vsg（内部复用 vsg header-only 数学，构建期依赖）；不依赖 `Engine`。vsg 依赖策略见 §3.0。
- **命名空间**：所有类型/函数在 `namespace aerovista::sync`（顶层 `aerovista` 符合 CONTRIBUTING.md 约定；`sync` 子层标识库边界）。子命名空间 `cigi_wire`/`sync_proto`/`sync_json` 嵌套在 `aerovista::sync` 下。外部引用示例：`aerovista::sync::SynchronSystem`、`aerovista::sync::cigi_wire::EyePose`。

### 1.3 非目标

- 不改变握手 / 数据面协议与线格式（`sync_proto`、CIGI V4）。
- 不改变线程模型与命令面时序（主线程执行场景、命令读循环线程收包入队）。
- 不做 Host 独立进程的协议 / 上行改造（独立 Host 进程已有 viewhost 示例，见 [viewhost设计.md](./viewhost设计.md)；「指定输入 IG 上行」仍属后期）。

## 2. 库结构

- 传输层（`UdpSocket`/`TcpSocket`/`CigiWire`/`EventProcess`/`HostSync`/`IgSync`/`SyncConfig`/`SyncProtocol`）**零 vsg、零 Engine 依赖**，纯 C++ + Winsock + CIGI。可被任意项目（含非 vsg 宿主）复用。
- IG 决策层（`SynchronSystem`）公开接口零 vsg（自有 POD + 注入接口）、不依赖 Engine，收包后做 frame 校验 / offset 合成 / stale policy / 断线兜底，产出位姿由宿主取走（§3.1）。
- 配置类型（`OffsetDeg`/`HostEyeStalePolicy`/`SyncRoleConfig`/`IgConfig`/`HostConfig`）全部归 sync 库（`SyncConfig.h`）；`EngineConfig.h` 只保留引擎侧配置。`SyncPaceConfig` 已于 2026-08 删除（无消费方）。
- 目录布局：`include/aerovista/sync/*.h`（公共头）+ `src/*.cpp`（实现）+ `examples/`（接入示例）。

## 3. 关键设计决策

### 3.0 vsg 依赖策略

**消除的是对 `Engine`（宿主引擎类）的依赖；公开接口零 vsg，内部复用 vsg header-only 数学（构建期依赖）。** 分两层：

- **传输层**（`UdpSocket`/`TcpSocket`/`CigiWire`/`EventProcess`/`HostSync`/`IgSync`/`SyncConfig`/`SyncProtocol`）：**零 vsg、零 Engine**，纯 C++ + Winsock + CIGI。可被任意项目（含非 vsg 宿主）复用。
- **IG 决策层**（`SynchronSystem`）：**公开接口零 vsg、不依赖 Engine**。场景模式（椭球变换）/channelId 由宿主注入，Host 眼点经 `preFrame()`（IG 收包）或 `queueHostEyePose()`（测试注入）喂入，产出位姿由宿主应用——SynchronSystem 不触碰宿主的相机对象，也不承担 Host 采样/扇出（数据流，见 §3.1）。

vsg 的分层复用：

1. **公开边界零 vsg**：`HostEyePose`/`OffsetDeg` 用自有 POD `DVec3`（`SyncMath.h`），不暴露 `vsg::dvec3`；`SynchronSystem` 不再继承 `vsg::Object`（工厂 `SynchronSystem::create()` 返回 `std::unique_ptr`）。消费方（含完全无 vsg 的 viewhost）编译期零 vsg 头。
2. **内部复用 vsg header-only 数学**：`SynchronSystem.cpp` 内 `#include <vsg/maths/...>` 使用 `dvec3`/`dquat`/`dmat4`/`normalize`/`dot`/`length`/`radians`，这些 `constexpr`/模板内联进 `aerovistaSync` 库，不产生 `vsg::` 外部链接符号，viewhost 链接期零 vsg 库。CMake 中 `vsg::vsg` 为 `PRIVATE` 构建依赖，不传递给消费方。
3. **大地测量学注入接口**：LLA↔ECEF、ENU 方向换算通过 `EllipsoidTransform` 注入接口（`SyncMath.h`）由宿主实现——engine 用 `vsg::EllipsoidModel` 实现（`VsgEllipsoidTransform` 适配器），viewhost 纯 Host 无需注入。彻底去掉了 `vsg::EllipsoidModel`/`vsg::LookAt` 这类非 header-only 依赖。

**隔离要求**：vsg 依赖不得扩散出 `SynchronSystem.cpp`；新代码禁止在传输层或公开头引入 `<vsg/...>`。
**已落地**：早期「门面层依赖 vsg」方案已演进为零 vsg 公开接口 + 内部 header-only 数学 + 椭球注入接口（本 §3.0 为现状）。

### 3.1 相机交互：纯数据流

SynchronSystem 是**IG 位姿决策器**，与宿主相机通过**数据流**交互，不持有宿主的任何相机对象。采用数据流而非接口回调的原因：宿主继承相机目标接口语义不搭，且运行期「你传我、我调你」有回环感。

```cpp
// SynchronSystem（sync 库，IG 决策器）：
void setEllipsoidTransform(const EllipsoidTransform* transform);    // 场景模式注入：非空=椭球，空=本地（唯一入口）
void setChannelId(int channelId);                                   // 错误日志

void update();                                                       // 收包 + 决策，产出本帧位姿
std::optional<HostEyePose> takePendingCameraPose();                 // 取走本帧应写相机的位姿

// 宿主（IG 侧）每帧：
//   preFrame() 收包 → update() 决策 → takePendingCameraPose() → 按 frame 自己写相机（每帧一次）
//   WorldLocal → setCameraPose；LLA → setCameraPoseLla
//
// Host 采样/扇出不经过 SynchronSystem：宿主（Engine）自行持有 HostSync + HostPosePublisher，
//   每帧 HostPosePublisher::captureAuthorityEye(lookAt) 采样（LookAt→位姿+防回声）
//   + HostPosePublisher::postHostFrame(simTimeMs) 扇出。
```

- SynchronSystem 对相机的「读」全部变成**显式输入**：Host 眼点经 `preFrame()`（IG 收包）或 `queueHostEyePose()`（测试注入）喂入，场景模式经 `setEllipsoidTransform()`（空=本地）注入。
- SynchronSystem 对相机的「写」变成**输出数据**：`takePendingCameraPose()` 返回 `HostEyePose`（含 frame），宿主自行应用。
- 依赖方向单一：宿主 → sync 库（注入/拉取），sync 库不持有宿主的任何对象引用。
- **Host 侧**：宿主 `Engine` 直接持有 `HostSync`，采样（LookAt→位姿 + 防回声）与扇出（IGCtrl）经 `HostPosePublisher` 完成；`stepSync()`（决策 + 应用，无采样）供测试/`tickSync` 使用。

### 3.2 配置结构归属

- `OffsetDeg`、`HostEyeStalePolicy`、`SyncRoleConfig`、`IgConfig`、`HostConfig` **全部归 sync 库**（`SyncConfig.h`）。
- `EngineConfig.h` 保留引擎侧配置（窗口/模型/实体/相机），跨库引用只走 sync 库公开头。
- **`IgConfig` 合并本地收发端口 + 远端 Host 目标**（`udpPortSend`/`udpPortRecv` + `targetAddr`/`targetTcpPort`/`targetUdpPortRecv`）；配置只有 `hostConfig` 与 `igConfig` 两块。见 §4。

### 3.3 命令面桥

命令面为**业务 processor + 引用式发送**（状态同步设计初版.md §7/§8）：Host 侧 `CommandTriggerHandler`（实机入口）经 `hostSync().tcpOutgoing() << CigiSymbolTextDefV4` → `flushTcp()` 发文本指令；IG 侧 engine 经 `igSync().registerEventProcessor` 注册业务 processor。均为**引擎 → sync 库**方向的调用，不构成库的反向依赖。旧 `bindSyncCommandHandler`/`setCommandHandler`/`sendCommand` 已随旧命令面删除（2026-08）。

## 4. 配置设计

### 4.0 配置结构

配置文件有两块（viewhost 只带 `hostConfig`，engine 带 `igConfig`，同进程 Host+IG 带两块）：

```jsonc
// viewhost（Host-only）
{ "hostConfig": { "udpPortSend": 8001, "udpPortRecv": 8000, "tcpPort": 8100 } }

// engine（IG-only）
{
  "igConfig": {
    "udpPortSend": 8000, "udpPortRecv": 8005,   // 本地收发
    "targetAddr": "127.0.0.1", "targetTcpPort": 8100, "targetUdpPortRecv": 8000  // 远端 Host
  }
}
```

**字段语义**：
- `hostConfig`：Host 本地传输参数（`udpPortSend`/`udpPortRecv`/`tcpPort`）。
- `igConfig`：IG 本地收发端口（`udpPortSend`/`udpPortRecv`）+ 远端 Host 目标（`targetAddr`/`targetTcpPort`/`targetUdpPortRecv`）。

**设计理由**：
- 本地 UDP 接收 / TCP 监听**固定绑定所有网卡**（`INADDR_ANY`，即 `0.0.0.0`）；`targetAddr` 才是可配的远端 Host 目标。
- 已否决 `bindAddr` 字段：实现从未消费「本地绑定网卡」（接收/监听均写死 `INADDR_ANY`），移除以免误导「改配置即可限网卡」。
- IG 侧一个配置块自洽（本地 + 远端），viewhost 侧一个配置块自洽，两端配置简单。
- 远端字段加 `target` 前缀，避免与本地同名端口字段冲突。

**校验规则**：`requireConnectedIg` 无 `igConfig` 拒绝；`igConfig` 缺 target 字段、未知键（如 `tcpPort`、`targetUdpPortSend`）拒绝。

**C++ 类型**：`IgConfig`（5 字段）/ `HostConfig`（3 字段）；`SyncRoleConfig` = `{enableHost, enableIg, hostConfig, igConfig}`。

### 4.1 host/ig 独立读取配置（viewhost / 独立 IG 进程）

viewhost（纯 Host）与独立 IG 进程（外部引擎挂载 sync，不用引擎整体配置）分别从**独立配置文件**读取各自的传输参数初始化，不依赖引擎侧配置。

**实现**：
- `aerovistaSync` 库内置 JSON 解析器（`SyncJson.h`，纯标准库，零 vsg 零引擎依赖）。
- 库内两个对称入口：
  - `loadHostConfig(path, HostConfig&, error)`：解析只含 `hostConfig` 块的文件。
  - `loadIgConfig(path, IgConfig&, error)`：解析只含 `igConfig` 块的文件。
- viewhost（纯 Host）用法：直接持 `HostSync` 传输层（不经 IG 决策器 `SynchronSystem`），`initialize` 起 accept/UDP 线程 + `run` 置 RUNNING，每帧 `update(simTimeMs, eye*)` 扇出：

```cpp
HostConfig host;
loadHostConfig("viewhost.json", host, &error);
HostSync hostSync;
hostSync.initialize(host);
hostSync.run();
// 每帧：hostSync.update(simTimeMs, &eye);
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
{ "hostConfig": { "udpPortSend": 8001, "udpPortRecv": 8000, "tcpPort": 8100 } }
{ "igConfig": { "udpPortSend": 8000, "udpPortRecv": 8005,
                "targetAddr": "127.0.0.1", "targetTcpPort": 8100, "targetUdpPortRecv": 8000 } }
```

**解析器单一事实源**：`SyncJson.h` 的 `JsonParser` + 通用辅助（`find`/`requireString`/`requireInt`/`rejectUnknownKeys` 等）全部归 sync 库，`loadHostConfig`/`loadIgConfig` 与引擎侧 `EngineConfig.cpp` **共用**。引擎不再自带 parser 和通用辅助，也不重复实现 `parseHostConfig`/`parseIgConfig`（直接调用 sync 库公开 API）。

**`requireInt` 严格整数**：整数字段（端口等）拒绝小数，sync 侧与引擎侧行为一致。

**验收测试**：
- `HostIGTests.cpp` 的 `[viewhost]` 场景——`loadHostConfig` 读 host-only 配置 → 直接持 `HostSync`（`initialize(host)` + `run`）拉起，与带 IG 的 Engine 真实 TCP/UDP 握手 + CIGI IGCtrl→SOF 收发。
- `HostIGTests.cpp` 的 `[standalone]` 场景——**host 与 IG 双侧都走 sync 库独立配置文件**（host 侧 `loadHostConfig` → `HostSync`；IG 侧 `loadIgConfig` → `SynchronSystem`），IG 侧装配参数程序化注入（`setOffsetDeg`/`setHostEyeStalePolicy`/`setChannelId`），双通道 CIGI 收发。
- `EngineConfigTests.cpp` 的 `loadIgConfig` 单元用例（正常解析 / 未知顶层键拒绝 / 部分对象拒绝）。

### 4.2 `syncSystem` 配置组（SynchronSystem 装配属性）

`channelId`/`offsetDeg`/`hostEyeStalePolicy`/`requireConnectedIg` 是 **SynchronSystem 的属性**，与 engine 渲染属性、host/ig 传输属性正交。独立成组后 engine 属性 / syncSystem 属性 / ig·host 属性三类分明，配置项可自由组合成不同配置文件。

**配置形态**：
```jsonc
{
  "syncSystem": {
    "channelId": 0,
    "offsetDeg": { "yaw": 0.0, "pitch": 0.0, "roll": 0.0 },
    "hostEyeStalePolicy": "ReuseLast",
    "requireConnectedIg": true
  },
  "igConfig": { ... },
  "hostConfig": { ... },
  "model": ..., "window": ...     // engine 渲染属性，不进 syncSystem
}
```

**归属边界**：
- `syncSystem` 组 = SynchronSystem 装配属性（IG 侧消费为主：offset/stale/requireConnectedIg；channelId 两端标识）。
- `hostConfig`/`igConfig` = 传输参数（sync 库，§4.1）。
- `model`/`window`/`entities`/`camera`/`coordFrame` = engine 渲染属性（不进 sync）。

**消费路径**：`SynchronSystem::initialize(role, syncSystem)` 一次性吸收完整装配配置（`channelId`/`offsetDeg`/`hostEyeStalePolicy`/`requireConnectedIg`）；engine 从 `config.syncSystem` 传入。运行时调整（联调标定）仍可用 `setOffsetDeg`/`setHostEyeStalePolicy`/`setChannelId`。viewhost 纯 Host 可缺省 `syncSystem` 组（默认值全 0/ReuseLast/false）。

> **配置格式统一**：JSON 顶层不保留旧扁平字段（`channelId`/`offsetDeg`/`hostEyeStalePolicy`/`requireConnectedIg` 已并入 `syncSystem` 组）。`EngineChannelConfig` 与 JSON 一一对应（`syncSystem`/`hostConfig`/`igConfig`/`model`/`window`/`coordFrame`/`entities`/`camera`）。

## 5. 否决与决策记录

- **`SyncCameraTarget` 接口已否决**：`Engine` 继承相机目标接口语义不搭，且运行期「你传我、我调你」有回环感。改用纯数据流（§3.1）。未来若有人考虑回调式接口，先读此否决。
- **`Network`（Boeing MPV，GPL）不使用**：UDP 收发统一走自有的 `UdpSocket`（GPL 依赖清除）。
- **命令面桥不做接口解耦**：引擎 → sync 库方向的直调不构成反向依赖（§3.3）。
