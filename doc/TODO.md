# TODO

待办事项列表。

---

## Sanitizer 在 clang 编译器下的实测验证

- [ ] 用本地 `clang-Ninja`（`C:\Program Files\LLVM\bin\clang++.exe`，target `x86_64-pc-windows-msvc`）实测三类 sanitizer 的实际支持情况（抽空再做）。
- [ ] 具体做法：用最小样例文件，分别加 `-fsanitize=address` / `-fsanitize=undefined` / `-fsanitize=thread` 编译链接，记录哪个能过、哪个报错。
- [ ] 预期（依据 clang 官方文档与社区讨论，待实测确认）：
  - ASan（`-fsanitize=address`）：预期可用。
  - UBSan（`-fsanitize=undefined`）：预期能编译运行，但不如 Linux 完整，可能有 false positive，需实测。
  - TSan（`-fsanitize=thread`）：预期 Windows 上不支持，编译报错。
- [ ] 若 ASan / UBSan 实测可用，再评估是否新增独立 CMake preset（如 `clang-asan` / `clang-ubsan`）及是否接入 CI。
- [ ] 相关总结见 [doc/notes/Sanitizer.md](./notes/Sanitizer.md)。

---

## 收包入口对等化（IG 侧 runPendingCommands/update vs Host 侧 drainIncoming）

- [x] 已完成（2026-08）：`IgSync::drainIncoming(sendSof)` 统一 drain TCP+UDP → 解包（先 UDP 后 TCP），与 `HostSync::drainIncoming()` 对等；`IgSync::update()` 收敛为帧级维护（外推冻结 + RUNNING 状态，不收包）；`runPendingCommands` 删除；`SynchronSystem::preFrame` 调 `drainIncoming(true)+update()`，`update()` 仅眼点决策。
- [x] 设计文档（状态同步设计.md §8.1/§8.2/§12）已同步。

---

## IG 时钟同步逻辑入 processor（待评估）

- [ ] 现状：`IgSync::processIncomingUdp` 里 `IgCtrlCaptureProc`（基础设施 processor）只捕获 IGCtrl 帧号/时间戳；相位展开（`queueHostTimeStamp`/`applyPhaseUnwrap`）与 `receivedAtUs` 消费仍留在 IgSync 侧，时钟状态（`_hasTimeStamp`/`_extendedTimeTicks`/`_lastSimTimeUs` 等）归 IgSync 成员。
- [ ] 方向：把时钟同步逻辑（相位展开 + 补偿 + 外推 + 冻结）整体移入一个 IGCtrl 的 `CigiBaseEventProcessor`（如 `SimTimeCaptureProc`），processor 内产出 `simTimeUs`；`frameStatsIgCtrlLine` 已通过 `igSync().simTimeUs()` 输出 HUD。
- [ ] 约束：`receivedAtUs` 必须由 I/O 线程在 recv 时刻记录并注入 processor（不能改为「主线程处理时刻」——UDP 空队列等待最多 2ms）；`SyncClockTests` 直接调 `queueHostTimeStamp`/`simTimeUsAt`/`frozen` 等公开接口，迁移需保留或重设计这些接口。待定。

## 时钟同步逻辑的 `receivedAtUs` 是否可去（待评估）

- [ ] 背景：UDP payload 当前带 `receivedAtUs`（I/O 线程 recv 时刻），用于时钟同步 §4.0 的 `simTimeUs(now) = lastSimTimeUs + (nowUs - lastReceivedAtUs)`。
- [ ] 不能简单去掉：主线程 processor 解包时刻比 recv 时刻晚 0~2ms（UDP 空队列等待，两个 I/O 轮询周期）。
- [ ] 待定：是否可接受误差 / 是否需改为「I/O 线程解包」或其他注入方式。

## 本地笛卡尔「合成 parent」约定（已解决，2026-09）

- [x] **已解决**：同步层收敛为只支持 LLA（2026-09 落地）——眼点与命令实体恒 `Detach`+LLA，本地绝对 XYZ 的 `Attach + ParentID=1` 合成 parent 借壳已随 `EyeFrame::WORLD_LOCAL` / `EyePose.frame` / `HostEyeCoordFrame` 删除而移除（`CigiWire.cpp::appendEye` 只走 Detach 分支）。
- [x] 后续演进：`coordFrame` 枚举 → `injectEllipsoidIfMissing`（bool，仅单机椭球渲染）；参与同步（有 `igConfig`）的场景由引擎自动注入椭球。
- [x] 相关测试（本地线契约 / E2E / 回归）已删或改椭球场景；本地场景保留但仅单机渲染（无 `igConfig`）。
- 关联文档：[lla位姿传输设计.md](./design/多通道同步/lla位姿传输设计.md) §2/§5、[实体与运动控制设计.md](./design/多通道同步/实体与运动控制设计.md) §4.2。

## 实体加载失败占位模型（红立方体，扩展位，2026-09）

- [ ] 现状：实体加载失败首版**不上报不处理**（实体静默缺失，IG 侧日志暴露）——[实体管理设计.md](./design/引擎基础功能/实体管理设计.md) §7.1。
- [ ] 扩展：把加载失败的实体以**占位模型（如红立方体）**替换，避免画面缺失、提高可观测性（能直观看到「哪个实体加载失败、在哪个位置」）。登记于 [实体管理设计.md](./design/引擎基础功能/实体管理设计.md) §11。
- [ ] 注意：占位替换与「加载结果上报」（IG→Host 事件通知，实体管理 §11 扩展位）是两个可独立引入的增强；若二者都做，占位是 IG 本地呈现、上报是让 Host 感知，互不替代。

## 视锥体业务逻辑重构

 使用host到ig的View Definition/View Control 报文控制视锥体





为什么要用viewhost要做ig和平台的中继

1. 充当视景本地调试时的调试器、编辑器
2. 真实调试时，需要视景可以现场调试？（可以平台直接搭建viewhost,合并为一个进程）；真正交付后是否可以ig直连平台；
3. 

viewhost做中继的缺点

权威：[平台同步设计.md](./design/平台同步设计.md) §2 / §6 / §7 / §11.1；[viewhost设计.md](./design/viewhost设计.md)。

1. 增加一跳 RTT。正式运行因此不经 viewhost，IG 直连平台。
2. 打破 CIGI 的 Host–IG 成对关系：IG 眼里的 Host 是 viewhost，平台眼里的 IG 是虚 IG。viewhost 必须加胶水（切齐入队、`pollRelay` 原样转发、数据面 UDP 取出后 `packSof`），不能当两段独立 Session。
3. 权威数据
   - 平台与 viewhost 要用同一份配置初值（如 `entities.json`）。
   - 两边都能改同一类数据（天气等）时 last-write-wins，平台权威表与 IG 实况会分叉；中继不把附加写进 `HostDataManager`、也不回写平台（§7 待完善）。
4. 平台看见的世界被压扁
   - 只见一台 peer（虚 IG），看不见 N 台真实 IG。
   - 真实 IG 的纯 SOF 不转平台（viewhost RTT）；master UDP 若 SOF 后有业务包，剥真实 SOF，虚 IG 当场组 `SOF'` 发平台（[平台同步设计.md](./design/平台同步设计.md) §6.3 / 序 7 **已完成**）。平台回显 SOF 仍来自取出 IGCtrl 后的 `packSof`。
   - 回程 TCP 只转 master（`channelId==0`）；侧通道只 UDP SOF 保活、TCP 不入队；真实 IG 的 HELLO 不转平台。
   - 真实 IG 掉线后虚 IG 仍连平台，平台 `readyIgCount` 仍为 1；SofGated 等的是虚 IG SOF。
5. 中继模式下 viewhost 不能当本地 Host 用
   - 禁止 `HostDriver::update` 扇出眼点（否则两路数据面 IGCtrl）；键盘眼点在**转发开着**时无操作。关掉运行期转发且缓存有效后，把当前眼点写成该缓存，再 `update` 自己组 IGCtrl（[平台同步设计.md](./design/平台同步设计.md) §6.1 / 序 8）。
   - 报文自检只对真实 IG：不解析、不显示平台下行，也不把自检包发给平台。
   - 转发绑 UI 定时器，不是 I/O 读循环立刻转。运行期停转发开关已在 `HostDriver::setRelayForwarding`（序 8）。
6. 仍待落地、会放大上面缺口
   - 起齐门闩无超时、无强行开始。

仅 master 回业务报文是 IG 侧开发建议，**不是**中继/sync 待办（[平台同步设计.md](./design/平台同步设计.md) §6.2；序 5 不开发）。
