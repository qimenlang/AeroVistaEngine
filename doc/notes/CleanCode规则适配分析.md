# Clean Code 规则适配分析（讨论稿）

> 状态：已部分落地——§4 中「注释纪律」「函数只做一件事」「错误处理与空值」「参数精简」4 条已落地为 `.cursor/rules/clean-code-guardrails.mdc`；其余条目待评审。
> 目的：判断哪些 Clean Code 原则值得做成 AI 规则，哪些不值得；给出推荐的落地方式与草案。

---

## 1. 结论先行

**有必要补一部分，但要有取舍、要分层。**

- 现有规则已经覆盖 Clean Code 中**机械可查**的部分（命名、格式、复杂度），这部分靠工具门禁即可，不需要再立规则。
- 值得补的是**语义性、工具查不了、但 AI 编码时高频违反**的部分（注释纪律、错误处理/空值策略、函数单一职责、参数精简、DRY）。
- 不建议把 Clean Code 全套照搬成规则——脱离上下文的原则写进规则只会变成空话或噪音。

---

## 2. 现状盘点：已有规则覆盖了什么

| 规则 / 机制 | 覆盖层面 | 手段 |
| ----------- | -------- | ---- |
| `.cursor/rules/cpp-vsg-style.mdc` | 命名、类型、格式 | clang-tidy naming + clang-format |
| `.cursor/rules/engine-complexity-gates.mdc` | 认知复杂度 ≤20、圈复杂度 ≤15 | clang-tidy + lizard + pre-commit/CI 门禁 |
| `.cursor/rules/engine-tests-atdd.mdc` | 测试角色 / 形式 / 验收 | 规范约束 |
| `.cursor/rules/anti-hallucination.mdc` | 行为约束 | 规范约束 |
| `.cursor/rules/requirement-clarification.mdc` | 需求澄清 / 分解 | 规范约束 |
| `.cursor/rules/clean-code-guardrails.mdc` | 注释纪律、函数单一职责、错误/空值策略、参数精简 | 规范约束（AI 自查 + review） |
| `doc/notes/代码质量指标.md` | 指标原理与门禁接线 | 文档 |

**结论**：Clean Code 的「机械维度」已有工具支撑；「语义维度」目前是空白，正是本讨论要补的。

---

## 3. 适配判断标准

一条原则适不适合做成 rule，按三个层次判断：

1. **工具能机械判定的** → 做成硬门禁（CI/pre-commit 失败）。现有复杂度、命名属于此类；以后新增的机械检查（如参数数量上限）也应走这条路。
2. **语义性、但 AI 高频违反、且项目内判断成本低** → 做成软规则（`.cursor/rules` 行为约束，AI 自查 + review 复核）。**这是最值得补的一层。**
3. **依赖大量业务上下文 / 主观权衡** → 只进 review checklist 或文档，不立硬性规则。

---

## 4. 逐原则分析

| Clean Code 原则 | 是否做成 rule | 理由 / 落地方式 |
| --------------- | ------------- | --------------- |
| 有意义的命名 | 已覆盖 | `cpp-vsg-style` + clang-tidy naming |
| 格式 | 已覆盖 | clang-format 全权负责 |
| 测试原则 | 已覆盖 | ATDD 规则（测试先行 / 验收契约） |
| **注释纪律**（解释 why/意图；禁止逐行复述；坏代码先重构再注释） | **强烈建议（软）** | 现有规则完全空白；AI 生成注释噪音是重灾区 |
| **错误处理与空值策略**（统一异常/错误码、不吞异常、错误带上下文、不裸返回 null） | **强烈建议（软）** | C++ 引擎稳定性关键；`vsg::ref_ptr` / `optional` 有客观基础 |
| **函数只做一件事 + 单一抽象层级** | **强烈建议（软）** | 复杂度门禁只查「缠不缠」，不查「做几件事」；语义互补 |
| **参数精简**（避免 boolean 入参、优先返回值、命令查询分离） | 建议 | `readability-function-size` 的参数选项可做硬上限（如 ≤5），其余软规则 |
| **消除重复 DRY** | 建议（软） | AI 复制粘贴重灾区；检测工具噪声大，只做告警/自查级 |
| 类单一职责 SRP | 建议（软） | 语义性，与模块划分强相关，判断成本中等 |
| 依赖方向 / 禁止新循环依赖 | 建议（软） | `代码质量指标.md` §3.4 已有依据，可提为编码自查项 |
| 魔法数字提取 | 可选 | clang-tidy 可开（`cppcoreguidelines-avoid-magic-numbers`），但需豁免清单，噪声控制成本高 |
| 系统架构 / 并发原则 | **不建议** | 粒度太大，写成规则会变成空话 |
| 注释率、函数行数硬上限 | **不建议** | 易被刷（gaming），且与复杂度门禁重叠/冲突 |
| 提前返回 / 卫语句 | 不建议单独立 | 认知复杂度已间接覆盖，可作为偏好写进函数规则 |

### 4.1 参数精简：boolean 入参 / 输出参数 / bool 返回值

这三者容易混，先澄清**不是同一回事**：

- **bool 返回值没有问题**，`isEmpty()`、`contains()` 这类谓词函数是好设计。
- 被反对的是 **boolean 入参**——调用处 `true`/`false` 没有语义，必须回看声明才知道含义；且函数内部必然 `if (flag) {...} else {...}` 两条不同行为路径，违背「函数只做一件事」。
- **输出参数**指 `void foo(T& out)` 这种结果靠引用塞出来的写法，调用处看起来像输入、实际是输出，没有记号、不可读、没法链式组合。

#### 为什么项目里会有「bool 返回值 + 输出参数」绑定的现状

这是 C 风格错误处理遗产：函数同时承担「执行」和「报状态」，结果塞引用、状态用 bool 报。真正的病根不是 bool 本身，而是**「结果」和「状态」被拆到两个地方**，这正是测试难写的来源。项目里典型：

```cpp
// 现状：bool 状态 + 输出参数
bool sampleEntityPoseById(int id, vsg::dvec3& positionOrLla, vsg::dvec3& eulerYprDeg) const;
bool entityName(int id, std::string& outName) const;
```

#### 更好的写法：结果即状态，测试反而更好写

```cpp
// 只需要成功/失败 → optional
std::optional<PoseYpr> sampleEntityPoseById(int id) const;

// 需要区分多种失败原因 → expected（C++23 或 tl::expected）
std::expected<PoseYpr, SampleError> sampleEntityPoseById(int id) const;
```

测试能断言「结果值」而不是只断言「成了没成」，更符合 ATDD 的验收契约：

```cpp
auto pose = engine.sampleEntityPoseById(42);
REQUIRE(pose.has_value());
REQUIRE(pose->eulerYprDeg.y == Catch::Approx(90.0));
```

项目里已在用 `std::optional`（`IgSync`、`CigiWire`），说明这条路走得通。

#### 但 C++ 输出参数不是绝对禁忌——规则要留口子

两个场景是合理惯例：

1. **性能热路径**：返回大对象（大 `vector`、`vsg::dmat4` 数组）时，复用调用方缓冲区避免反复构造。
2. **对接 C 风格第三方库**：cigi 库本身是 `bool cigi_xxx(...)` 风格；`createVulkanDevice(int& queueFamily)` 这类「同时要拿多个基本类型结果」也常见。

所以这条规则的准确措辞：

> 优先返回值；**当性能或对接第三方 API 需要时**，输出参数可以用，但调用处/命名要清晰（参数名带 `out`/`result` 前缀）。

#### ❌/✅ 例子

```cpp
// ❌ boolean 入参，调用处不可读，函数内部必然分两路
void render(SceneNode& node, bool picked);
placeModelFromPayload(payload, true);

// ✅ 拆函数 / 带语义的枚举
void renderPicked(SceneNode& node);
void placeModelForeground(payload);

// ❌ 输出参数，调用处看不出 name 是结果
bool entityName(int id, std::string& outName) const;

// ✅ 结果即状态
std::optional<std::string> entityName(int id) const;

// ✅ 性能/第三方 API 场景允许输出参数，但命名带 out/result
bool createVulkanDevice(int& outQueueFamily);
```

#### 注意：不要因果倒置

「为了能测试所以返回 bool」——测试**不应**决定 API 形状，API 由调用语义决定，测试顺应 API。如果测试逼着把所有函数压平成 `bool` + 输出参数，通常是函数本该返回结果却被压平，或测试在断言「调用成功」这种弱断言。接受性测试应断言可观察的结果（值、副作用），`optional`/`expected` 比 bool 断言得更准。

---

## 5. 推荐的落地方式

与工具门禁**分工不重叠**：

```text
工具门禁（已有）   → 结构不超限：复杂度 / 命名 / 格式
软规则（待补）     → 语义不烂：注释 / 错误处理 / 函数职责 / DRY
```

新增一个软规则文件，例如 `.cursor/rules/clean-code-guardrails.mdc`（globs 同 engine），骨架草案：

```markdown
# Clean code guardrails (engine)

语义性约束：工具查不了，靠 AI 自觉 + review 复核。
互补关系：engine-complexity-gates 管结构，本规则管语义。

## 1. Comments
- 注释解释 why / 意图；禁止逐行复述代码。
- 坏代码先重构，不靠注释解释。
- ❌  // increment counter
- ✅  // depth-first so children appear before parent in the sort

## 2. Functions
- 一个函数一个抽象层级；明显可拆就拆。
- 参数 ≤ 3~4；禁止 boolean 参数；避免输出参数（用返回值）。
- 优先卫语句提前返回，减少嵌套。

## 3. Errors & null
- 遵循模块既定错误策略；不吞异常；错误信息带上下文。
- 不返回裸 null；优先 optional / vsg::ref_ptr。

## 4. DRY
- 复制粘贴超过 1 处即提取；改动时优先复用现有接口。

## Before finishing an edit
- 自查本清单；评审时逐条过。
```

要点：

- 每条都要带 ❌/✅ 具体例子（仿照 `cpp-vsg-style`），否则 AI 会把抽象原则理解偏。
- 只保留高价值、低噪声的 4~6 条，不贪多。
- 若某条后续能自动化（如参数上限），从软规则迁移到工具门禁，规则里删掉该条。

---

## 6. 演进建议

1. 先以讨论稿评审，确认原则取舍。
2. 落地为软规则文件后，试用 1~2 周看是否产生「假阳性」或与现有门禁冲突。
3. 定期把「软规则里能机械化的条目」迁移到 clang-tidy / 脚本。
4. 阈值与豁免策略演进参照 `代码质量指标.md` §5 的增量门禁思路。
