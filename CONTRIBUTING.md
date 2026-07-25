# 为 AeroVistaEngine 贡献代码

本项目基于 [VulkanSceneGraph (VSG)](https://github.com/vsg-dev/VulkanSceneGraph)。引擎代码应遵循 **VSG 的命名与风格约定**，使引擎 API 与 `vsg::` API 在同一处调用时读起来一致。

**不要**对引擎或场景图代码套用 Google C++ Style 的命名习惯（例如方法用 PascalCase、成员用带尾下划线的 `snake_case`）。

新代码必须遵守本文档。修改已有代码时，若改动范围小且局部，可顺带把附近命名对齐；避免无关的大规模重命名。

---

## 命名一览


| 类别                   | 风格              | 示例                                   |
| ---------------------- | ----------------- | -------------------------------------- |
| 命名空间               | 全小写            | `aerovista`, `vsg`                     |
| 类 / 结构体 / 枚举类型 | PascalCase        | `Engine`, `SynchronSystem`             |
| 方法 / 自由函数        | camelCase         | `renderOneTick`, `addChild`            |
| 公有数据成员           | camelCase，无前缀 | `extent`, `children`                   |
| 私有 / 保护成员        | `_camelCase`      | `_viewer`, `_syncSystem`               |
| 局部变量 / 参数        | camelCase         | `frameStart`, `modelPath`              |
| 与成员同名的构造参数   | `in_` + camelCase | `in_matrix`, `in_width`                |
| 类型别名（容器等）     | PascalCase        | `Children`, `Windows`                  |
| 数学 / GLSL 风格别名   | 小写              | `vec3`, `dmat4`（优先用 VSG 已有别名） |
| 智能指针模板           | snake_case（VSG） | `vsg::ref_ptr<T>`                      |
| 枚举常量 / 宏          | `SCREAMING_SNAKE` | `LOGGER_INFO`, `AEROVISTA_…`           |
| 主类对应的源文件       | 与类名一致        | `SynchronSystem.h` / `.cpp`            |


术语说明：

- **PascalCase（帕斯卡命名）**：每个单词首字母大写，无下划线，如 `SynchronSystem`。
- **camelCase（驼峰命名）**：首单词小写，后续单词首字母大写，如 `renderOneTick`。
- **snake_case（蛇形命名）**：小写单词以下划线连接，如 `ref_ptr`。
- **SCREAMING_SNAKE（全大写蛇形）**：大写的 snake_case，如 `ONE_TIME`。

---



## 命名空间

- 优先使用单一顶层引擎命名空间（例如 `aerovista`），与 `vsg` 并列，**不要**嵌套在 `vsg` 内部。
- 按**目录**划分模块，而不是嵌套命名空间（与 VSG 相同：如 `nodes/`、`app/`、`function/sync/`）。
- 第三方名称留在各自命名空间中（`vsg::`、Catch2、以及 CIGI 等库提供的类型名）。

```cpp
namespace aerovista
{
    class Engine { /* ... */ };
}

// 使用 VSG 类型时带上其命名空间：
vsg::ref_ptr<vsg::Viewer> viewer;
```

在 `.cpp` 中，文件内辅助函数可用匿名命名空间。

---



## 类型（类、结构体、枚举）

- 类型名使用 **PascalCase**。
- 参与 VSG 对象模型、需要引用计数的场景图对象，应使用 `vsg::Inherit`：

```cpp
class SynchronSystem : public vsg::Inherit<vsg::Object, SynchronSystem>
{
public:
    void initialize();
    void update();
    void shutdown();
};
VSG_type_name(aerovista::SynchronSystem); // 按 VSG 方式注册类型时使用
```

- 引用计数 / 行为型类型用 `class`；小型 POD 聚合（数学量、选项包等）用 `struct`，与 VSG 习惯一致。
- 嵌套实现类型可用 `Implementation` 这类名字（参见 VSG 的 `DescriptorSet::Implementation`）。
- 枚举**类型名**用 PascalCase；枚举**枚举值**用 `SCREAMING_SNAKE`（可按概念加前缀）：

```cpp
enum RunBehavior
{
    ONE_TIME,
    ALL_FRAMES
};
```

---



## 函数与方法

- 几乎所有成员方法与自由函数使用 **camelCase** 动词或动词短语。
- 方法**不要**使用 PascalCase（新代码中 `Initialize`、`CaptureToFile` 等写法不正确）。

```cpp
// 推荐
bool init(const vsg::Path& modelPath);
bool renderOneTick();
bool captureToFile(const vsg::Path& outputPngPath);
void addChild(vsg::ref_ptr<vsg::Node> child);

// 避免
bool CaptureToFile(const vsg::Path& outputPngPath);
void Initialize();
```

与 VSG 对齐的常见模式：


| 模式        | 示例                                    |
| ----------- | --------------------------------------- |
| 修改 / 动作 | `addWindow`, `pollEvents`, `close`      |
| 访问器      | `getFrameStamp`, `windows()`            |
| 工厂        | 通过 `Inherit` 提供的静态 `create(...)` |
| 访问者      | `accept`, `traverse`, `apply`           |
| 读写        | `read`, `write`, `compare`              |


引擎命名空间中的自由函数同样使用 camelCase；若镜像 VSG 辅助 API，也可沿用 VSG 中的 snake_case 一族（如 `compare_value`、`read_cast`）。

---



## 变量



### 公有数据成员

- **camelCase**，不加 `m_`，也不加尾下划线 `_`。
- 在无需强封装的场景 / 配置类型上，直接暴露公有字段是 VSG 风格中的常见做法。

```cpp
class Engine
{
public:
    VkExtent2D extent{1920, 1080};
    bool showWindow = true;
};
```



### 私有 / 保护成员

- 前导下划线 + camelCase：`_viewer`、`_syncSystem`。
- 新代码中**不要**使用 `m_syncSystem` 或 `sync_system_`。

```cpp
private:
    vsg::ref_ptr<vsg::Viewer> _viewer;
    vsg::ref_ptr<SynchronSystem> _syncSystem;
```



### 局部变量与参数

- camelCase：`frameStart`、`modelPath`、`nearFarRatio`。
- 若构造函数参数会与成员同名遮蔽，加 `in_` 前缀：

```cpp
explicit MatrixTransform(const dmat4& in_matrix) :
    matrix(in_matrix) {}
```



### 缩写

- 引擎自有 API 优先用可读全称，避免晦涩缩写（用 `address`、`outgoingMessage`，而不是 `addr`、`OmsgPtr`）。
- **第三方**类型名保持库本身的写法（如 `CigiIGSession`、`VkExtent2D`）；封装侧的成员仍应使用清晰的引擎命名。

---



## 类型别名与智能指针

- 容器 / 集合别名：PascalCase 复数形式，与 VSG 一致（`Children`、`Windows`）。
- VSG 对象优先使用 `vsg::ref_ptr<T>` / `vsg::observer_ptr<T>`；使用 `Inherit` 时用 `Type::create(...)` 创建。
- 数学类型：复用 VSG 别名（`vsg::vec3`、`vsg::dmat4` 等），不要另起一套平行名字。

```cpp
using Children = std::vector<vsg::ref_ptr<vsg::Node>>;
Children children;

auto group = vsg::Group::create();
```

---



## 常量与宏

- 枚举值与编译期特性宏：`SCREAMING_SNAKE`。
- 引擎自有宏 / CMake 可见宏定义：加 `AEROVISTA_` 前缀（或团队约定的短前缀），避免冲突。
- 能用 `static constexpr` 或 `enum` 时，尽量少用宏。
- 新头文件优先 `#pragma once`（仅在维护旧文件时保留 `#ifndef` 头卫）。

```cpp
static constexpr double nearFarRatio = 0.001;
#define AEROVISTA_SOME_FEATURE 1
```

---



## 文件与目录


| 项目                      | 约定                                    | 示例                                     |
| ------------------------- | --------------------------------------- | ---------------------------------------- |
| 一个主类对应一对文件      | 文件名与类名一致                        | `SynchronSystem.h`、`SynchronSystem.cpp` |
| 小型工具 / 自由函数头文件 | 小写或 snake_case（VSG 风格）           | `read.h`、`compare.h`、`ref_ptr.h`       |
| 目录                      | 小写，按功能划分                        | `engine/source/function/sync/`           |
| 头文件                    | `#pragma once`；包含 VSG 用 `<vsg/...>` | `#include <vsg/nodes/Group.h>`           |


在引入独立的公开 include 树之前，头文件与源文件一并放在 `engine/source/` 下。

---



## 格式化

- 对齐 VSG 的格式意图（见 `thirdparty/vsg/.clang-format`）：4 空格缩进、类型/函数使用 Allman 风格大括号、指针左对齐（`T* p`）。
- 若项目已有或后续添加 `.clang-format`，对改动到的 C++ 文件优先按与 VSG 相同的风格执行 `clang-format`。

---



## 遗留代码与第三方代码

- `thirdparty/` 下的**厂商代码**保持上游风格；不要为迁就本文档而重排或重命名。
- **引擎遗留文件**（例如仍带 snake_case 参数或 `m`_ 前缀的旧网络代码）不应继续扩散该风格。优先用符合 VSG 命名的薄适配层；在对该文件做实质性修改时再重命名。
- 不要另发明第三套内部风格。拿不准时，对照 `thirdparty/vsg/include/vsg/` 中邻近头文件的命名即可。

---



## 速查（推荐 vs 避免）

```cpp
namespace aerovista
{
    class FrameStatsHandler : public vsg::Inherit<vsg::Visitor, FrameStatsHandler>
    {
    public:
        bool enabled = true;                 // 公有：camelCase，无前缀

        void addSample(double frameSeconds); // 方法：camelCase
        static vsg::ref_ptr<FrameStatsHandler> create();

    protected:
        double _lastFrameSeconds = 0.0;      // 私有/保护：_camelCase
    };

    bool compareImages(const Path& a, const Path& b); // 自由函数：camelCase
}

// 新引擎代码中应避免：
//   void Initialize();
//   bool CaptureToFile(...);
//   int m_count;
//   int count_;
//   void render_one_tick();
```

---



## 拉取请求（PR）

- 改动保持聚焦；除非为表达清晰所必需，不要把无关重命名与功能开发混在同一提交中。
- 若顺带对齐了遗留符号命名，请在 PR 说明中写明。
- 测试与黄金图资源使用清晰、一致的命名（如 `renderingTests`，避免拼写错误）。

