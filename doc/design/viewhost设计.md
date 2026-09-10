# viewhost 设计（MFC Host 宿主程序）

> **已按新接口同步（2026-08-24；2026-08-25 IGCtrl 自动填充）**：`HostSync::update`/`EyePose` 已删除，`HostDriver::update` 改用 `outMsgWithIgCtrlUdp() + cigi_wire::appendEye + flushUdp()`（[状态同步设计初版.md](./多通道同步/状态同步设计初版.md) §7.1）——`outMsgWithIgCtrlUdp()` 自动前置 IGCtrl（帧号/自计时时间戳/`TimeStampValid=true`）；眼点类型为 **`cigi_wire::EyePose`**（`frame` 枚举替代 `isLla` 布尔）。本文正文已全部对齐。

面向「用 MFC 对话框程序作为独立 Host 进程，经 `aerovistaSync` 的 `HostSync` 向多个携带 IG 的 Engine 扇出同一 Host 眼点，模拟多通道同步」的设计。

基础协议 / 行为见 [多通道同步模块设计.md](./多通道同步/多通道同步模块设计.md)；库结构与接入见 [sync模块化设计.md](./多通道同步/sync模块化设计.md) 与 [../../thirdparty/sync/README.md](../../thirdparty/sync/README.md)。

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
2. 复用 `aerovistaSync`（`HostSync` 传输 + `HostDataManager` 权威表，不重写网络 / 协议 / 线程模型），`HostDriver` 做宿主编排：读配置 → 启动传输 → 按帧扇出眼点 → UI 只调 Driver 意图 API（§4.0）。
3. 落地 [多通道同步模块设计.md](./多通道同步/多通道同步模块设计.md) §1.1 的「HostSync 独立进程」远期项（**已落地 2026-08**：Engine 不再承担 Host，`HostPosePublisher` 删除，HostSync 独立运行于本进程；与 viewhost 配套的 IG 配置为 `viewhost_ig_*.json` / `scene_ecef_ig_*.json`）。

### 1.2 非目标

- 不实现 IG 侧渲染 / 决策（那是 `Engine` + `SynchronSystem` 的职责）。
- 不改动 sync 库的协议、线程模型、载荷布局（CIGI V4 + TCP/UDP 双平面不变）。
- 不做边缘融合、精标定、精时钟（RTT / PTP）。
- 不做 **加载 / 切库 / 复位类** TCP 命令 UI（实体摆放/文本指令命令面已落地，见 §4.5；加载/切库/复位属后续，见 [多通道同步模块设计.md](./多通道同步/多通道同步模块设计.md) §9）。

### 1.3 与 sync 库的关系

`HostSync` / `HostDataManager` 均是**零 vsg** 的 sync 库类型（Winsock + 标准库 + CIGI），MFC 程序链接无阻碍。`HostDriver` 持有二者（§4.0）。数据面发送接口（新契约，[状态同步设计初版.md](./多通道同步/状态同步设计初版.md) §7.1）：

```cpp
// HostDriver::update —— 业务侧组装数据面帧节拍（IGCtrl 自动 + 眼点）后统一 flushUdp
auto& omsg = _host.outMsgWithIgCtrlUdp();        // 自动前置 IGCtrl（帧号=数据面、TimeStamp=自计时、TimeStampValid=true）
cigi_wire::appendEye(omsg, eye);         // 可选：追加 ownship 眼点
_host.flushUdp();                        // PackageMsg + 扇出
```

- `outMsgWithIgCtrlUdp()` 自动前置 IGCtrl 并填充帧号/时间戳（`HostSync` 自计时，2026-08-25）；帧号由 `outMsgWithIgCtrlUdp()` 内部数据面计数器自动分配。
- `cigi_wire::appendEye`（`CigiWire.h`）负责 ownship 眼点的 CCL 组装（WorldLocal→Attach+XYZ / LLA→Detach+LLA，LLA 越界丢弃内置）。原 `appendHostFrame`（IGCtrl+眼点整体组装）已删——IGCtrl 归属 sync。
- 数据面与命令面统一走 CCL 会话；`HostSync::update`/`EyePose` 已删除。

依赖传递（[sync模块化设计.md](./多通道同步/sync模块化设计.md) §3.0）：

- `aerovistaSync` 公开链接 `cigicl-static` + `ws2_32`（`CigiWire` 打包 IGCtrl 用），**不**链接 `vsg`（眼点数学在 engine `CameraDriver`，见 [sync模块化设计.md](./多通道同步/sync模块化设计.md) §3.0）。
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
    HostDriver.h/.cpp         # 持有 HostSync + HostDataManager：生命周期 + 帧驱动 + 意图 API
    ViewHostMath.h/.cpp       # 纯 C++：applyManualStep（可测）
  resources/
    ViewHost.rc / resource.h  # 对话框 / 字符串表
    viewhost.json             # hostConfig（复用 loadHostConfig）
```

**命名约定**：

- MFC 框架派生类遵循 MFC 惯例（`CViewHostApp`、`CViewHostDlg`，C 前缀 + PascalCase），属 MFC 类型族，与 `cpp-vsg-style` 规则不冲突（该规则约束 engine 业务代码）。
- 业务逻辑类（`HostDriver`）置于 `namespace aerovista::viewhost`，方法 `camelCase`、私有成员 `_camelCase`，符合项目风格。`HostDataManager` / `HostSync` 在 `aerovista::sync`，不进 viewhost 命名空间。

## 4. 功能与数据流设计

### 4.0 Host 分层（写死）

Host 进程内三类对象；**权威表不进 `HostSync`，MFC 不组包、不直接改表**：

| 类型 | 命名空间 | 职责 |
| --- | --- | --- |
| `HostSync` | `aerovista::sync` | 传输：握手、`flushTcp` / `flushUdp`、收包。不持任务状态 |
| `HostDataManager` | `aerovista::sync` | Host 侧**全部**权威表门面（首版仅实体族，内部分表；环境/视景等见 [多通道同步模块设计.md](./多通道同步/多通道同步模块设计.md) §9 P2）。JSON 只提供初值、`snapshot()` 给 UI、按当前行填 CIGI。**不持 socket、不 `flush`** |
| `HostDriver` | `aerovista::viewhost` | **持有**上二者。意图 API：先写表 → 从表组包 → `HostSync` 发送。后加入 IG ready 全量重放（无 UI）。每帧 ownship 眼点（**不进**权威表）。`pollIncoming` / 报文自检 |

```text
ViewHostDlg  只报意图 / 按 snapshot 刷新树
    → HostDriver（编排：何时改、何时发、TCP 还是 UDP）
         → HostDataManager 改表、按行填报文
         → HostSync.flushTcp / flushUdp
```

写死：

1. UI 事件只调 `HostDriver`，不 `#include` CCL、不直接碰 `HostSync` / `HostDataManager`。
2. 发出去的字段永远来自表（先 `setXxx` 再组包），不以编辑框原文组包。
3. 后加入 IG ready 重放走同一 Driver 路径，不经按钮。
4. ownship 眼点仍走 `HostDriver::update`（§4.1），不进 `HostDataManager`。
5. 报文自检（§4.7）仍经 Driver 直发随机包，**不写入**权威表。
6. Dlg **不**把控件双向绑到表字段。属性面板初值来自 `snapshot()`；编辑框是草稿；**Apply**（意图 API）才写表。一次性 TCP 仍是「填参 → Apply」（§4.8）。

库边界与否决「表并入 `HostSync`」见 [sync模块化设计.md](./多通道同步/sync模块化设计.md) §3.4。实体表字段与广播时机见 [实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §3 / §7。

### 4.1 启动流程

对齐 [`minimal_viewhost.cpp`](../../thirdparty/sync/examples/minimal_viewhost.cpp)：

```text
1. loadHostConfig(viewhost.json, host, &error)      // sync 库内解析，只含 hostConfig 块
2. HostSync::initialize(host)                        // bind UDP + TCP listen，起 accept/UDP 线程
3. HostSync::run()                                   // 置 RUNNING（一次，非每帧）
4. 定时器按目标 fps 调 HostDriver::update(&eye)  // 每帧 outMsgWithIgCtrlUdp<<眼点→flushUdp
5. HostSync::shutdown()                              // 退出时收尾
```

### 4.2 眼点表示与平移参考系（恒 LLA）

viewhost 作为 Host 端恒发 **LLA**（同步只 LLA，2026-09 收敛；`cigi_wire::EyePose` 无 frame 判别字段，恒 Detach+LLA）。字段语义见 [lla位姿传输设计.md](./多通道同步/lla位姿传输设计.md) §3.1 / §3.2 与 `CigiWire.h`：

| EyePose 字段 | LLA 语义 |
| --- | --- |
| `x` | 纬度 lat（度，`[-90, 90]`） |
| `y` | 经度 lon（度，`[-180, 180]`） |
| `z` | 海拔 alt（米，相对椭球面） |
| `yawDeg` / `pitchDeg` / `rollDeg` | 当地 **ENU** YPR（东-北-天；`yaw=0` 朝北，`+yaw` 左转朝西） |

> **术语澄清**：viewhost 作为 Host 端发的是 **LLA**，**不是** ECEF。ECEF（地心米制笛卡尔）是 IG 侧椭球场景的渲染工作坐标（[lla位姿传输设计.md](./多通道同步/lla位姿传输设计.md) §2）；`cigi_wire::EyePose` 无「直接发 ECEF」选项。LLA 即配合 engine 椭球场景（`scene_ecef_ig_*.json` 等参与同步的 IG 自动注入椭球）。

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
- 经度越界按 [lla位姿传输设计.md](./多通道同步/lla位姿传输设计.md) §5 normalize 到 `(-180, 180]`；**纬度 clamp 到 `[-89.9, 89.9]`**（避免 lat 越界触发 CCL bound check 拒包，同时避免 `cos(lat)→0` 使经度增量除零）；pitch 越界同样 clamp 到 `[-89.9, 89.9]`；**yaw 累加后 normalize 到 `(-180, 180]`**（避免方向键持续偏航导致 yaw 无限增长、`float` 精度劣化——`CigiWire.cpp` 的 `llaEyeInRange` 不校验 yaw，见 §4.5）。
- **极区数值说明**：clamp 后 `cos(lat)` 最小约 `cos(89.9°) ≈ 0.00175`，经度增量会放大约 570 倍，但**不会除零**；经度增量后仍 normalize，数值安全，仅「单位米对应的经度分辨率」随纬度升高而降低——属局部平面近似的固有精度损失，非错误。

### 4.3 帧节拍与线程模型

- **HostSync 内部已有线程**：`_acceptThread`（TCP accept）、`_udpThread`（UDP 收 SOF / 握手）、`_clientThreads`（每 client 一个）。viewhost **不额外造网络线程**。
- **扇出驱动**：`HostDriver::update`（内部 `outMsgWithIgCtrlUdp() << IGCtrl << 眼点 → flushUdp()`）是 UDP 非阻塞扇出（FreeRun，不等 SOF），不会长时间占用调用线程。**初版写死：用 MFC `SetTimer`（约 60fps）在 UI 线程驱动 `update()`**，对齐示例的「主循环 + sleep」模式，实现最简。
- 若未来需要更高节拍稳定性，再迁移到专用工作线程 + `PostMessage` 回传状态（本版不做）。

**本地时钟与帧增量（写死）**：

- `_simTimeMs` = UI 定时器本地 `std::chrono::steady_clock` 流逝毫秒（单调连续，不随 UI 卡顿回退），**仅用于按键步进 dt 归一化**；模拟时间戳不由它产生——由 `outMsgWithIgCtrlUdp()` 的 HostSync 自计时填充（[状态同步设计初版.md](./多通道同步/状态同步设计初版.md) §7.1）。
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

**`_eye` 初始化（写死，恒 LLA）**：

- `_eye` 构造后**立即初始化**为：初始演示眼点（lat/lon/alt，位于模型群附近），`yaw = 0`、`pitch = roll = 0`。同步只 LLA（2026-09 收敛），`EyePose` 无 frame 字段。
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

> **UI 已由 §4.8 取代**：手输 `Entity ID` 的摆放表单已删除。位姿改走实体树双击属性面板 Apply → `setEntityPose` + `sendEntity`（先写表再组包，一次 `flushTcp`）。

命令面语义仍是：`HostDriver::setEntityPose` 写权威表 last pose，`sendEntity` 按表组 `CigiEntityPositionCtrlV4`（**Detach+LLA，`EntityID≠0`**）→ `outMsgWithIgCtrlTcp()` → **一次** `flushTcp()`。未知 id 失败且不发送。

IG 侧消费：engine `initSync` 订阅 `addCallback<CigiEntityPositionCtrlV4>`（眼点 + 命令实体多播分流，§4.1），ownship（`EntityID==0`）翻译为 `ChannelEye` 入队 CameraDriver、命令实体按 Detach→`updateEntityPose(id, lla, ypr)`（`vsg::dvec3`）更新实体位姿（状态同步设计初版.md §12）。

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

IG 侧对照：engine `initSync` 对 IgSync 已注册的**全部 Host→IG 报文**逐一 `addCallback`，收到即记录类名到 HUD「recv: <类名>」行（F2 开关帧统计）。两端类名一致 = 该报文「发送→链路→解包→投递」全链路支持（`cigi梳理.md` 链路矩阵）。

- 测试报文覆盖：TCP 命令面 34 种（一次性/配置/请求/符号类）+ UDP 数据面 4 种（持续/每帧控制类）；IGCtrl 与 ownship 眼点由常态化发送覆盖，不在此列。

**上行自检（IG→Host，engine 触发）**：engine 侧 `PacketProbeHandler`（原废弃 `CommandTriggerHandler` 改造）挂接窗口事件：

- **F9**：随机发一条 TCP 上行报文（IG→Host 16 类响应/通知，经 `outMsgWithSofTcp` → `flushTcp`）；
- **F10**：显式发 `CigiSOFV4`（`outMsgWithSofUdp` → `flushUdp`；IG→Host UDP 仅 SOF 一种，cigi梳理.md 链路矩阵）。

viewhost 侧：`HostDriver::pollIncoming()`（转发 `HostSync::drainIncoming`）在 UI 定时器每帧调用，`OnInitDialog` 中对 16 类 TCP 上行报文逐一 `addCallback`（`HostDriver::addCallback<T>` 转发 `HostSync::addCallback`），收到即刷新「最近接收」静态文本；F10 的 SOF 用已有「SOF 接收」计数确认（每帧自动回 SOF 亦计入）。engine HUD 追加「send: <类名>」行显示 F9/F10 发送结果。

- 上行覆盖：TCP 16 类（IGMsg/EventNotification/AnimationStop/HatHotResp/X/LosResp/X/SensorResp/X/PositionResp/WeatherCondResp/AerosolResp/Maritime/TerrestrialSurfaceResp/CollDetSeg/VolResp）+ UDP 1 类（SOF）。
- 纯调试工具，不改变协议语义；随机选择（`std::mt19937`），多次按键遍历覆盖。

### 4.8 实体控制 UI（平级树 + 双击属性面板）

**目标**：把 §4.5 的「手输 `Entity ID` + 单一摆放表单」升级为完整的实体控制界面。实体显隐 / 位姿的协议语义见 [实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md)（Host 权威、`EntityCtrlV4` 只切显隐——IG 侧实例已启动全量预建、`EntityPositionCtrlV4` 摆放）。动画为首版之外。

**数据来源（写死：Host 读独立实体目录文件，非网络获取）**：实体目录为**独立 `entities.json`**（[实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §5 方案 B——**文件暂放** `engine/resources/config/entities.json`，Host 与各 IG 读到一致内容、人工确保）。树与属性面板初值来自 `HostDataManager` 对该文件的**子集**解析（`id` / `name` / `model` / `initialEntityState` / `pose.ellipsoid`），不通过网络从 IG 拉取。IG 完整 schema（含 `pose` 双轨）仍走 engine `loadEntitiesFile`。**路径（落地）**：不进 `viewhost.json` / `hostConfig`（避免改 sync 配置 schema）。与 `viewhost.json` 相同，从**工作目录**读 `entities.json`；构建时 POST_BUILD 把 `engine/resources/config/entities.json` 拷到 exe 旁。加载失败弹出提示、实体树为空，Host 眼点仍可用。

**数据流（写死，分层见 §4.0）**：`HostDataManager` 从 `entities.json` 子集初始化实体权威表（含 last pose 初值，运行期不回写文件）；Dlg 按 `HostDriver` 提供的 snapshot 刷新树；双击打开属性面板时初值来自表，编辑为草稿，**Apply** 才调 Driver 意图 API（先写表再组包 `flushTcp`）。一次性 TCP「填参 → Apply」，持续 UDP「toggle 持续模式」；ownship 眼点走每帧循环、不进表。其它报文族以后仍进同一 `HostDataManager`、同一 Driver 编排（[多通道同步模块设计.md](./多通道同步/多通道同步模块设计.md) §9 P2）。

**初始广播（写死：自动全量 + 按 `initialEntityState` / `pose.ellipsoid` 初始化权威表）**：`HostDriver` 在检测到 **新增** ready IG 后调用 `broadcastEntityAuthority()`——从表当前值组 `EntityCtrl` 与 `EntityPositionCtrl` 全量 `flushTcp`（表由 `initialEntityState` + `pose.ellipsoid` 初始化，见 [实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §7）。不经按钮。IG 侧实例启动已全量预建，`Standby` 项只是初始隐藏。后续控制走树 + 双击属性面板，**不提供销毁按钮**。

**交互（写死：平级树 + 双击属性面板）**：

- **树**：根节点 `entities`（只作折叠容器，可展开 / 收缩，**不是**实体，双击不弹面板、只展开/折叠）。子节点**全部平级**（首版无父子挂载，[实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §12）。每项显示 `name`（缺省 `basename(model)`）。数据来自 `entitySnapshot()`，不是再读 JSON。id 不手输（对比 §4.5 现状手输 id）。
- **双击弹出属性面板**：在某个 entity 节点上左键双击，弹出该实体的属性面板（非常驻右侧 inspector）。不经右键、不经上下文菜单。标题栏关闭 / ESC 不提交。
- **面板字段**：`id` / `name` **只读**；`state`（Standby / Active）、`alpha`、pose（lat/lon/alt + yaw/pitch/roll）可编辑。打开时初值来自表。编辑框是草稿。**无销毁控件**。无「关闭」按钮（关窗走标题栏）。
- **Apply（写死）**：先把脏**报文族**对应的 UI 当前值写入表，再从表组包，**一次** `flushTcp()`（一个 TCP 数据报可含多张业务包）。Dlg 不直接组 CCL。按报文族提交，不按字段拆同一张包；否决 `applyEntity` 把 Ctrl+Position 无条件全发。
  - `EntityCtrl` 族脏（`state` 或 `alpha` 任一改了）→ 用面板上 **state+alpha 的当前值** 调 `setEntityCtrl`（两字段一次写表）
  - `EntityPositionCtrl` 族脏（pose 任一分量改了）→ 用面板上 **完整 pose** 调 `setEntityPose`
  - 然后 `sendEntity`：只组脏了的报文族；两族都脏则两张包一次 flush。未改的族不发。
  - 发出去的字节永远来自表，不从编辑框原文组包。
- **重置（写死）**：从权威表重填当前草稿并刷新脏比较基线。不写表、不组包、不关面板。否决「清空」（会理解成把字段抹空）。
- **首版不进此面板**：动画（未进权威表）；部件 / 组件 / 速度 / 加速度 / 钳制（扩展点，见下）。

| 表内字段 / 扩展 | 报文 | 首版面板 |
| --- | --- | --- |
| 显隐 `state` + `alpha` | `CigiEntityCtrlV4` | ✅ Apply → `setEntityCtrl` + `sendEntity` |
| 位姿 pose | `CigiEntityPositionCtrlV4` | ✅ Apply → `setEntityPose` + `sendEntity` |
| 动画 | `CigiAnimationCtrlV4` | ❌ 未进表，不进此面板 |
| 部件 / 组件 / 速度 / 加速度 / 钳制 | `ArtPartCtrl` 等 | ❌ 扩展点 |

- **交互语义分两类（写死）**：一次性 TCP（显隐 / 摆放 / 日后动画与部件）用属性面板「填参 → Apply」；持续 UDP（实时位姿 / 速度 / 加速度 / 钳制）用「选中实体 → toggle 持续模式」，**不适合 Apply**（是控制回路，非表单）。首版只做一次性 state / alpha / pose；持续类留扩展点。

**实体加载结果（写死：首版不上报不处理）**：见 [实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §7.1——实体加载失败首版**不上报、Host 不感知**（IG 侧日志暴露，实体静默缺失）。viewhost 实体树因此**不显示**「加载中 / 成功 / 失败」或「业务 ready」，只反映连接层 ready（现状 `readyIgCount`）与树上用户操作后的状态。

**`HostDriver` 意图 API（Dlg 只调这些）**：

```text
bool loadEntityCatalog(path);                    // → HostDataManager 建表（entities.json 子集）
std::vector<EntityAuthorityRow> entitySnapshot() const; // 树绑定；Dlg 不 include CCL
bool setEntityCtrl(id, state, alpha, error);     // 只写表：EntityCtrl 两字段一次写齐
bool setEntityPose(id, pose, error);             // 只写表：last pose
bool sendEntity(id, EntitySend{ctrl, position}, error); // 从表组脏报文族，一次 flushTcp
void sendAnimationCtrl(...);                     // 先写表（若跟踪动画）→ 组包；首版不做
void broadcastEntityAuthority();                 // ready 路径：按表当前值全量 EntityCtrl + EntityPositionCtrl（§7）；仍待
```

> 组包实现在 `HostDataManager`，Driver 只编排写表与发送。脏判定按**报文族**（一张 `EntityCtrl` 的字段一起提交），不按字段拆同一张包。否决综合 `applyEntity`（把 Ctrl+Position 无条件一次发全）。否决 Apply 内对同一张 `EntityCtrl` 连 flush 两次。无加载结果上报订阅（首版实体加载失败不上报，§7.1）。

**测试（写死）**：MFC UI **不测**（§6）。`HostDataManager`（建表 / 运行期更新 / 组包字段）在 `engine/Tests` 以 `[unit]` 覆盖（链 `aerovistaSync`，不启 socket、不编 MFC）。建表码 `ENT-04-table-*`；运行期更新 / 组包码 `ENT-04-update-*` / `ENT-04-pack-*`（[实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §11）。生命周期 / 显隐 / 位姿的可观察结果仍以 `[acceptance]` 覆盖（同 §11 的 `ENT-02-*` / `ENT-03-*` / `ENT-04-late-join` 等）。Host 侧 `entities.json` **子集**解析归 `HostDataManager`（`SyncJson`）；IG 完整 schema（含 `pose` 双轨）仍走 engine `loadEntitiesFile`（§5 文件不迁 sync 库）。

---

## 5. 配置设计

- 复用 sync 库 `loadHostConfig`，配置形态与 `viewhost.json` 一致（顶层仅 `hostConfig` 块，未知键拒绝）：

```jsonc
{ "hostConfig": { "udpPortSend": 8001, "udpPortRecv": 8000, "tcpPort": 8100 } }
```

- **坐标系字段**：viewhost（Host 进程）**不配置任何坐标系字段**——同步只 LLA（2026-09 收敛），Host 恒发 LLA，与 IG 侧坐标系**靠人工部署保持一致**（IG 侧由「场景有无 `EllipsoidModel`」决定，见 [lla位姿传输设计.md](./多通道同步/lla位姿传输设计.md) §2.5）。`viewhost.json` 仅含 `hostConfig` 端口块。

## 6. 测试策略

**分层原则**：

- **不测**：MFC UI（`CDialog` 消息循环 / `GetAsyncKeyState` 轮询）。`HostSync` 的握手 / 扇出 / LLA 组包已由 `engine/Tests` 的 `HostIGTests`（`[viewhost]` / `[standalone]`）覆盖。
- **测（`[unit]`）**：（1）键盘步进→LLA 换算（§4.2，`ViewHostMath`，与示例共源）。（2）`HostDataManager` 权威表：建表 `ENT-04-table-*`；运行期更新 `ENT-04-update-*`；按行组包 `ENT-04-pack-*`（[实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §11）。`HostDriver` 编排不单独测 MFC；表逻辑在 Manager 上测，发送仍走既有 Host↔IG 用例。

**约束（写死）**：步进换算必须保持**纯 C++**——不依赖 MFC / vsg，只依赖 `cigi_wire::EyePose` 这一 POD 类型（include `CigiWire.h` 即可，不产生链接依赖），否则无法挂入 `engine/Tests`。

**挂载**：

- 新增 `engine/Tests/ViewHostMathTests.cpp`，加入 `engine/Tests/CMakeLists.txt` 的 `SOURCES`。
- 换算实现（`ViewHostMath.cpp`）也加入该测试 target 的编译单元——**测试与示例共用同一份源码**，不复制逻辑。
- 测试 target 已链接 `vsgEngineLib`（间接含 `aerovistaSync`，提供 `cigi_wire::EyePose`），无需新增链接。

## 7. 与现有文档关系

本文档落地后，已按 `doc-sync` 完成跨文档同步：

1. [多通道同步模块设计.md](./多通道同步/多通道同步模块设计.md) §1.1：补「独立 Host 进程示例已落地」（指向本文档）。
2. [sync模块化设计.md](./多通道同步/sync模块化设计.md) §1.3 非目标：改为「不做 Host 独立进程的协议 / 上行改造（viewhost 示例已落地）」。
3. [多通道同步模块设计.md](./多通道同步/多通道同步模块设计.md) §10 状态表 / §9 P2 / §0 表格 / §5 权威源：标注「Host 本地输入已有 viewhost 示例」，「指定输入 IG 上报」仍属后期；§9 P2 其它报文族进同一 `HostDataManager`。
4. [sync模块化设计.md](./多通道同步/sync模块化设计.md) §3.4：`HostDataManager` 归 sync 库、不并入 `HostSync`；[实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §3.2：实体表内容与广播仍由其规定。

## 8. 否决与决策记录

- **多通道在 IG 侧（澄清）**：viewhost 不感知通道数与 `offsetDeg`，只持一个 `HostSync` 扇出同一眼点。
- **扇出驱动走 UI 定时器（初版写死）**：`HostDriver::update`（`outMsgWithIgCtrlUdp+appendEye+flushUdp`）非阻塞，UI 定时器驱动最简；高节拍稳定性需求留待工作线程方案。
- **触发方式选 toggle 按钮（否决左键开始 / 右键结束）**：右键在 Windows 惯例为上下文菜单语义，且按钮控件对右键不产生点击通知，需在对话框层额外处理 `WM_RBUTTON*`；左/右键还缺状态可见性。改为单一 toggle 按钮（文字+颜色反映状态）承载「开始控制 ↔ 停止控制」。
- **键盘读取用 `GetAsyncKeyState` 轮询（否决 `OnKeyDown`）**：对话框焦点在子控件上时 `WM_KEYDOWN` 不路由到对话框，且按下有重复延迟；物理键状态轮询与焦点无关、连续输入跟手，但需 toggle 开关避免与文字输入冲突（§4.4）。
- **恒 LLA 眼点（非 ECEF，2026-09 收敛）**：viewhost 发 LLA（lat/lon/alt + 当地 ENU YPR），配合 engine 椭球场景；ECEF 仅是 IG 侧渲染坐标，`cigi_wire::EyePose` 无发 ECEF 选项（§4.2）。
- **程序放 `thirdparty/sync/examples/`**：viewhost 是 sync 库的 Host 接入示例，与 `minimal_viewhost.cpp` 并列，不进 `tools/`（§3）。
- **平移参考系 = 机头局部（否决地理固定 N/S/E/W）**：WASD 沿当前 `yaw` 的机头局部水平面移动，配合方向键 yaw/pitch 的姿态控制更符合「驾驶」直觉；上下用绝对垂直 alt（§4.2）。
- **测试范围分层（写死）**：MFC UI 不测；`HostDataManager` 与步进换算挂 `engine/Tests` `[unit]`（§6）。`HostDriver` 不再视为无逻辑薄封装——意图编排通过测 Manager + 既有 Host↔IG 用例覆盖，不测对话框。
- **Host 分层（2026-09，§4.0）**：`HostDriver` 持有 `HostSync` + `HostDataManager`；UI 只调 Driver。否决权威表并入 `HostSync`，否决 MFC 组包。
- **圆周轨迹已移除（决策）**：viewhost 只保留键盘手动操控眼点，不做自动圆周轨迹；`Trajectory` / `TrajectoryConfig` 已删除。眼点由初始值起步，经 `applyManualStep` 累积。
- **空格热键切换控制（§4.4）**：toggle 按钮**不带助记键**（字母助记键与 WASD/CE 操控键冲突，按 S 会误触发切换）；键盘切换改由 `PreTranslateMessage` 拦截空格实现，避免「按 S 切换控制」的坑。
- **唯一 Host 数据源（2026-08 拆进程）**：engine 不再承担 Host（`HostPosePublisher` 及其采样/防回声逻辑删除，见 [多通道同步模块设计.md](./多通道同步/多通道同步模块设计.md) §5），viewhost 成为项目内唯一 Host 端数据源；配套 IG 配置走椭球模式（`viewhost_ig_*.json` / `scene_ecef_ig_*.json`）。命令面发送（`outMsgWithIgCtrlTcp`）归属 Host 进程；**实体摆放命令 UI 已落地（2026-08，§4.5）**，其余命令 UI 留后期。
- **报文自检按钮（2026-08，§4.7）**：`testtcp` / `testudp` 随机发对应链路测试报文（TCP 34 种 + UDP 4 种），engine 侧全量订阅并 HUD 显示类名，用于验证各报文「发送→链路→解包→投递」全链路支持；纯调试工具，不改变协议语义。
- **上行报文自检（2026-08，§4.7）**：engine `PacketProbeHandler`（原废弃 `CommandTriggerHandler` 改造重命名）F9 随机 TCP 上行（16 类）/ F10 发 SOF（UDP 上行仅此一种）；viewhost 侧 `HostDriver::pollIncoming`（转发 `drainIncoming`）+ 16 类 TCP 订阅刷新「最近接收」；HUD 显示「send」。IG→Host UDP 无随机多样性，F10 固定发 SOF 验证链路。
- **实体控制 UI 演进（2026-09，§4.8）**：从「手输 Entity ID + 单一摆放表单」（§4.5）升级为**平级树 + 双击属性面板 + Apply / 重置**。Host 读**独立 `entities.json`** 填树（不网络拉取）。否决左侧点选 + 右侧常驻意图分组；否决综合 `applyEntity`；否决右键出面板；否决把重填草稿叫「清空」；否决 Apply 对同一张 `EntityCtrl` 按字段拆开发送。一次性 TCP「填参 → Apply」、持续 UDP「toggle 持续模式」分离。实体加载失败**首版不上报不处理**（无业务层 ready、无重试，见 [实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §7.1）。
- **初始状态配置 + Standby↔Active 双向切换（2026-09，§4.8）**：实体目录新增 `initialEntityState`（默认 `Active`，可设 `Standby`）；Host 权威表按此与 `pose.ellipsoid` 初始化，广播 `EntityCtrl` 与 `EntityPositionCtrl`；属性面板在 `Standby↔Active` 间改 `state`（Apply → `setEntityCtrl` + `sendEntity`）——IG 侧实例已启动全量预建挂 Switch，切换只是 **Switch 显隐**（ON/OFF，零加载零编译、不释放模型）。**不做** `load` 文本指令——调试临时加载需求由「配置 `Standby` + 手动激活」承载，模型路径须预先写进实体目录（见 [实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §7）。
- **销毁 UI 首版禁用（2026-09，§4.8）**：实体平时在 `Standby↔Active` 间切换，不提供「销毁」按钮——`Destroyed` 是释放资源的破坏性操作（值 2，`Remove`=同值历史别名），UI 误触代价高。**协议层 `Destroyed` 首版降级为 `Standby`**（Switch OFF、保留实例与资源，`_entityMap` 不 erase），完整销毁/重建为后续项（见 [实体与运动控制设计.md](./多通道同步/实体与运动控制设计.md) §7/§11/#19）。

## 9. 与实现关系

| 项 | 状态 |
| --- | --- |
| `thirdparty/sync/examples/viewhost/` 工程 + 对话框控制台 | 已实现 |
| `HostDriver`（持有 `HostSync` + `HostDataManager`）+ `applyManualStep`（步进换算，纯 C++） | 已实现：写表与 `sendEntity` 分离，Apply 一次 flushTcp；`broadcastEntityAuthority` / peer 去重仍待 |
| 复用 `loadHostConfig` / `HostSync` 全链路（无 sync 库改动） | 已实现 |
| **新接口适配（2026-08-24 矛盾 A；2026-08-25 IGCtrl 自动填充）** | `HostDriver::update` 用 `outMsgWithIgCtrlUdp+appendEye+flushUdp`（`outMsgWithIgCtrlUdp()` 自动前置 IGCtrl，帧号/自计时时间戳）；`_eye`/`applyManualStep` 用 `cigi_wire::EyePose`（`frame` 枚举）；MSVC 构建通过 |
| `engine/Tests/ViewHostMathTests.cpp`：步进换算 `[unit]` 测试 | 已添加 |
| **实体摆放命令（2026-08；2026-09 改经权威表）** | `HostDriver::setEntityPose` 写 last pose，`sendEntity` 从表组 `EntityPositionCtrl`，与其它脏报文族同一次 `flushTcp`；手输摆放表单已由 §4.8 属性面板取代 |
| **报文自检（2026-08，§4.7）** | `HostDriver::sendRandomTcpPacket` / `sendRandomUdpPacket`（随机报文工厂表）+ 对话框「报文自检」区（testtcp/testudp 按钮 + 状态显示）；engine 侧全量 addCallback 探测 + HUD「recv: <类名>」；engine 全量测试通过 + 双构建（clang / MSVC）通过 |
| **上行报文自检（2026-08，§4.7）** | `HostDriver::pollIncoming`（转发 `drainIncoming`）+ `HostDriver::addCallback<T>` 模板转发；`OnInitDialog` 订阅 16 类 IG→Host TCP 报文 + UI 定时器每帧 pollIncoming + 「最近接收」显示；engine `PacketProbeHandler`（F9 随机 TCP 16 类 / F10 发 SOF）+ HUD「send」行；双构建（clang / MSVC）通过 |
| 多通道同步模块设计.md / sync模块化设计.md 同步（§7） | 已同步 |
| **实体控制 UI 演进（2026-09，§4.8 / §4.0）** | 已实现：Driver 持有 Manager；平级树 + 双击属性面板；Apply 按报文族 `setEntityCtrl` / `setEntityPose` 后一次 `sendEntity`（一次 flushTcp）。重置从表重填草稿。`entities.json` 与 `viewhost.json` 同目录。`broadcastEntityAuthority` 仍待（§15 #3） |
