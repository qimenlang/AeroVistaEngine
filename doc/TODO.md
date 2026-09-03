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
- [x] 设计文档（状态同步设计初版.md §8.1/§8.2/§12）已同步。

---

## IG 时钟同步逻辑入 processor（待评估）

- [ ] 现状：`IgSync::processIncomingUdp` 里 `IgCtrlCaptureProc`（基础设施 processor）只捕获 IGCtrl 帧号/时间戳；相位展开（`queueHostTimeStamp`/`applyPhaseUnwrap`）与 `receivedAtUs` 消费仍留在 IgSync 侧，时钟状态（`_hasTimeStamp`/`_extendedTimeTicks`/`_lastSimTimeUs` 等）归 IgSync 成员。
- [ ] 方向：把时钟同步逻辑（相位展开 + 补偿 + 外推 + 冻结）整体移入一个 IGCtrl 的 `CigiBaseEventProcessor`（如 `SimTimeCaptureProc`），processor 内产出 `simTimeUs`；`frameStatsIgCtrlLine` 已通过 `igSync().simTimeUs()` 输出 HUD。
- [ ] 约束：`receivedAtUs` 必须由 I/O 线程在 recv 时刻记录并注入 processor（不能改为「主线程处理时刻」——UDP 空队列等待最多 5ms，会引入 ~30% 帧周期误差）；`SyncClockTests` 直接调 `queueHostTimeStamp`/`simTimeUsAt`/`frozen` 等公开接口，迁移需保留或重设计这些接口。待定。

## 时钟同步逻辑的 `receivedAtUs` 是否可去（待评估）

- [ ] 背景：UDP payload 当前带 `receivedAtUs`（I/O 线程 recv 时刻），用于时钟同步 §4.0 的 `simTimeUs(now) = lastSimTimeUs + (nowUs - lastReceivedAtUs)`。
- [ ] 不能简单去掉：主线程 processor 解包时刻比 recv 时刻晚 0~5ms（UDP 空队列等待），60fps 下 ~30% 帧周期误差。
- [ ] 待定：是否可接受误差 / 是否需改为「I/O 线程解包」或其他注入方式。

## 本地笛卡尔「合成 parent」约定（已解决，2026-09）

- [x] **已解决**：同步层收敛为只支持 LLA（2026-09 落地）——眼点与命令实体恒 `Detach`+LLA，本地绝对 XYZ 的 `Attach + ParentID=1` 合成 parent 借壳已随 `EyeFrame::WORLD_LOCAL` / `EyePose.frame` / `HostEyeCoordFrame` 删除而移除（`CigiWire.cpp::appendEye` 只走 Detach 分支）。
- [x] 后续演进：`coordFrame` 枚举 → `injectEllipsoidIfMissing`（bool，仅单机椭球渲染）；参与同步（有 `igConfig`）的场景由引擎自动注入椭球。
- [x] 相关测试（本地线契约 / E2E / 回归）已删或改椭球场景；本地场景保留但仅单机渲染（无 `igConfig`）。
- 关联文档：[lla位姿传输设计.md](./design/多通道同步/lla位姿传输设计.md) §2/§5、[实体与运动控制设计.md](./design/多通道同步/实体与运动控制设计.md) §4.2。
