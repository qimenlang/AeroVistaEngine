# sync 模块化设计（设计基线）

面向「将 sync 多通道同步模块做成一个库，单独编译，供本项目 vsgEngine 及其他项目使用」的设计基线。
基础行为与协议见 [多通道同步模块设计.md](./多通道同步模块设计.md)；坐标/位姿语义见 [lla位姿传输设计.md](./lla位姿传输设计.md)。

> 本文档描述当前实现的**现状与设计理由**，不记录变更历史。外部接入与构建方式见 [接入说明](../../../thirdparty/sync/README.md)。

## 1. 目标与边界

### 1.1 目标

1. sync 模块整体作为独立库 `aerovistaSync`（`thirdparty/sync` submodule，`add_subdirectory` 引用形态）单独编译。
2. 依赖方向单向：`vsgEngine(vsgEngineLib) → aerovistaSync`、`vsgEngineLib → AeroVistaConfig`；**库不反向依赖引擎**。
3. 保留协议、线程模型、配置与测试行为。

### 1.2 库边界

```text
vsgEngine (exe)
  └→ vsgEngineLib           （引擎：scene / viewer / 相机 / 配置解析）
       ├→ AeroVistaConfig   （JSON 契约辅助：thirdparty/config；语法走 nlohmann/json）
       └→ aerovistaSync     （sync 库：thirdparty/sync；传输层 + Host 任务状态 + HostDriver + IG 收发端点）
            ├─ 传输层：UdpSocket / TcpSocket / CigiWire / EventProcess / SyncInterface / HostSync / IgSync / SofRttTracker
            │           / SyncConfig
            ├─ Host 任务状态：HostDataManager（权威表，不持 socket）
            ├─ HostDriver：HostSync + HostDataManager + 可选虚 IgSync（viewhost 直接使用；含中继）
            ├─ IG 收发端点：SynchronSystem（收包 + IgSync 帧维护 + 连接查询）
            └─ 外部依赖：cigicl-static、AeroVistaConfig、ws2_32
```

- 库含传输层、**Host 任务状态**（`HostDataManager`）、**HostDriver**（`HostSync` + 权威数据 + 可选虚 `IgSync`）与 IG 收发端点。viewhost 直接持 `HostDriver`。engine 仅 IG。
- 库公开接口零 vsg；sync 实现 TU 不 `#include <vsg/...>`（眼点数学在 engine `CameraDriver`）。不依赖 `Engine`。vsg 依赖策略见 §3.0。
- **命名空间**：库公开类型/函数在 `namespace aerovista::sync`（顶层 `aerovista` 符合 CONTRIBUTING.md 约定；`sync` 子层标识库边界）。子命名空间 `cigi_wire` 嵌套在 `aerovista::sync` 下。外部引用示例：`aerovista::sync::SynchronSystem`、`aerovista::sync::HostDriver`、`aerovista::sync::cigi_wire::EyePose`。配置 JSON 契约辅助在独立库 `AeroVistaConfig`（`namespace aerovista::config`）。

### 1.3 非目标

- 不改变数据面协议与线格式（CIGI V4）。握手为 CIGI 信封（§4.2 / [平台同步设计.md](../平台同步设计.md) §6.3）。
- 不改变线程模型与命令面时序（主线程执行场景、命令读循环线程收包入队）。
- 不做 Host 独立进程的协议 / 上行改造（独立 Host 进程已有 viewhost 示例，见 [viewhost设计.md](../viewhost设计.md)；「指定输入 IG 上行」仍属后期）。
- 不把 `IgSync` 改名为 `ClientSync`。`HostSync`/`IgSync` 同继承纯虚 `SyncInterface`（切齐 take/send/`flush*`，不持 socket/session）。平台正式跑同一个 `HostSync`；viewhost 中继不另养一套用来解包重组出站的 CIGI IG Session（握手可复用 `connect`；虚 IG 回 SOF 用 `packSof` 读头）。见 [平台同步设计.md](../平台同步设计.md)。

## 2. 库结构

- 传输层（`UdpSocket`/`TcpSocket`/`CigiWire`/`EventProcess`/`SyncInterface`/`HostSync`/`IgSync`/`SofRttTracker`/`SyncConfig`）**零 vsg、零 Engine 依赖**，纯 C++ + Winsock + CIGI。可被任意项目（含非 vsg 宿主）复用。
- Host 任务状态（`HostDataManager`）**零 vsg、零 Engine、不持 `HostSync`**：权威表与按行组包；发送仍走 `HostSync::flush*`。见 §3.4。
- `HostDriver` **零 vsg、零 Engine**：持有 `HostSync` + `HostDataManager`；中继时另持虚 `IgSync`。viewhost 直接使用。报文自检 / 命令行也在本类（[viewhost设计.md](../viewhost设计.md) §4.0）。
- IG 收发端点（`SynchronSystem`）公开接口零 vsg、不依赖 Engine：收包解包 + IgSync 帧维护 + `igLinked()`。眼点 offset 合成在 Engine `CameraDriver`，写相机在 `Engine::applyLastHostEye`（§3.1）。
- 配置类型（`OffsetDeg`/`IgConfig`/`HostTarget`/`HostConfig`）全部归 sync 库（`SyncConfig.h`）；`EngineConfig.h` 只保留引擎侧配置。
- 目录布局：`include/aerovista/sync/*.h`（公共头）+ `src/*.cpp`（实现）+ `examples/`（接入示例）。

## 3. 关键设计决策

### 3.0 vsg 依赖策略

**消除的是对 `Engine`（宿主引擎类）的依赖；公开接口与实现 TU 均零 vsg。** 分两层：

- **传输层**（`UdpSocket`/`TcpSocket`/`CigiWire`/`EventProcess`/`SyncInterface`/`HostSync`/`IgSync`/`SofRttTracker`/`SyncConfig`）：**零 vsg、零 Engine**，纯 C++ + Winsock + CIGI。可被任意项目（含非 vsg 宿主）复用。
- **IG 收发层**（`SynchronSystem`）：**公开接口零 vsg、不依赖 Engine**。只负责收包解包 + IgSync 帧维护 + 连接状态查询（`igLinked()`）。眼点合成（offset）在 Engine `CameraDriver`（`engine/source/function/driver/`），写相机在 `Engine::applyLastHostEye`；SynchronSystem 不触碰眼点决策，也不承担 Host 采样/扇出（数据流，见 §3.1）。

vsg 的分层：

1. **公开边界零 vsg**：`OffsetDeg` 为自有 POD（`SyncConfig.h`），不暴露 `vsg::dvec3`；眼点不进 sync 公开头（engine `ChannelEye` 用 `vsg::dvec3`，viewhost 用 `cigi_wire::EyePose`）。`SynchronSystem` 工厂返回 `std::unique_ptr`。消费方（含无 vsg 的 viewhost）编译期零 vsg 头。
2. **眼点数学在 engine**：`CameraDriver.cpp` 使用 `vsg::dvec3` / `dquat`。sync 库实现 TU **不** `#include <vsg/...>`，CMake **不**链接 `vsg::vsg`。
3. **同步只 LLA**：同步层只支持 LLA；`setEllipsoidMode` / `tryAcceptPendingEye` / `SyncMath.h` 已删除。`vsg::EllipsoidModel` 不进 sync 公开边界。

**隔离要求**：新代码禁止在传输层或公开头引入 `<vsg/...>`。

### 3.1 相机交互：纯数据流

SynchronSystem 是**IG 收发端点**，与宿主通过**数据流**交互，不持有宿主的任何相机对象。眼点合成在 `CameraDriver`（Engine 值成员，无 `Engine&` 回指）；写相机在 `Engine::applyLastHostEye`。采用数据流而非接口回调的原因：宿主继承相机目标接口语义不搭，且运行期「你传我、我调你」有回环感。

```cpp
// SynchronSystem（sync 库，IG 收发端点）：

void preFrame();                                       // 收包解包 + IgSync 帧维护
bool igLinked() const;                                 // TCP+UDP 就绪（连接观测）
std::uint32_t igCtrlReceivedCount() const;             // 生产路径观测（HUD）
std::uint32_t lastIgCtrlFrameCntr() const;
std::uint64_t simTimeUs() const;
IgSync& igSync();                                      // 测试与上行探测

// CameraDriver（Engine 值成员；不持 Engine / 相机）：
void onOwnshipEyePose(const CigiEntityPositionCtrlV4& pose); // 眼点回调入口（Engine 转发）
ChannelEye compose(const ChannelEye& host);            // 通道偏移合成，写入 _lastApplied（测试注入）
std::optional<ChannelEye> lastAppliedEye() const;      // 最近合成位姿

// 宿主（IG 侧）每帧：
//   SynchronSystem::preFrame() 收包 → Engine::applyLastHostEye() 写相机（每帧一次）
//   恒 LLA → setCameraPoseLla
//
// Host 采样/扇出不经过 SynchronSystem：库内 HostDriver 持 HostSync，
//   viewhost 每帧驱动 HostDriver::update 扇出（键盘累积眼点，无采样/防回声）。
```

- 眼点对相机的「读」全部变成**显式输入**：Host 眼点经 `SynchronSystem::preFrame()`（IG 收包）触发业务回调（Engine::registerIgCallbacks 转发到 `CameraDriver::onOwnshipEyePose`），或 `compose()`（测试注入）喂入。
- 眼点对相机的「写」在 Engine：`applyLastHostEye` 读 `lastAppliedEye` → `setCameraPoseLla`（恒 LLA），SynchronSystem 与 CameraDriver 都不触碰相机。
- 依赖方向单一：宿主 → sync 库（注入/拉取），sync 库不持有宿主的任何对象引用。
- **Host 侧**：库内 `aerovista::sync::HostDriver` = `HostSync` + `HostDataManager` + 可选虚 `IgSync`；viewhost 直接使用。扇出（IGCtrl + 眼点）经 `HostDriver::update`；实体等权威状态经 Manager 建表、Driver 意图 API 发送（[viewhost设计.md](../viewhost设计.md) §4.0）。`stepSync()`（决策 + 应用）供测试/`tickSync` 使用。engine 不承担 Host 角色（`HostPosePublisher` 已删除）。

### 3.2 配置结构归属

- `OffsetDeg`、`IgConfig`、`HostTarget`、`HostConfig` **全部归 sync 库**（`SyncConfig.h`）。
- `EngineConfig.h` 保留引擎侧配置（窗口/模型/实体/相机），跨库引用只走 sync 库公开头。
- **`IgConfig` = 本地 UDP 接收端口 + 远端 `HostTarget`**（`udpPortRecv` + `target.{addr,tcpPort,udpPortRecv}`）；JSON 仍扁平 `targetAddr` / `targetTcpPort` / `targetUdpPortRecv`。传输块仍是 `hostConfig` 与 `igConfig`；viewhost 中继另加 `relay.enable` / `expectedIgCount`（[平台同步设计.md](../平台同步设计.md) §9）。见 §4。

### 3.3 命令面桥

命令面为**业务订阅 + 帧头化发送**（状态同步设计.md §7/§8）：Host 侧经 `HostDriver` → `HostSync::outMsgWithIgCtrlTcp() << 报文` → `flushTcp()`（实体控制先写 `HostDataManager` 再组包，见 [viewhost设计.md](../viewhost设计.md) §4.0）；IG 侧 engine 经 `igSync().addCallback` 订阅报文。均为**引擎/宿主 → sync 库**方向的调用，不构成库的反向依赖。engine 内报文自检 `PacketProbeHandler`：`bindRecvProbes` 订阅 Host→IG 全量报文记类名；F9 随机 TCP 一次性上行 / F10 发 UDP（契约 SOF + 碰撞 Notification；现网 F10 仍仅 SOF）。与 viewhost testtcp/testudp 下行对称。原 `CommandTriggerHandler` 随拆 Host **已删除**。旧 `bindSyncCommandHandler`/`setCommandHandler`/`sendCommand` / `registerEventProcessor` 已随旧命令面删除。

### 3.4 Host 任务状态（`HostDataManager`）

**写死：权威表在 sync 库，不并入 `HostSync`；viewhost 只做 UI。**

`HostDataManager`（`namespace aerovista::sync`）是 Host 侧任务状态门面：维护按报文族划分的权威表（首版仅实体），用 `AeroVistaConfig` 读 `entities.json` **子集**（`id` / `name` / `model` / `initialEntityState` / `pose.ellipsoid`；`pose.local` 与完整双轨仍由 engine `loadEntitiesFile` 服务 IG 预建）。物理文件仍放 engine 资源目录（[实体管理设计.md](../引擎基础功能/实体管理设计.md) §4），**不把文件迁进 sync 库**。

- **做**：建表、运行期更新、`snapshot()`、按当前行填 CIGI 报文对象。
- **不做**：`initialize` socket、`flushTcp` / `flushUdp`、ready 判定、每帧眼点。这些归 `HostSync` / `HostDriver`。
- **消费方**：`aerovista::sync::HostDriver` 持有 `HostSync` + `HostDataManager`（[viewhost设计.md](../viewhost设计.md) §4.0）；`engine/Tests` 直接测 Manager（不启网络）或测 `HostDriver`（`PLT-ig-first` 等）。建表码 `ENT-04-table-*`；运行期更新 / 组包码 `ENT-04-update-*` / `ENT-04-pack-*`（[实体与运动控制设计.md](./实体与运动控制设计.md) §11）。IG 完整 schema 见 [实体管理设计.md](../引擎基础功能/实体管理设计.md) §4。

与 IG 侧对称关系：`IgSync`（传输）+ `SynchronSystem`（决策）；Host 侧为 `HostDriver`（持 `HostSync` + `HostDataManager` + 可选虚 `IgSync`）。`HostDataManager` 不是第二个 `SynchronSystem`（不做眼点合成），只承担 CIGI 任务状态的 last-value。

## 4. 配置设计

### 4.0 配置结构

配置文件有两块（**本地调试** viewhost 只带 `hostConfig`，engine 只带 `igConfig`）。**否决的是 engine 兼 Host**（engine 配置不得含 `hostConfig`，2026-08 拆进程）。真模拟器中继时 **viewhost** 可同时有 `hostConfig`（对真实 IG listen）+ `igConfig`（虚 IG 连平台）+ `relay.enable` / `expectedIgCount`，见 [平台同步设计.md](../平台同步设计.md) §9——这不是把 Host 搬回 engine。

```jsonc
// viewhost（Host-only）
{ "hostConfig": { "udpPortRecv": 8000, "tcpPort": 8100 } }

// engine（IG-only）
{
  "igConfig": {
    "udpPortRecv": 8005,   // 本地 UDP 接收
    "targetAddr": "127.0.0.1", "targetTcpPort": 8100, "targetUdpPortRecv": 8000  // 远端 Host
  }
}
```

**字段语义**：
- `hostConfig`：Host 本地传输参数（`udpPortRecv`/`tcpPort`）。
- `igConfig`：IG 本地 UDP 接收端口（`udpPortRecv`）+ 远端 Host 目标（JSON：`targetAddr`/`targetTcpPort`/`targetUdpPortRecv` → C++ `HostTarget`）。

**设计理由**：
- 本地 UDP 接收 / TCP 监听**固定绑定所有网卡**（`INADDR_ANY`，即 `0.0.0.0`）；`targetAddr` 才是可配的远端 Host 目标。
- 已否决 `bindAddr` 字段：实现从未消费「本地绑定网卡」（接收/监听均写死 `INADDR_ANY`），移除以免误导「改配置即可限网卡」。
- 已删除 `udpPortSend`：发送走已 bind 的接收 socket，源端口 = `udpPortRecv`（CIGI UDP_SYNC 用 fromPort 对 HELLO）；JSON 残留为未知键拒绝。
- IG 侧一个配置块自洽（本地 + 远端），viewhost 侧一个配置块自洽，两端配置简单。
- 远端字段加 `target` 前缀，避免与本地同名端口字段冲突。

**校验规则**：`requireConnectedIg` 无 `igConfig` 拒绝；`igConfig` 缺 target 字段、未知键（如 `tcpPort`、`udpPortSend`、`targetUdpPortSend`）拒绝。engine 配置若含 `hostConfig` 属未知键 → 拒绝（`hostConfig` 只存在于 Host 进程配置，engine 不再解析）。

**C++ 类型**：`HostTarget`（`addr` / `tcpPort` / `udpPortRecv`）嵌在 `IgConfig::target`；JSON 键仍扁平 `targetAddr` / `targetTcpPort` / `targetUdpPortRecv`（避免与本端 `udpPortRecv` 撞名）。`HostConfig` 传输字段：`udpPortRecv` / `tcpPort`；中继另含 `relay`（`enable` / `expectedIgCount`）与可选 `igConfig`（`relay.enable=false` 时为空）。`messageSync` / `masterChannelId` 见 [状态同步设计.md](./状态同步设计.md) §3.1，**尚未入结构体**；写入 JSON 会因未知键拒绝。

### 4.1 host/ig 独立读取配置（viewhost / 独立 IG 进程）

viewhost（纯 Host）与独立 IG 进程（外部引擎挂载 sync，不用引擎整体配置）分别从**独立配置文件**读取各自的传输参数初始化，不依赖引擎侧配置。

**实现**：
- `aerovistaSync` 通过独立库 `AeroVistaConfig`（`thirdparty/config`）读 JSON：语法走 nlohmann/json v3.12.0，契约辅助（`find`/`require*`/`rejectUnknownKeys`）在 `aerovista::config`。零 vsg 零引擎依赖。
- 库内两个对称入口：
  - `loadHostConfig(path, HostConfig&, error)`：解析 Host 进程配置。本地调试只含 `hostConfig`；中继时另含 `relay` / `igConfig`（[平台同步设计.md](../平台同步设计.md) §9）。
  - `loadIgConfig(path, IgConfig&, error)`：解析只含 `igConfig` 块的文件（独立 IG / 外部引擎）。
- viewhost（纯 Host）用法：直接持 `HostSync` 传输层（不经 IG 收发端点 `SynchronSystem`），`initialize` 起 accept/UDP 线程 + `run` 置 RUNNING，每帧 `outMsgWithIgCtrlUdp() << 眼点 → flushUdp()` 扇出（IGCtrl 帧号/时间戳由 `outMsgWithIgCtrlUdp()` 自动填充，§7.1）：

```cpp
HostConfig host;
loadHostConfig("viewhost.json", host, &error);
HostSync hostSync;
hostSync.initialize(host);
hostSync.run();
// 每帧：auto& omsg = hostSync.outMsgWithIgCtrlUdp();
//       cigi_wire::appendEye(omsg, eye);
//       hostSync.flushUdp();
```

- 独立 IG 用法（外部 engine 挂载，可自由选择配置来源）：

```cpp
IgConfig ig;
SyncSystemConfig syncSystem;
loadIgConfig("ig.json", ig, &error);
SynchronSystem::create()->initialize(std::optional<IgConfig>{ig}, syncSystem);
```

**配置形态**（schema 与 engine 侧块一致，包裹方案）：
```jsonc
{ "hostConfig": { "udpPortRecv": 8000, "tcpPort": 8100 } }
{ "hostConfig": { "udpPortRecv": 8000, "tcpPort": 8100 },
  "relay": { "enable": true, "expectedIgCount": 3 },
  "igConfig": { "udpPortRecv": 8002, "targetAddr": "10.0.0.5",
                "targetTcpPort": 9100, "targetUdpPortRecv": 9000 } }
{ "igConfig": { "udpPortRecv": 8005,
                "targetAddr": "127.0.0.1", "targetTcpPort": 8100, "targetUdpPortRecv": 8000 } }
```

`hostConfig` 可选 `messageSync` / `masterChannelId`（缺省 FreeRun；会话中途不切）见 [状态同步设计.md](./状态同步设计.md) §3.1。实现前 JSON 多写这两键会因未知键拒绝。

**解析器分层**：JSON **语法**走 nlohmann/json；**契约辅助**（`parseJsonText`/`find`/`requireInt`/`rejectUnknownKeys` 等）归独立库 `AeroVistaConfig`（`namespace aerovista::config`），engine 与 sync **共用**。`loadHostConfig`/`loadIgConfig`/`parseHostConfig`/`parseIgConfig` 仍归 sync（sync 自己的结构体）；引擎窗口/实体/相机 schema 仍在 `EngineConfig.cpp`。引擎不重复实现 `parseHostConfig`/`parseIgConfig`。

**`requireInt` 严格整数**：整数字段（端口、窗口、实体 `id` 等）拒绝小数（`1.5`）；JSON 写成 `1.0` 仍视为整数。sync 侧与引擎侧行为一致。

**验收测试**：
- `HostIGTests.cpp` 的 `[viewhost]` 场景——`loadHostConfig` 读 host-only 配置 → 直接持 `HostSync`（`initialize(host)` + `run`）拉起，与带 IG 的 Engine 真实 TCP/UDP 握手 + CIGI IGCtrl→SOF 收发。
- `HostIGTests.cpp` 的 `[standalone]` 场景——**host 与 IG 双侧都走 sync 库独立配置文件**（host 侧 `loadHostConfig` → `HostSync`；IG 侧 `loadIgConfig` → `SynchronSystem`），IG 侧装配参数程序化注入（`cameraDriver().setOffsetDeg`），双通道 CIGI 收发。
- `EngineConfigTests.cpp` 的 `loadHostConfig` / `loadIgConfig` 单元用例（正常解析 / 未知顶层键拒绝 / 部分对象拒绝 / `CFG-host-relay-*`）。

### 4.2 `syncSystem` 配置组（SynchronSystem 装配属性）

`channelId`/`offsetDeg`/`requireConnectedIg` 是 **`syncSystem` 组字段**，与 engine 渲染属性、host/ig 传输属性正交。独立成组后 engine 属性 / syncSystem 属性 / ig·host 属性三类分明，配置项可自由组合成不同配置文件。

**配置形态**：
```jsonc
{
  "syncSystem": {
    "channelId": 0,
    "offsetDeg": { "yaw": 0.0, "pitch": 0.0, "roll": 0.0 },
    "requireConnectedIg": true
  },
  "igConfig": { ... },
  "model": ..., "window": ...     // engine 渲染属性，不进 syncSystem
  // "hostConfig" 仅存在于 Host 进程配置（viewhost.json），engine 配置不含（2026-08 拆进程）
}
```

**归属边界**：
- `syncSystem` 组：`requireConnectedIg` 由 SynchronSystem 消费（connect 失败是否拒绝）；`channelId` 由 IgSync **HELLO 上报**（= 本端 `syncSystem.channelId`；[平台同步设计.md](../平台同步设计.md) §6.3）；`offsetDeg` 由 Engine `CameraDriver` 消费。
- `hostConfig`/`igConfig` = 传输参数（sync 库，§4.1）。
- `model`/`window`/`entitiesFilePath`/`camera`/`injectEllipsoidIfMissing`/`groundGrid` = engine 渲染属性（不进 sync）。

**消费路径**：`SynchronSystem::initialize(igConfig, syncSystem)`：`requireConnectedIg` 决定 connect 失败是否拒绝；`channelId` 由 `IgSync` 填入 CIGI HELLO。engine 传入 `config.igConfig` + `config.syncSystem`；`offsetDeg` 注入 `CameraDriver`。运行时联调标定用 `cameraDriver().setOffsetDeg`。viewhost 纯 Host 可缺省 `syncSystem` 组（默认值全 0/false）。虚 IG 对平台的 HELLO 不走该字段，填 `0`。

> **配置格式统一**：JSON 顶层不保留旧扁平字段（`channelId`/`offsetDeg`/`requireConnectedIg` 已并入 `syncSystem` 组）。`EngineChannelConfig` 与 JSON 一一对应（`syncSystem`/`igConfig`/`model`/`window`/`injectEllipsoidIfMissing`/`entitiesFilePath`/`camera`/`groundGrid`；`hostConfig` 仅 Host 进程配置，2026-08 拆进程后 engine schema 不再含它）。`hostEyeStalePolicy` 已删除（未知键拒绝）。

## 5. 否决与决策记录

- **`SyncCameraTarget` 接口已否决**：`Engine` 继承相机目标接口语义不搭，且运行期「你传我、我调你」有回环感。改用纯数据流（§3.1）。未来若有人考虑回调式接口，先读此否决。
- **`Network`（Boeing MPV，GPL）不使用**：UDP 收发统一走自有的 `UdpSocket`（GPL 依赖清除）。
- **命令面桥不做接口解耦**：引擎 → sync 库方向的直调不构成反向依赖（§3.3）。
- **`SyncRoleConfig` 已删除（2026-08）**：拆 Host 进程后 `enableHost`/`hostConfig` 无消费方（`SynchronSystem` 只看 IG 半边，HostSync 独立 `initialize(HostConfig)`）；删结构体，`SynchronSystem::initialize` 改收 `std::optional<IgConfig>`（空 = 不启 IG；engine 传入 `config.igConfig`）。
- **`HostDataManager` 不并入 `HostSync`（2026-09）**：传输类不持实体/环境等任务状态；权威表单独类型，由 `HostDriver` 同时持有二者。未来若有人把 last-value 塞进 `HostSync`，先读 [viewhost设计.md](../viewhost设计.md) §4.0。
- **`HostDriver` 进库（2026-09）**：`HostDriver` = `HostSync` + `HostDataManager` + 可选虚 `IgSync`；viewhost 直接使用，含中继。否决 examples 第二套 `HostDriver`。否决并入 `HostSync`。
- **`HostEyeStalePolicy` / 双驱动器已删除（2026-09）**：断线门控与 ReuseLast/Freeze 相对收包即合成无生产差异；`CameraDriverBase`/`RawCameraDriver`/`CameraDriver` 三套收成单一 `CameraDriver`。JSON `hostEyeStalePolicy` 为未知键拒绝。
- **`CameraDriver` 不再回指 Engine（2026-09）**：驱动器只做 CCL 翻译与 compose；写相机由 `Engine::applyLastHostEye`。Engine 以值成员持有驱动器（不再 `unique_ptr`）。
- **椭球注入对象已否决（2026-08 / 2026-09）**：`SynchronSystem::setEllipsoidTransform(const EllipsoidTransform*)` 及 engine 侧 `VsgEllipsoidTransform` 适配器删除；`setEllipsoidMode(bool)` 场景模式注入亦随同步只 LLA（2026-09）删除——决策器无需几何对象或模式判据。预留用的 `SyncMath.h`（`EllipsoidTransform` / 其后的 `DVec3`）已删除，不再占公开边界。
- **`udpPortSend` 已删除（2026-09）**：发送走已 bind 的接收 socket，源端口 = `udpPortRecv`；配置残留为未知键拒绝。
- **`registerEventProcessor` 已删除（2026-09）**：业务订阅只走 `addCallback<PacketT>`；测试不再经 CCL `RegisterEventProcessor` 旁路。
- **测试用 HostFrame pack/unpack 已删除（2026-09）**：`packHostFrame` / `unpackHostFrame` / `unpackSof` 与 `HostFrame` 不再保留（含内部头）。数据面契约测 `HostSync`/`IgSync` 可观察收发。公开 `CigiWire.h` 保留生产 `packSof` / `appendEye` / `CigiFrameAssembler`。
- **`sofReceivedCount` 无副作用（2026-09）**：不再内部 `drainIncoming()`；Host 收包 push 模式须先显式 `drainIncoming` / `pollIncoming` 再读计数。

## 6. 问题自查（待讨论）

> 审查发现的未决问题先在此登记。未授权前不改公开接口。

| # | 问题 | 关联章节 | 状态 |
| --- | --- | --- | --- |
| 3 | **`cigi_wire::EyePose::entityId` 发送路径不读**：`appendEye` 写死 `EntityID=0`；`parentId` 发送同样写死 0。字段对生产发送接口无效。 | §3.0 同步只 LLA | 待讨论 |
| 9 | **`IgSync::drainIncoming(bool sendSof)` boolean 入参**：生产 `preFrame` 恒 `true`；`false` 只服务「不回 SOF」测试。是否拆成 `drainIncoming` / `drainIncomingWithoutSof`。 | §3.1 帧循环 | 待讨论 |
| 10 | **`HostSync::run()` 名实不符**：`initialize` 已起 accept/UDP 线程；`run()` 只把 `_status` 置 `RUNNING`。漏调则握手仍工作，但 `HostDriver::isRunning()` 为假。 | §4.1 生命周期 | 待讨论 |
| 11 | **`hasReadyIg()` 与 `readyIgCount()` 冗余**：生产（viewhost）只用 count；`hasReadyIg` 仅测试。 | 状态观测 | 待讨论 |

---

## 7. 验收要点

> 对齐 [测试用例书写规范.md](../../测试用例书写规范.md)。对照 `EngineConfigTests.cpp` / `TcpSocketTests.cpp` / `UdpSocketTests.cpp`。码一经分配不改号、不复用、不重排。Catch2 挂同名 tag。
>
> engine 通道 JSON（window / `igConfig` 嵌在通道文件）见 [多通道同步模块设计.md](./多通道同步模块设计.md) §11。本节只登记 **Host/IG 独立配置入口** 与 **库内套接字**。

| 码 | 场景 | 验收 | Catch2 |
| --- | --- | --- | --- |
| `CFG-host-parse-ok` | 解析 host-only | `loadHostConfig` 读出 `udpPortRecv` / `tcpPort`（§4.0）；`relay.enable==false` 且 `igConfig` 为空；节拍键落地后由 `MSYNC-*`（[状态同步设计.md](./状态同步设计.md) §10）覆盖，不扩写本行 | `[unit][config][sync][host][CFG-host-parse-ok]` |
| `CFG-host-reject-unknown` | Host 未知顶层键 | 拒绝（`hostConfig` / `relay` / `igConfig` 以外） | `[unit][config][sync][host][CFG-host-reject-unknown]` |
| `CFG-host-reject-partial` | 半填 hostConfig | 方案 A：对象出现则子字段全必填 | `[unit][config][sync][host][CFG-host-reject-partial]` |
| `CFG-host-relay-off` | 中继关闭 | `enable` 缺省或 false：解析成功，`relay.enable==false`，`igConfig` 为空（JSON 带完整 `igConfig` / `expectedIgCount` 也忽略） | `[unit][config][sync][host][CFG-host-relay-off]` |
| `CFG-host-relay-on` | 中继打开 | `enable=true` 且 `expectedIgCount≥1` 且完整 `igConfig`：读出开关、台数与虚 IG 本端/平台 target；`igConfig.udpPortRecv` ≠ `hostConfig.udpPortRecv` | `[unit][config][sync][host][CFG-host-relay-on]` |
| `CFG-host-relay-need-ig` | 中继缺 igConfig | `enable=true` 但无 `igConfig` 或半填 → 拒绝 | `[unit][config][sync][host][CFG-host-relay-need-ig]` |
| `CFG-host-relay-need-count` | 中继缺台数 | `enable=true` 但无 `expectedIgCount` → 拒绝 | `[unit][config][sync][host][CFG-host-relay-need-count]` |
| `CFG-host-relay-count` | 台数非法 | `enable=true` 且 `expectedIgCount` 不是 ≥1 的整数 → 拒绝 | `[unit][config][sync][host][CFG-host-relay-count]` |
| `CFG-host-relay-port` | 虚 IG 与 Host UDP 撞口 | `enable=true` 且两 `udpPortRecv` 相同 → 拒绝 | `[unit][config][sync][host][CFG-host-relay-port]` |
| `CFG-host-relay-unknown` | relay 未知子键 | 拒绝 | `[unit][config][sync][host][CFG-host-relay-unknown]` |
| `CFG-ig-parse-ok` | 解析 ig-only | `loadIgConfig` 读出本地 recv + target（§4.0） | `[unit][config][sync][ig][CFG-ig-parse-ok]` |
| `CFG-ig-reject-unknown` | IG 未知顶层键 | 拒绝 | `[unit][config][sync][ig][CFG-ig-reject-unknown]` |
| `CFG-ig-reject-partial` | 半填 igConfig | 方案 A | `[unit][config][sync][ig][CFG-ig-reject-partial]` |
| `TCP-loopback-connect` | TCP 建连 | listen/accept/connect loopback | `[unit][TCP-loopback-connect]` |
| `TCP-send-recv-all` | TCP 完整收发 | `sendAll`/`recvAll` 双向完整字节 | `[unit][TCP-send-recv-all]` |
| `TCP-peer-closed` | 对端关闭 | `recv` 返回 `PEER_CLOSED` | `[unit][TCP-peer-closed]` |
| `TCP-recv-timeout` | 读超时 | 无数据 → `TIMEOUT` | `[unit][TCP-recv-timeout]` |
| `TCP-connect-fail` | 未监听 | connect 失败 | `[unit][TCP-connect-fail]` |
| `TCP-accept-peer-ip` | accept 对端 | 回填对端 IPv4 | `[unit][TCP-accept-peer-ip]` |
| `TCP-close-idempotent` | close 幂等 | 重复 close 安全 | `[unit][TCP-close-idempotent]` |
| `TCP-wsa-refcount` | WSA 引用计数 | 并发 create/destroy 不破坏引用计数 | `[unit][stress][TCP-wsa-refcount]` |
| `UDP-loopback` | UDP 自环 | `sendTo`/`recv` loopback | `[unit][UDP-loopback]` |
| `UDP-recvfrom-peer` | recvFrom 源地址 | 回填源 IP 与端口 | `[unit][UDP-recvfrom-peer]` |
| `UDP-send-bad-arg` | 非法 sendTo | 非法参数返回 -1 | `[unit][UDP-send-bad-arg]` |
| `UDP-uninit` | 未初始化 | recv/sendTo 返回 -1 | `[unit][UDP-uninit]` |
| `UDP-port-busy` | 端口占用 | `initialize` 冲突返回 false | `[unit][UDP-port-busy]` |
| `UDP-close-idempotent` | close 幂等 | 重复 close 安全 | `[unit][UDP-close-idempotent]` |
