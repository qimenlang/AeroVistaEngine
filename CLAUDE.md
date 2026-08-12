# AeroVistaEngine — Claude project rules

Rules merged from `.cursor/rules/*.mdc`. Scope is stated per section.

---

## 1. Engine C++ style (VSG)

**Scope:** `engine/**/*.{h,hpp,cpp}`
Authority: `CONTRIBUTING.md`. Match `vsg::` style; **do not** use Google C++ naming (PascalCase methods, `m_` / trailing `_` members).

### Naming (required)

| Kind                             | Style                | Example                       |
| -------------------------------- | -------------------- | ----------------------------- |
| Namespace                        | lowercase            | `aerovista`                   |
| Types (class/struct/enum)        | PascalCase           | `SynchronSystem`              |
| Methods / free functions         | camelCase            | `initialize`, `renderOneTick` |
| Public data                      | camelCase, no prefix | `extent`                      |
| Private / protected              | `_camelCase`         | `_syncSystem`                 |
| Locals / params                  | camelCase            | `modelPath`                   |
| Ctor params shadowing members    | `in_` + camelCase    | `in_matrix`                   |
| Container aliases                | PascalCase plural    | `Children`                    |
| Enum **values** / feature macros | SCREAMING_SNAKE      | `ONE_TIME`, `AEROVISTA_…`     |
| `static constexpr` values        | camelCase            | `nearFarRatio`                |
| Main-class files                 | match class name     | `SynchronSystem.h/.cpp`       |

### Hard bans (new engine code)

```cpp
// ❌
void Initialize();
bool CaptureToFile(...);
int m_count;
int count_;
void render_one_tick();

// ✅
void initialize();
bool captureToFile(...);
int _count;          // private
void renderOneTick();
```

- Prefer readable names (`address`) over cryptic abbreviations (`addr`, `OmsgPtr`) on engine APIs.
- Keep third-party type spellings (`CigiIGCtrlV4`, `VkExtent2D`).
- If a method is named `connect` / `shutdown`, call Winsock with `::connect` / `::shutdown`.

### Types & VSG

- Prefer top-level `namespace aerovista` (not nested under `vsg`); modules by **directory**, not nested namespaces.
- Ref-counted scene-graph types: `vsg::Inherit<...>` + `Type::create(...)`; register with `VSG_type_name` when needed.
- Behavior / ref-counted → `class`; small POD / options → `struct`.
- Prefer `vsg::ref_ptr` / `vsg::vec3` / `vsg::dmat4`; do not invent parallel math names.

### Files & format

- New headers: `#pragma once`; VSG includes as `#include <vsg/...>`.
- Do not restyle or rename `thirdparty/`.
- Do not spread legacy styles (e.g. old `Network` snake_case / bare members); wrap with a thin VSG-style adapter; rename only on substantive edits of that file.
- Let `clang-format` own layout (4 spaces, Allman braces, `T* p`).

### Before finishing an edit

1. No new PascalCase methods; no `m_` / trailing `_` members.
2. Enum values are SCREAMING_SNAKE.
3. Public API names are clear; files match primary type names where applicable.
4. Unrelated mass renames are out of scope unless asked.
5. Complexity (cognitive ≤20, CCN ≤15): see §2; run tidy/lizard on touched `engine/source` files.

---

## 2. Engine complexity gates

**Scope:** `engine/source/**/*.{cpp,h,hpp}`
Authority: `doc/notes/代码质量指标.md` §6. Naming/VSG style stays in §1.

### Thresholds (hard)

| Metric               | Tool                                                   | Max    | Scope                                     |
| -------------------- | ------------------------------------------------------ | ------ | ----------------------------------------- |
| Cognitive complexity | clang-tidy `readability-function-cognitive-complexity` | **20** | `engine/**` TUs (Tests **exempt**)        |
| Cyclomatic CCN       | lizard (`scripts/lizard_engine.py`, `-C 15`)           | **15** | `engine/source/**/*.cpp` only             |

Pre-commit / CI treat over-limit as **failure**. Do not "fix" by silencing the gate.

### Hard bans

- Do **not** add `NOLINT` / `NOLINTNEXTLINE` for these two checks to ship over-limit code.
- Do **not** leave a new/edited function over threshold "for a follow-up".
- Prefer extract **private methods** or **file-local helpers** (anonymous namespace) over deeper nesting.
- `Network.cpp` / `Network.h` are script-excluded legacy; do not copy that exemption pattern elsewhere.

### When editing `engine/source`

1. Keep each function under both thresholds while writing (early extract if branches pile up).
2. Before finishing the change, run on touched files:

```text
python scripts/clang_tidy_engine.py engine/source/path/Foo.cpp
python scripts/lizard_engine.py engine/source/path/Foo.cpp
```

3. Need `compile_commands.json` for tidy (e.g. `cmake --preset clang-Ninja` or `ci-debug`). Optional: `AEROVISTA_TIDY_BUILD_DIR`.
4. Full source scan when unsure: `python scripts/lizard_engine.py --all-source`.

### Fix pattern (preferred)

```cpp
// ❌ one large init/update with many if/else branches
// ✅ orchestrator + helpers each under the limits
bool Engine::initGraphics(const vsg::Path& modelPath);
bool Engine::createVulkanDevice(int& queueFamily);
```

---

## 3. Engine tests (ATDD / Catch2)

**Scope:** `engine/Tests/**/*.{cpp,h,hpp}` (opt-in — apply when touching tests)
Authority: `doc/测试用例书写规范.md`. Form ≠ test level. Prefer observable contracts over implementation details.

### Role → form → tags (required)

| Role        | Ask                     | Form                        | Tags                    |
| ----------- | ----------------------- | --------------------------- | ----------------------- |
| Acceptance  | Done / receivable?      | `SCENARIO`                  | `[acceptance][bdd]…`    |
| Integration | Modules/protocol wired? | `SCENARIO` or `TEST_CASE`   | `[integration]…`        |
| Unit        | Local logic correct?    | `TEST_CASE` default         | `[unit]…`               |

- Do **not** treat `SCENARIO` / `[bdd]` alone as acceptance.
- API rulers / CLI parsers → `[unit]`, not `[acceptance]`.

### Scenario narrative (required)

- **One Scenario, one outcome.** Split fail / succeed / disconnect lifecycles.
- **Given** = state; **When** = action/event; **Then** = machine-checkable result.
- Titles/When: behavior language. **Never** put test techniques in titles (`queue-injected`, mock names).
- Inject/mock/stub OK in **comments/code**; keep separate from true-packet E2E scenarios.
- Config: assert **rules** (e.g. channelId 0 starts Host; runtime addr == `engine.config.*`; default ≡ loaded `main.json`). Do **not** hardcode full JSON field tables (ports/sizes) unless `[golden]` / characterization and reference update is explicit.

### Hard bans

```text
❌ One SCENARIO: connect fails THEN succeeds THEN disconnects
❌ [bdd] on setCameraPose / resolveConfigPath with no [unit]
❌ Title: "queue-injected Host eye…"
❌ REQUIRE every main.json literal (8000, 640, …) in acceptance tests

✅ Split connect-fail / connect-ok / host-offline disconnect
✅ [unit][camera] TEST_CASE for LookAt pose helper
✅ Title: "linked IG applies Host eye…"; queue in comment
✅ Compare to loadEngineChannelConfig / engine.config
```

### AI + tests

- Human writes/reviews acceptance and critical Then assertions.
- **Do not** weaken or rewrite Then meanings to make tests green.
- Prefer few stable acceptance contracts + integration for protocol + many fast unit tests.

### Before finishing an edit

1. Tags match real role (`acceptance` / `integration` / `unit`).
2. One receivable outcome per Scenario.
3. No technique words in Scenario titles; Then is deterministic.
4. No new hardcoded config snapshot tables without explicit golden/characterization intent.

---

## 4. 需求梳理与分解 (Requirement clarification & decomposition)

**Scope:** 所有以 "帮我实现 / 加一个功能 / 改一下 X" 等需求描述开始的对话。

### 何时触发

收到需求时，先判断是否满足以下 **任一** 条件：

1. **需求不清晰**——存在歧义、缺失、或多种合理解读。
2. **需求较大**——涉及多个文件 / 跨模块 / 多步实现 / 影响面难以一次说清。

只要满足其一，**先不要开始写代码**。

### 需求不清晰 → 提问澄清

- 用 `AskUserQuestion` 提结构化选择题（选项可枚举时）；开放性问题用普通文本提问。
- **每轮提问聚焦**：一次问 1–3 个相关问题，不要一次抛 10 个问题的问卷。
- 问完得到答复后，若还有新的模糊点继续追问，直到能用一段话把需求复述清楚。
- 澄清完成后，**先用自己的话把需求复述一遍给用户确认**，再进入实现或 Plan。

#### 需要问的例子

- 边界行为未定义（空输入 / 失败 / 超时 / 并发）。
- 范围模糊（"优化这个" —— 优化什么指标？什么算越界？）。
- 验收标准缺失（怎么算做完？跑什么测试？）。
- 存在多种合理技术路径且权衡不同（同步 vs 异步、内存 vs 磁盘 等）。
- 涉及共享状态 / 外部系统但没指明具体是哪个。

#### 不需要问的（自己看代码/配置就能确定）

- 已有命名/风格 —— 见 §1。
- 复杂度/测试规范 —— 见 §2、§3。
- 明显有默认值且用户显然不关心的琐碎细节 —— 自行选择并在回复中说明。

### 需求较大 → 分解后再确认

- **先拆再做**：把大需求拆成 2–6 个可独立交付的小需求 / 步骤。
- 每个子项写清楚：目标、涉及的模块 / 文件、验收标准。
- 将拆分方案给用户，**得到确认后再动手**。
- 确认后使用 `TaskCreate` 建立任务列表跟踪进度。
- 如果分解本身也依赖澄清（比如某一步不确定），先走"澄清"再走"分解"。

### 完成澄清 / 分解前的自检

1. 需求已用一段话复述并得到用户确认。
2. 若已分解：每个子项边界清晰、可独立完成、有验收标准。
3. 未在用户确认前动手写代码或改配置。

### 例外——不必澄清直接做

- 拼写 / 明显 typo 修复。
- 单点小改动且用户指令已经足够具体（"把这个函数里 A 改成 B"）。
- 用户明确说 "你决定就好 / 直接做"。

---

## 5. 设计文档同步（重构时）

Authority: `doc/design/*.md` 是「设计即契约」，与实现必须一致。详见 `.cursor/rules/doc-sync-on-refactor.mdc`。

### 触发时机

每次改动引擎业务逻辑（协议、线程模型、接口签名、载荷布局、时序、执行模型、网络收发）——不只新功能，**重构、修 bug 改行为、收敛机制**同样适用。

### 核查范围

**`doc/design/` 下的所有设计文档**——本次改动可能影响任何一份（不只是命令面）。改动后列出 `doc/design/` 全部文档，逐一判断是否触及；触及则更新，未触及则跳过。

### 核查清单（改完代码后）

1. **签名**：新增/改动的公开 API 与文档 API 表一致。
2. **载荷布局**：协议字节布局与文档一致，两端（测试 helper 与实现）一致。
3. **执行/线程模型**：文档描述与实际线程结构一致；目录锚点 = § 标题。
4. **行为决策**：写死决策有文档记录；废弃机制标注「已否决」。
5. **实现状态表**：「待实现/已实现」反映当前实现。
6. **跨文档引用**：各设计文档相互引用一致。
7. **已删除符号**：文档不得引用已删除的类/函数名。

### 硬性要求

- 代码逻辑改动完成后，先跑 `rg "旧函数名|旧术语|旧行为" doc/design` 自查残留。
- 找不到对应文档段落时主动补充（设计决策写「写死」）。
- 纯命名/格式改动不需同步文档；但目录锚点随 § 标题变更同步。

