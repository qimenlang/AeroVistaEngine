# Sanitizer 总结

sanitizer 是一族**运行时插桩检查器**，在编译期注入检查代码、链接进运行时库，程序运行时自动发现各类内存与未定义行为问题。同族常见的有 AddressSanitizer（ASan）、UndefinedBehaviorSanitizer（UBSan）、ThreadSanitizer（TSan）、MemorySanitizer（MSan）、LeakSanitizer（LSan）等。本文聚焦本项目可能用到的 ASan / UBSan / TSan。

---

## 1. 三类 sanitizer 的作用

| Sanitizer | 全称 | 作用 | 典型发现 |
|-----------|------|------|----------|
| **ASan** | AddressSanitizer | 检测内存错误 | 越界读写、use-after-free、use-after-scope、double-free、内存泄漏（部分） |
| **UBSan** | UndefinedBehaviorSanitizer | 检测未定义行为 | 有符号整数溢出、除零、非法移位、空指针解引用、错误类型转换等 |
| **TSan** | ThreadSanitizer | 检测数据竞争（data race） | 两个线程无同步访问同一地址、至少一个是写 |

---

## 2. 开启方式：编译选项

sanitizer 是**编译 + 链接**都要加同一 flag，插桩后的二进制会额外链接 `clang_rt.*` 运行时库。

| Sanitizer | clang / GCC flag | MSVC flag |
|-----------|------------------|-----------|
| ASan | `-fsanitize=address` | `/fsanitize=address` |
| UBSan | `-fsanitize=undefined` | 无 |
| TSan | `-fsanitize=thread` | 无 |

CMake 里通常通过 `CMAKE_CXX_FLAGS` 或独立 preset 注入，例如：

```cmake
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address")
```

也可以组合多个（如 `-fsanitize=address,undefined`），但 ASan 与 TSan 通常不能同时开（运行时冲突）。

---

## 3. 本项目（Windows clang）支持矩阵

本项目 `clang-Ninja` / `ci-clang` preset 用 LLVM clang，target 为 `x86_64-pc-windows-msvc`（MSVC ABI）。**支持与否由 target 平台决定，不由 clang++ / clang-cl 驱动入口决定**——两者是同一前端，仅 flag 语法不同（clang-cl 用 `/fsanitize=`，clang++ 用 `-fsanitize=`）。

| Sanitizer | Windows clang（MSVC target） | 说明 |
|-----------|------------------------------|------|
| **ASan** | ✅ 可用 | 成熟支持；需 `/Zi` 调试信息，不能与 `/RTC` 混用 |
| **UBSan** | ⚠️ 部分可用 | 仅 clang-cl 支持（MSVC `cl.exe` 不支持）；不如 Linux 完整，可能有 false positive，**需实测** |
| **TSan** | ❌ 不支持 | clang 官方支持平台仅 Linux / Android / macOS；Windows 上 `-fsanitize=thread` 直接编译报错 |

> 结论：**TSan 在 Windows 上无解**，本项目的数据竞争排查改用并发压力测试 + 原子操作保证（见 `engine/Tests/TcpSocketTests.cpp` 的 `[stress]` 用例）。

---

## 4. CI 开 vs 本地开

sanitizer 不区分 CI 还是本地，都能开。实践建议：

- **本地（按需临时开）**：用独立 build preset / 目录，不污染日常构建；插桩后慢 2–5 倍、内存占用上升，适合快速定位问题。
- **CI（专门 job）**：做独立的 sanitizer 构建 job（如「ASan build + 跑测试」），每次提交全量扫、失败结果归档。
- 本项目 Windows 环境：**能加 ASan / UBSan 的 job，加不了 TSan**（平台限制）。

---

## 5. 与普通测试 / 逻辑竞态的关系（重要边界）

- sanitizer 查的是 **C++ 内存模型层面的错误**（越界、UB、data race）。
- 它**不查业务逻辑竞态**。例如引用计数「多减一次」导致状态错误——如果计数本身已是 `std::atomic<int>`，TSan 认为没有 data race，但「多加/少减」是逻辑错误，仍需**测试用例 + 行为断言**守护。
- TSan 报的 data race 要求「本次运行真的触发到该冲突访问」，即把「概率出错」变成「发生即报」；但仍需代码路径被实际执行。

---

## 6. 本项目落地状态

- **待实测**：ASan / UBSan 在本地 clang 下能否真正编译链接运行（见 [doc/TODO.md](../TODO.md)）。
- **TSan**：Windows 上放弃，改用并发压力测试兜底（`TcpSocketTests.cpp` 的 `[stress]` 用例）。
- **相关代码**：`thirdparty/sync` 中 WSA 引用计数（`SocketCommon.cpp`）与 `_wsaAcquired` 令牌（`TcpSocket`/`UdpSocket`）已用 `std::atomic` 保证线程安全。
