# viewhost 设计（MFC Host 宿主程序）

> **已按新接口同步（2026-08-24；2026-08-25 IGCtrl 自动填充）**：`HostSync::update`/`EyePose` 已删除，`HostDriver::update` 改用 `outMsgWithIgCtrlUdp() + cigi_wire::appendEye + flushUdp()`（[状态同步设计初版.md](./状态同步设计初版.md) §7.1）——`outMsgWithIgCtrlUdp()` 自动前置 IGCtrl（帧号/自计时时间戳/`TimeStampValid=true`）；眼点类型为 **`cigi_wire::EyePose`**（`frame` 枚举替代 `isLla` 布尔）。本文正文已全部对齐。

面向「用 MFC 对话框程序作为独立 Host 进程，经 `aerovistaSync` 的 `HostSync` 向多个携带 IG 的 Engine 扇出同一 Host 眼点，模拟多通道同步」的设计。

基础协议 / 行为见 [多通道同步模块设计.md](./多通道同步模块设计.md)；库结构与接入见 [sync模块化设计.md](./sync模块化设计.md) 与 [../../thirdparty/sync/README.md](../../thirdparty/sync/README.md)。

> 本文档描述设计决策与现状，不记录变更历史。

## 目录

1. [目标与边界](#1-目标与边界)
2. [需求澄清结论（多通道在 IG 侧）](#2-需求澄清结论多通道在-ig-侧)
3. [程序形态与目录](#3-程序形态与目录)
4. [功能与数据流设计](#4-功能与数据流设计)
5. [配置设计](#5-配置设计)
6. [测试策略](#6-测试策略)
7. [与现有文档关系](#7-与现有文档关系)
8. [否决与决策记录](#8-否决与决策记录)
9. [与实现关系](#9-与实现关系)

---

## 1. 目标与边界

### 1.1 目标

1. 提供一个独立的 Windows MFC 宿主程序 `viewhost`，作为多通道同步的 **Host 端数据源**，向携带 IG 的 Engine 进程扇出同一 Host 眼点。
2. 复用 `aerovistaSync` 的 `HostSync`（不重写网络 / 协议 / 线程模型），只做「宿主壳」：读配置 → 启动 `HostSync` → 按帧驱动业务侧组装扇出 → 显示连接状态。
3. 落地 [多通道同步模块设计.md](./多通道同步模块设计.md) §1.1 的「HostSync 独立进程」远期项（**已落地 2026-08**：Engine 不再承担 Host，`HostPosePublisher` 删除，HostSync 独立运行于本进程；与 viewhost 配套的 IG 配置为 `viewhost_ig_*.json` / `scene_ecef_ig_*.json`）。

### 1.2 非目标

- 不实现 IG 侧渲染 / 决策（那是 `Engine` + `SynchronSystem` 的职责）。
- 不改动 sync 库的协议、线程模型、载荷布局（CIGI V4 + TCP/UDP 双平面不变）。
- 不做边缘融合、精标定、精时钟（RTT / PTP）。
- 不做 TCP 命令面（加载 / 切库 / 复位）——那是后续，见 [多通道同步模块设计.md](./多通道同步模块设计.md) §9。

### 1.3 与 sync 库的关系

`HostSync` 是**零 vsg 依赖**的纯 C++ 类型（Winsock + 标准库），MFC 程序链接无阻碍。数据面发送接口（新契约，[状态同步设计初版.md](./状态同步设计初版.md) §7.1）：

```cpp
// HostDriver::update —— 业务侧组装数据面帧节拍（IGCtrl 自动 + 眼点）后统一 flushUdp
auto& omsg = _host.outMsgWithIgCtrlUdp();        // 自动前置 IGCtrl（帧号=数据面、TimeStamp=自计时、TimeStampValid=true）
cigi_wire::appendEye(omsg, eye);         // 可选：追加 ownship 眼点
_host.flushUdp();                        // PackageMsg + 扇出
```

- `outMsgWithIgCtrlUdp()` 自动前置 IGCtrl 并填充帧号/时间戳（`HostSync` 自计时，2026-08-25）；帧号由 `outMsgWithIgCtrlUdp()` 内部数据面计数器自动分配。
- `cigi_wire::appendEye`（`CigiWire.h`）负责 ownship 眼点的 CCL 组装（WorldLocal→Attach+XYZ / LLA→Detach+LLA，LLA 越界丢弃内置）。原 `appendHostFrame`（IGCtrl+眼点整体组装）已删——IGCtrl 归属 sync。
- 数据面与命令面统一走 CCL 会话；`HostSync::update`/`EyePose` 已删除。

依赖传递（[sync模块化设计.md](./sync模块化设计.md) §3.0）：

- `aerovistaSync` 公开链接 `cigicl-static` + `ws2_32`（`CigiWire` 打包 IGCtrl 用），PRIVATE 链接 `vsg::vsg`。
- `vsg` 仅用于 `SynchronSystem.cpp` 内 header-only 数学，**不产生 `vsg::` 外部符号**，不传给消费方。
- 因此 MFC 程序链接 `aerovistaSync` 时自动带上 `cigicl-static` + `ws2_32`，**无需** `vsg` 库。

## 2. 需求澄清结论（多通道在 IG 侧）

「多通道」由**各 IG 进程自己的 `channelId` + `offsetDeg`** 决定，viewhost **不感知**通道数与偏移。例证（`engine/resources/config/`）：

- `scene_ecef_ig_left.json`：`channelId: 1`、`offsetDeg.yaw: +18.05`。
- `scene_ecef_ig_right.json`：`channelId: 2`、`offsetDeg.yaw: −18.05`（对称负偏移）。

```text
viewhost（纯 Host）
  └→ HostSync（initialize + run + 每帧 HostDriver::update 扇出同一眼点）
       ├→ IG A（Engine，channelId=0，offset 0）
       ├→ IG B（Engine，channelId=1，offset +18.05°）
       └→ IG C（Engine，channelId=2，offset −18.05°）
        各 IG 自己 Host ⊕ offsetDeg → setCameraPose
```

viewhost 侧逻辑因此很单纯：持**一个** `HostSync`，把同一个眼点扇出给所有 ready IG，每个 IG 自己加偏移。等价于现有 [`minimal_viewhost.cpp`](../../thirdparty/sync/examples/minimal_viewhost.cpp) 的 MFC GUI 版。

## 3. 程序形态与目录

- **形态**：基于 `CDialog` 的对话框控制台——键盘操控眼点 + 连接状态显示。
- **选型理由**：viewhost 是控制台性质的 Host 数据源，无文档保存 / 多文档 / 打印需求，也不需要 Doc/View 框架自带的菜单栏 / 工具栏 / 状态栏；用对话框控件做参数输入与状态展示最轻量、最快落地。其余 MFC 形态（SDI / MDI / CFormView）的对比见实现阶段可选的替代（本设计写死选 `CDialog`）。
- **目录**（作为 sync 库接入示例，放 `examples/`，与现有 `minimal_viewhost.cpp` 并列）：

```text
thirdparty/sync/examples/viewhost/
  CMakeLists.txt              # if(MSVC) 守卫；链接 aerovistaSync
  src/
    ViewHostApp.h/.cpp        # CWinApp 派生
    ViewHostDlg.h/.cpp        # CDialog 派生：主控制台（UI + 定时器）
    HostDriver.h/.cpp         # HostSync 封装：生命周期 + 帧驱动 + 状态读取
    ViewHostMath.h/.cpp       # 纯 C++：applyManualStep（可测）
  resources/
    ViewHost.rc / resource.h  # 对话框 / 字符串表
    viewhost.json             # hostConfig（复用 loadHostConfig）
```

**命名约定**：

- MFC 框架派生类遵循 MFC 惯例（`CViewHostApp`、`CViewHostDlg`，C 前缀 + PascalCase），属 MFC 类型族，与 `cpp-vsg-style` 规则不冲突（该规则约束 engine 业务代码）。
- 业务逻辑类（`HostDriver`）置于 `namespace aerovista::viewhost`，方法 `camelCase`、私有成员 `_camelCase`，符合项目风格。

## 4. 功能与数据流设计

### 4.1 启动流程

对齐 [`minimal_viewhost.cpp`](../../thirdparty/sync/examples/minimal_viewhost.cpp)：

```text
1. loadHostConfig(viewhost.json, host, &error)      // sync 库内解析，只含 hostConfig 块
2. HostSync::initialize(host)                        // bind UDP + TCP listen，起 accept/UDP 线程
3. HostSync::run()                                   // 置 RUNNING（一次，非每帧）
4. 定时器按目标 fps 调 HostDriver::update(&eye)  // 每帧 outMsgWithIgCtrlUdp<<眼点→flushUdp
5. HostSync::shutdown()                              // 退出时收尾
```

### 4.2 眼点表示与平移参考系（默认 LLA）

**默认 LLA（已选定）**：眼点用椭球模式 LLA 表示，`cigi_wire::EyePose.frame = EyeFrame::LLA`，字段语义见 [lla位姿传输设计.md](./lla位姿传输设计.md) §3.1 / §3.2 与 `CigiWire.h`：

| EyePose 字段 | LLA 语义 |
| --- | --- |
| `frame` | `EyeFrame::LLA`（Detach+LLA 编码） |
| `x` | 纬度 lat（度，`[-90, 90]`） |
| `y` | 经度 lon（度，`[-180, 180]`） |
| `z` | 海拔 alt（米，相对椭球面） |
| `yawDeg` / `pitchDeg` / `rollDeg` | 当地 **ENU** YPR（东-北-天；`yaw=0` 朝北，`+yaw` 左转朝西） |

> **术语澄清**：viewhost 作为 Host 端发的是 **LLA**（`frame=LLA`），**不是** ECEF。ECEF（地心米制笛卡尔）是 IG 侧椭球场景的渲染工作坐标（[lla位姿传输设计.md](./lla位姿传输设计.md) §2）；`cigi_wire::EyePose` 只有 `frame` 枚举（WORLD_LOCAL / LLA），无「直接发 ECEF」选项。默认 LLA 即配合 engine 椭球场景（`scene_ecef_*.json`，`coordFrame: "Ellipsoid"`）。

**平移参考系（机头局部 + 绝对垂直）**：

- WASD「前后左右」= 沿当前 `yaw` 的**机头局部水平面**平移（随朝向旋转），非地理固定 N/S/E/W。
- C/E「上下」= 绝对垂直（`alt` 增减）。
- 方向键 = 姿态（yaw / pitch），不产生平移。

**局部平移 → LLA 增量（初版局部平面近似）**：每帧平移量小，用 WGS-84 简化球近似（1° lat ≈ 111320 m；1° lon ≈ 111320·cos(lat) m）：

```text
forward_enu = (−sin yaw, cos yaw)     // ENU 基：X=East, Y=North（lla §3.2）
right_enu   = (cos yaw,  sin yaw)
Δnorth = forward_north·dFwd + right_north·dRight
Δeast  = forward_east·dFwd  + right_east·dRight
lat += Δnorth / 111320
lon += Δeast  / (111320 · cos(lat))
alt += dUp
```

- `dFwd` / `dRight` / `dUp` 为每帧位移（米），由按键映射产生（§4.5）。
- 经度越界按 [lla位姿传输设计.md](./lla位姿传输设计.md) §5 normalize 到 `(-180, 180]`；**纬度 clamp 到 `[-89.9, 89.9]`**（避免 lat 越界触发 CCL bound check 拒包，同时避免 `cos(lat)→0` 使经度增量除零）；pitch 越界同样 clamp 到 `[-89.9, 89.9]`；**yaw 累加后 normalize 到 `(-180, 180]`**（避免方向键持续偏航导致 yaw 无限增长、`float` 精度劣化——`CigiWire.cpp` 的 `llaEyeInRange` 不校验 yaw，见 §4.5）。
- **极区数值说明**：clamp 后 `cos(lat)` 最小约 `cos(89.9°) ≈ 0.00175`，经度增量会放大约 570 倍，但**不会除零**；经度增量后仍 normalize，数值安全，仅「单位米对应的经度分辨率」随纬度升高而降低——属局部平面近似的固有精度损失，非错误。

### 4.3 帧节拍与线程模型

- **HostSync 内部已有线程**：`_acceptThread`（TCP accept）、`_udpThread`（UDP 收 SOF / 握手）、`_clientThreads`（每 client 一个）。viewhost **不额外造网络线程**。
- **扇出驱动**：`HostDriver::update`（内部 `outMsgWithIgCtrlUdp() << IGCtrl << 眼点 → flushUdp()`）是 UDP 非阻塞扇出（FreeRun，不等 SOF），不会长时间占用调用线程。**初版写死：用 MFC `SetTimer`（约 60fps）在 UI 线程驱动 `update()`**，对齐示例的「主循环 + sleep」模式，实现最简。
- 若未来需要更高节拍稳定性，再迁移到专用工作线程 + `PostMessage` 回传状态（本版不做）。

**本地时钟与帧增量（写死）**：

- `_simTimeMs` = UI 定时器本地 `std::chrono::steady_clock` 流逝毫秒（单调连续，不随 UI 卡顿回退），**仅用于按键步进 dt 归一化**；模拟时间戳不由它产生——由 `outMsgWithIgCtrlUdp()` 的 HostSync 自计时填充（[状态同步设计初版.md](./状态同步设计初版.md) §7.1）。
- `_moveStep` / `_turnStepDeg` **不按「假设 60fps」固定值**，而是按**实际 dt** 归一化：`dt = 本帧 _simTimeMs − 上帧 _simTimeMs`（秒）；`_moveStep = speed(m/s) · dt`，`_turnStepDeg = rate(°/s) · dt`。避免 `SetTimer` 周期不精确导致速度随负载漂移。
- `speed` / `rate` 为程序内可调常量（初版默认如 `speed = 30 m/s`、`rate = 60 °/s`）。

### 4.4 键盘输入驱动 EyePose（手动操控）

**交互模式（已选定）**：

- 单一 toggle 按钮，文字随状态在「开始控制」/「停止控制」间切换，并用状态文字/颜色指示「操控中 / 空闲」。
- 语义 = **键盘操控的启用/禁用**：点击「开始控制」后键盘接管眼点（WASD/CE/方向键生效）；点击「停止控制」后键盘不再响应，眼点保持当前值不变。
- 键盘接管开关同时解决「`GetAsyncKeyState` 是全局物理状态、会与文字输入冲突」的问题——仅 `_controlling == true` 时才轮询按键。

**键盘读取的三个坑（MFC 对话框特有）**：

1. **焦点**：对话框含子控件时，`WM_KEYDOWN` 先发给焦点控件而非对话框，重写 `OnKeyDown` 常常收不到。→ 用 `GetAsyncKeyState` 轮询物理键状态，与焦点无关。
2. **Enter/ESC**：`CDialog` 默认 Enter→`OnOK`、ESC→`OnCancel` 会关闭对话框。→ 重写 `OnOK` / `OnCancel` 为空，避免按 ESC 误关。
3. **连续按键**：`WM_KEYDOWN` 有按下延迟与重复间隔，「按住持续移动」不跟手。→ 在定时器里每帧 `GetAsyncKeyState` 轮询，算增量。

**并入 §4.3 的 60fps 定时器（同一拍轮询 + 扇出）**：

```cpp
void ViewHostDlg::onTick()
{
    // 1. 仅手动操控中才轮询按键
    if (_controlling)
    {
        double dFwd = 0.0, dRight = 0.0, dUp = 0.0;
        double dyaw = 0.0, dpitch = 0.0;

        if (GetAsyncKeyState('W') & 0x8000) dFwd   += _moveStep;
        if (GetAsyncKeyState('S') & 0x8000) dFwd   -= _moveStep;
        if (GetAsyncKeyState('A') & 0x8000) dRight -= _moveStep;
        if (GetAsyncKeyState('D') & 0x8000) dRight += _moveStep;
        if (GetAsyncKeyState('E') & 0x8000) dUp    += _moveStep;
        if (GetAsyncKeyState('C') & 0x8000) dUp    -= _moveStep;

        if (GetAsyncKeyState(VK_LEFT)  & 0x8000) dyaw   += _turnStepDeg;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) dyaw   -= _turnStepDeg;
        if (GetAsyncKeyState(VK_UP)    & 0x8000) dpitch += _turnStepDeg;
        if (GetAsyncKeyState(VK_DOWN)  & 0x8000) dpitch -= _turnStepDeg;

        applyManualStep(_eye, dFwd, dRight, dUp, dyaw, dpitch);  // §4.2 换算到 lat/lon/alt/ypr
    }

    // 2. 扇出（_controlling 为 false 时 _eye 不变，仍持续扇出当前眼点）
    //    update 只收眼点；IGCtrl 帧号/时间戳由 outMsgWithIgCtrlUdp() 自计时自动填充。
    _hostDriver.update(&_eye);
}
```

- `_controlling` = toggle 按钮状态；`_moveStep` / `_turnStepDeg` 为每帧增量（按实际 dt 归一化，见 §4.3，单位米/度）。
- `applyManualStep(eye, dFwd, dRight, dUp, dyawDeg, dpitchDeg)` 为**自由函数**（`aerovista::viewhost`），按 §4.2 把 `dFwd`/`dRight`/`dUp` 换算成 lat/lon/alt，把 `dyaw`/`dpitch` 累加到 YPR；从当前 `_eye` 累积。

**`_eye` 初始化（写死，与默认 LLA 对齐）**：

- `_eye` 构造后**立即初始化**为：`frame = EyeFrame::LLA`，位置 = 初始演示眼点（lat/lon/alt，位于模型群附近），`yaw = 0`、`pitch = roll = 0`。**禁止**用 `cigi_wire::EyePose` 默认构造（其 `frame=WORLD_LOCAL` 会与默认 LLA 矛盾，首帧发出 WorldLocal）。
- 进入手动模式：从「当前 `_eye`」起始累积，保证切入手动瞬间眼点不跳变。

**键盘切换控制（空格热键，写死）**：

- **空格** = 切换「开始控制 / 停止控制」，与点击 toggle 按钮等价（同一 `OnToggleControl`）。
- 实现：重写 `PreTranslateMessage`，在消息派发给任何控件前拦截 `WM_KEYDOWN` + `VK_SPACE`，调 `OnToggleControl()` 并 `return TRUE` 吞掉该消息。
- **为何用 `PreTranslateMessage` 而非 `OnKeyDown`**：对话框焦点在子控件上时 `WM_KEYDOWN` 不路由到对话框（见上面「三个坑」之 1）；`PreTranslateMessage` 全局先拦截，与焦点无关。`return TRUE` 吞掉消息避免焦点在按钮上时空格被当成「触发按钮」二次消费。
- **助记键冲突处理（已落地）**：toggle 按钮**不带** `&S` 助记键——字母助记键会被 `IsDialogMessage` 直接激活按钮（按 S 会切换控制而非后退），故按钮文本为纯文字「开始控制」；`退出(&X)` 保留（X 不与操控键冲突）。

**按键映射（初版写死）**：

| 键 | 动作 |
| --- | --- |
| 空格 | 切换 开始控制 ↔ 停止控制 |
| W | 前进（机头方向，水平面） |
| S | 后退 |
| A | 左移（机头左侧） |
| D | 右移（机头右侧） |
| E | 上升（alt +） |
| C | 下降（alt −） |
| ← | 偏航左转（`+yaw` → 西） |
| → | 偏航右转（`−yaw` → 东） |
| ↑ | 俯仰抬头（`+pitch`） |
| ↓ | 俯仰低头（`−pitch`） |

> C/E 上下方向（E=上 / C=下）与 pitch 正负以实现时对齐 §4.2 的 ENU 约定为准，常量可调。

### 4.5 实体摆放（Host 控制 IG 实体位姿）

在对话框增加「实体摆放」区：`Entity ID` + `lat/lon/alt`（LLA）+ `yaw pitch roll` 编辑框 + 「摆放」按钮。点击后：

1. 读输入（yaw/pitch/roll 空格分隔解析，缺省 0）；
2. `HostDriver::sendEntityPose(id, lat, lon, alt, yaw, pitch, roll)`——经 `outMsgWithIgCtrlTcp()`（自动前置 IGCtrl，命令面帧号/`TimeStampValid=false`）组装 `CigiEntityPositionCtrlV4`（**Detach+LLA，`EntityID≠0`**）→ `flushTcp()` 一次性精准下发；
3. 状态栏显示最近摆放值。

IG 侧消费：engine `initSync` 订阅 `subscribe<CigiEntityPositionCtrlV4>`，过滤 ownship（`EntityID==0`）后按 Detach→`updateEntityPose(id, lla, ypr, ELLIPSOID)` 更新实体位姿（状态同步设计初版.md §12）。

- 命令面走 **TCP**（一次性、可靠送达，§8.5 链路选择），与数据面眼点（UDP 持续）解耦。
- 与「眼点为何不用 ViewCtrl」同源：绝对 LLA 位姿只能用 `EntityPositionCtrlV4` Detach 表达（[cigi梳理.md](../notes/cigi梳理.md) 决策节）。

### 4.6 UI 状态显示

定时器刷新时从 `HostSync` 读取（线程安全，内部 atomic/mutex）：

| 指标 | 来源 |
| --- | --- |
| ready IG 数 | `readyIgCount()` |
| IGCtrl 发送轮次 | `igCtrlSentCount()` |
| SOF 接收数 | `sofReceivedCount()` |
| 当前眼点（lat/lon/alt, yaw/pitch/roll） | 键盘累积 `_eye` |
| 最近摆放实体 | `OnPlaceEntity` 写入的静态文本 |
| 最近测试报文 | `OnTestTcp` / `OnTestUdp` 写入的静态文本（§4.7） |
| 最近接收报文 | `pollIncoming` 解包后订阅回调写入的静态文本（§4.7，上行自检） |

### 4.7 报文自检（testtcp / testudp / F9 / F10）

**下行自检（Host→IG，viewhost 触发）**：在对话框增加「报文自检」区：`testtcp` / `testudp` 两个按钮 + 最近测试报文静态文本。点击后：

1. `HostDriver::sendRandomTcpPacket()` / `sendRandomUdpPacket()`——随机构造一个对应链路的测试报文（默认字段，仅 `EntityPositionCtrlV4` 补 `EntityID=7`）经 `outMsgWithIgCtrlTcp/Udp` → `flushTcp/flushUdp` 发送，返回报文类名；
2. 状态栏显示「最近测试: TCP/UDP <类名>」。

IG 侧对照：engine `initSync` 对 IgSync 已注册的**全部 Host→IG 报文**逐一 `subscribe`，收到即记录类名到 HUD「recv: <类名>」行（F2 开关帧统计）。两端类名一致 = 该报文「发送→链路→解包→投递」全链路支持（`cigi梳理.md` 链路矩阵）。

- 测试报文覆盖：TCP 命令面 34 种（一次性/配置/请求/符号类）+ UDP 数据面 4 种（持续/每帧控制类）；IGCtrl 与 ownship 眼点由常态化发送覆盖，不在此列。

**上行自检（IG→Host，engine 触发）**：engine 侧 `PacketProbeHandler`（原废弃 `CommandTriggerHandler` 改造）挂接窗口事件：

- **F9**：随机发一条 TCP 上行报文（IG→Host 16 类响应/通知，经 `outMsgWithSofTcp` → `flushTcp`）；
- **F10**：显式发 `CigiSOFV4`（`outMsgWithSofUdp` → `flushUdp`；IG→Host UDP 仅 SOF 一种，cigi梳理.md 链路矩阵）。

viewhost 侧：`HostDriver::pollIncoming()`（转发 `HostSync::drainIncoming`）在 UI 定时器每帧调用，`OnInitDialog` 中对 16 类 TCP 上行报文逐一 `subscribe`（`HostDriver::subscribe<T>` 转发 `HostSync::subscribe`），收到即刷新「最近接收」静态文本；F10 的 SOF 用已有「SOF 接收」计数确认（每帧自动回 SOF 亦计入）。engine HUD 追加「send: <类名>」行显示 F9/F10 发送结果。

- 上行覆盖：TCP 16 类（IGMsg/EventNotification/AnimationStop/HatHotResp/X/LosResp/X/SensorResp/X/PositionResp/WeatherCondResp/AerosolResp/Maritime/TerrestrialSurfaceResp/CollDetSeg/VolResp）+ UDP 1 类（SOF）。
- 纯调试工具，不改变协议语义；随机选择（`std::mt19937`），多次按键遍历覆盖。

## 5. 配置设计

- 复用 sync 库 `loadHostConfig`，配置形态与 `viewhost.json` 一致（顶层仅 `hostConfig` 块，未知键拒绝）：

```jsonc
{ "hostConfig": { "udpPortSend": 8001, "udpPortRecv": 8000, "tcpPort": 8100 } }
```

- 眼点默认 LLA（§4.2），`viewhost.json` 本身不含坐标系统开关；配合的 IG 侧场景由 engine 的 `viewhost_ig_*.json`（`coordFrame: "Ellipsoid"`）决定。

## 6. 测试策略

**分层原则**：

- **不测**：MFC UI（`CDialog` 消息循环 / `GetAsyncKeyState` 轮询）、`HostDriver`（`HostSync` 薄封装）。`HostSync` 的握手 / 扇出 / LLA 组包已由 `engine/Tests` 的 `HostIGTests`（`[viewhost]` / `[standalone]`）覆盖；UI 壳无新逻辑，测它成本高、价值低。
- **测（`[unit]`）**：viewhost 新增的**纯数值逻辑**——键盘步进→LLA 换算（§4.2 的 `forward_enu` / `right_enu` / lat·lon 增量）。这是现有测试覆盖不到的新逻辑，且边界易错：lat clamp、lon normalize 到 `(-180,180]`、`cos(lat)` 除零、yaw normalize。

**约束（写死）**：步进换算必须保持**纯 C++**——不依赖 MFC / vsg，只依赖 `cigi_wire::EyePose` 这一 POD 类型（include `CigiWire.h` 即可，不产生链接依赖），否则无法挂入 `engine/Tests`。

**挂载**：

- 新增 `engine/Tests/ViewHostMathTests.cpp`，加入 `engine/Tests/CMakeLists.txt` 的 `SOURCES`。
- 换算实现（`ViewHostMath.cpp`）也加入该测试 target 的编译单元——**测试与示例共用同一份源码**，不复制逻辑。
- 测试 target 已链接 `vsgEngineLib`（间接含 `aerovistaSync`，提供 `cigi_wire::EyePose`），无需新增链接。

## 7. 与现有文档关系

本文档落地后，已按 `doc-sync-on-refactor` 完成跨文档同步：

1. [多通道同步模块设计.md](./多通道同步模块设计.md) §1.1：补「独立 Host 进程示例已落地」（指向本文档）。
2. [sync模块化设计.md](./sync模块化设计.md) §1.3 非目标：改为「不做 Host 独立进程的协议 / 上行改造（viewhost 示例已落地）」。
3. [多通道同步模块设计.md](./多通道同步模块设计.md) §10 状态表 / §9 P2 / §0 表格 / §5 权威源：标注「Host 本地输入已有 viewhost 示例」，「指定输入 IG 上报」仍属后期。

## 8. 否决与决策记录

- **多通道在 IG 侧（澄清）**：viewhost 不感知通道数与 `offsetDeg`，只持一个 `HostSync` 扇出同一眼点。
- **扇出驱动走 UI 定时器（初版写死）**：`HostDriver::update`（`outMsgWithIgCtrlUdp+appendEye+flushUdp`）非阻塞，UI 定时器驱动最简；高节拍稳定性需求留待工作线程方案。
- **触发方式选 toggle 按钮（否决左键开始 / 右键结束）**：右键在 Windows 惯例为上下文菜单语义，且按钮控件对右键不产生点击通知，需在对话框层额外处理 `WM_RBUTTON*`；左/右键还缺状态可见性。改为单一 toggle 按钮（文字+颜色反映状态）承载「开始控制 ↔ 停止控制」。
- **键盘读取用 `GetAsyncKeyState` 轮询（否决 `OnKeyDown`）**：对话框焦点在子控件上时 `WM_KEYDOWN` 不路由到对话框，且按下有重复延迟；物理键状态轮询与焦点无关、连续输入跟手，但需 toggle 开关避免与文字输入冲突（§4.5）。
- **默认 LLA 眼点（非 ECEF）**：viewhost 发 `frame=LLA` 的 LLA（lat/lon/alt + 当地 ENU YPR），配合 engine 椭球场景；ECEF 仅是 IG 侧渲染坐标，`cigi_wire::EyePose` 无发 ECEF 选项（§4.2）。
- **程序放 `thirdparty/sync/examples/`**：viewhost 是 sync 库的 Host 接入示例，与 `minimal_viewhost.cpp` 并列，不进 `tools/`（§3）。
- **平移参考系 = 机头局部（否决地理固定 N/S/E/W）**：WASD 沿当前 `yaw` 的机头局部水平面移动，配合方向键 yaw/pitch 的姿态控制更符合「驾驶」直觉；上下用绝对垂直 alt（§4.2）。
- **测试范围分层（写死）**：UI / `HostDriver` 薄封装不测（`HostSync` 已由 `HostIGTests` 覆盖）；步进换算是新增纯数值逻辑，挂 `engine/Tests` 的 `[unit]` 测试，与示例共用同一份源码（§6）。
- **圆周轨迹已移除（决策）**：viewhost 只保留键盘手动操控眼点，不做自动圆周轨迹；`Trajectory` / `TrajectoryConfig` 已删除。眼点由初始值起步，经 `applyManualStep` 累积。
- **空格热键切换控制（§4.4）**：toggle 按钮**不带助记键**（字母助记键与 WASD/CE 操控键冲突，按 S 会误触发切换）；键盘切换改由 `PreTranslateMessage` 拦截空格实现，避免「按 S 切换控制」的坑。
- **唯一 Host 数据源（2026-08 拆进程）**：engine 不再承担 Host（`HostPosePublisher` 及其采样/防回声逻辑删除，见 [多通道同步模块设计.md](./多通道同步模块设计.md) §5），viewhost 成为项目内唯一 Host 端数据源；配套 IG 配置走椭球模式（`viewhost_ig_*.json` / `scene_ecef_ig_*.json`）。命令面发送（`outMsgWithIgCtrlTcp`）归属 Host 进程；**实体摆放命令 UI 已落地（2026-08，§4.5）**，其余命令 UI 留后期。
- **报文自检按钮（2026-08，§4.7）**：`testtcp` / `testudp` 随机发对应链路测试报文（TCP 34 种 + UDP 4 种），engine 侧全量订阅并 HUD 显示类名，用于验证各报文「发送→链路→解包→投递」全链路支持；纯调试工具，不改变协议语义。
- **上行报文自检（2026-08，§4.7）**：engine `PacketProbeHandler`（原废弃 `CommandTriggerHandler` 改造重命名）F9 随机 TCP 上行（16 类）/ F10 发 SOF（UDP 上行仅此一种）；viewhost 侧 `HostDriver::pollIncoming`（转发 `drainIncoming`）+ 16 类 TCP 订阅刷新「最近接收」；HUD 显示「send」。IG→Host UDP 无随机多样性，F10 固定发 SOF 验证链路。

## 9. 与实现关系

| 项 | 状态 |
| --- | --- |
| `thirdparty/sync/examples/viewhost/` 工程 + 对话框控制台 | 已实现 |
| `HostDriver`（HostSync 封装）+ `applyManualStep`（步进换算，纯 C++） | 已实现 |
| 复用 `loadHostConfig` / `HostSync` 全链路（无 sync 库改动） | 已实现 |
| **新接口适配（2026-08-24 矛盾 A；2026-08-25 IGCtrl 自动填充）** | `HostDriver::update` 用 `outMsgWithIgCtrlUdp+appendEye+flushUdp`（`outMsgWithIgCtrlUdp()` 自动前置 IGCtrl，帧号/自计时时间戳）；`_eye`/`applyManualStep` 用 `cigi_wire::EyePose`（`frame` 枚举）；MSVC 构建通过 |
| `engine/Tests/ViewHostMathTests.cpp`：步进换算 `[unit]` 测试 | 已添加 |
| **实体摆放命令（2026-08）** | `HostDriver::sendEntityPose`（`outMsgWithIgCtrlTcp` + `CigiEntityPositionCtrlV4` Detach+LLA → `flushTcp`）+ 对话框「实体摆放」区（id/lat/lon/alt/ypr 输入 + 按钮 + 状态显示，§4.5）；IG 侧 engine `updateEntityPose` 订阅消费；MSVC 构建通过 |
| **报文自检（2026-08，§4.7）** | `HostDriver::sendRandomTcpPacket` / `sendRandomUdpPacket`（随机报文工厂表）+ 对话框「报文自检」区（testtcp/testudp 按钮 + 状态显示）；engine 侧全量 subscribe 探测 + HUD「recv: <类名>」；engine 全量测试通过 + 双构建（clang / MSVC）通过 |
| **上行报文自检（2026-08，§4.7）** | `HostDriver::pollIncoming`（转发 `drainIncoming`）+ `HostDriver::subscribe<T>` 模板转发；`OnInitDialog` 订阅 16 类 IG→Host TCP 报文 + UI 定时器每帧 pollIncoming + 「最近接收」显示；engine `PacketProbeHandler`（F9 随机 TCP 16 类 / F10 发 SOF）+ HUD「send」行；双构建（clang / MSVC）通过 |
| 多通道同步模块设计.md / sync模块化设计.md 同步（§7） | 已同步 |
