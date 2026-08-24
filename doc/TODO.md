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
