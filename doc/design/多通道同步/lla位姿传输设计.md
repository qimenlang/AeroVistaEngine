# LLA 位姿传输设计

同步层只支持 LLA（2026-09 收敛）：Host→IG 相机同步恒传 **LLA + 当地姿态**（`Detach`+LLA）。  
帧时序、连接态、无新包 / 断线沿用 [多通道同步模块设计.md](./多通道同步模块设计.md) §4–§5（防回声已随拆进程移除）；坐标系背景见 [坐标系统总结.md](../../notes/坐标系统总结.md)。  
本文取代「仅做场景 JSON 装配、暂不动同步」的前置草案方向（见 [坐标系统模块设计.md](./坐标系统模块设计.md)），把范围收束到 **能传、能采、能写** 的 LLA 同步闭环。

---

## 目录

1. [目标与范围](#1-目标与范围)
2. [EllipsoidModel 装配](#2-ellipsoidmodel-装配)
3. [位姿语义](#3-位姿语义)
4. [Engine / SynchronSystem 路径](#4-engine--synchronsystem-路径)
5. [报文（CIGI）](#5-报文cigi)
6. [配置](#6-配置)
7. [验收要点](#7-验收要点)
8. [不做](#8-不做)
9. [实现顺序](#9-实现顺序)

---

## 1. 目标与范围

**做**：

| 项 | 说明 |
| --- | --- |
| 权威眼点 | 椭球模式下 Host→IG 传 **LLA + YPR（当地）** |
| 写相机 | IG：`LLA→ECEF`，在 ENU 叠 `offsetDeg`，再写 `LookAt` |
| 采样出站 | **已随拆进程移除**（2026-08）：Host 为 viewhost、键盘累积直接产出 LLA + 当地 YPR（不经 LookAt 采样，见 [viewhost设计.md](../viewhost设计.md) §4.2） |
| 本地场景 | 仅单机渲染，不参与同步（无 `igConfig`；本地 = 场景无椭球，见 §2.1） |

**不做**（见 §8）：仅为演示把普通模型钉到某 LLA、本迭代强制依赖瓦片。实体 / 相机配置结构见 [位姿配置设计.md](./位姿配置设计.md)。

成功标准：同一套帧循环下，Host 与全体 IG 在椭球场景中共用眼点；各通道 frustum 仍由本地 `offsetDeg` 区分。

**本迭代保证与不保证**：

| 保证 | 不保证 |
| --- | --- |
| LLA 位姿采 / 传 / 写在数值与同步语义上正确 | `injectEllipsoidIfMissing=true`（或启 IG 同步自动注入）+ 普通 `.vsgt` 且未钉到 ECEF 时，画面「看得见合理地球场景」 |
| 有 `EllipsoidModel` 后投影 / Trackball / 跟拍走椭球路径 | 无瓦片、模型仍在本地原点时的可视化观感 |

装配与可视化边界见 §2.6。

---

## 2. EllipsoidModel 装配

本节收束：**配置意图 → 场景加载后可选注入 → 运行时只认场景有无 `EllipsoidModel`**。  
`injectEllipsoidIfMissing`、注入时机、半径、冲突、默认初始相机均在此定义；其它章节只引用本节结论。

### 2.1 判据（因果一条链）

不是两套互斥开关，而是先后关系：

| 层 | 职责 |
| --- | --- |
| JSON `injectEllipsoidIfMissing`（或启 IG 同步） | **意图**：模型**未**自带椭球时，是否要注入 `EllipsoidModel` |
| 场景上有无 `EllipsoidModel` | **运行时唯一判据**：读 pose 哪半 / 是否参与同步（同步只 LLA，2026-09 收敛） |

注入意图**只**驱动「是否注入」，**不**单独决定组包类型。  
「World = ECEF」是约定：有 `EllipsoidModel` 后 LookAt / Transform 用地心笛卡尔米制。瓦片只提供地表几何，**不是**模式开关。

### 2.2 配置字段 `injectEllipsoidIfMissing`

通道 JSON 顶层可选字段（方案 A；非 bool 解析失败）：

```text
injectEllipsoidIfMissing?: bool   // 缺省 = false
```

| 注意 | 说明 |
| --- | --- |
| 取值语义 | 无自带椭球时是否注入 WGS-84 椭球 |
| 生效范围 | **仅单机渲染（无 `igConfig`）**；启 IG 同步时引擎自动注入，无需此开关 |
| 场景对象 | 挂到 scene 上的类型名才是 `vsg::EllipsoidModel`（勿与配置取值混用） |
| HELLO | **不**携带 / 协商注入意图（第一版） |

> **坐标系判据（写死，2026-09）**：眼点与命令实体的坐标系**运行时判据统一为「场景有无 `EllipsoidModel`」**（§2.1）；`injectEllipsoidIfMissing` 只在「场景无椭球」时决定是否注入，自带椭球则一律保留（无 fail-fast——运行时只看场景判据，与注入开关解耦）。**同步层只支持 LLA**：眼点与命令实体位姿在线格式恒为 `Detach`+LLA（§5），无 `Attach`+XYZ 路径。命令实体见 [实体与运动控制设计.md](./实体与运动控制设计.md) §4.2。

### 2.3 装配流程（保留 / 注入 / 相机）

**写死顺序**（实现与验收同一条链；判据对象为场景上名为 `"EllipsoidModel"` 的对象）：

```text
1. 解析通道 JSON
   - 得到 injectEllipsoidIfMissing 意图；非 bool → 解析失败（方案 A）
2. loadScene(model)
3. 查询场景是否已有 "EllipsoidModel"
   ├─ 已有（如 readymap.vsgt）→ 保留：不删除、不替换半径（自带椭球即椭球场景）
   └─ 没有
        → 有 igConfig（启 IG 同步）→ 注入 EllipsoidModel::create()（WGS-84，§2.4；同步只 LLA 需椭球）
        → injectEllipsoidIfMissing == true → 注入 EllipsoidModel::create()（WGS-84，§2.4；单机椭球渲染）
        → 否则 → 不注入 → 本地笛卡尔（仅单机渲染）
4. 若此时场景有 EllipsoidModel
   → 写默认初始 LookAt（见 [位姿配置设计.md](./位姿配置设计.md) §4）；禁止再用 AABB + 世界 Z-up
5. 创建 Projection / Trackball
   → 有椭球：EllipsoidPerspective + 椭球 Trackball
   → 无椭球：普通透视
   （禁止在本步之后再补挂 EllipsoidModel）
6. 此后同步 / 组包 / 写相机只读场景有无 EllipsoidModel：
   无 → 单机本地渲染（不参与同步）
   有 → LLA + setCameraPoseLla（§4；Engine 持有椭球引用，无则 API 返回 false）
```

一句话：**只有场景加载完且没有 `EllipsoidModel` 时，才按「启 IG 同步 / `injectEllipsoidIfMissing`」决定是否注入**；已有则一律保留。

步骤 4 的默认初始相机：见 [位姿配置设计.md](./位姿配置设计.md) §4。按 AABB 计算 centre / radius，分别用 Local 或 Ellipsoid 公式生成 LookAt，不再写死北京为唯一默认。

### 2.4 椭球半径（Host / IG 一致）

| 来源 | 半径 |
| --- | --- |
| 模型自带（如 `readymap.vsgt`） | **沿用文件内** `EllipsoidModel`（勿替换为默认） |
| `injectEllipsoidIfMissing=true` 或启 IG 同步注入 | `EllipsoidModel::create()` → VSG 默认 **WGS-84**（赤道 6378137，极 6356752.3142） |

`readymap` 内半径（6378140 / 6356750）与 WGS-84 略有差异。同一会话内 Host 与各 IG **不得混用**「一边读瓦片椭球、一边注入默认 WGS-84」；否则同一 LLA 算出的 ECEF 可差**米级**——线上看眼点数字一致，LookAt.eye 却对不齐；肉眼难辨，普通跟拍测试也难覆盖。

第一版 **HELLO 仍不传半径**（与不传注入意图同级）；靠部署一致 + 可观测 / 测试兜底：

| 项 | 要求 |
| --- | --- |
| 日志 | 场景装配完成（保留或注入之后）打印一次：`radiusEquator`、`radiusPolar`、来源（`model` / `inject-WGS84`），级别至少 `INFO` |
| 测试 | BDD **显式**覆盖「两端半径不一致」：例如 Host=`readymap`、IG=`injectEllipsoidIfMissing:true` 无瓦片注入；预期为 **fail**（跟拍 ECEF 超差）或标记 **`[skip]`/文档化已知错配**——二者选一并在用例名写清，禁止默默绿过 |
| 同端自检（推荐） | 单进程内可 assert 当前 `EllipsoidModel` 半径与期望来源一致；跨进程第一版不强制互查 |

后续若要硬防：可在 HELLO 或首帧附带半径，不符则 ERROR + 计数器（类似 §4.5）；非本迭代必做。

### 2.5 冲突、多通道与部署约束

| 情况 | 策略 |
| --- | --- |
| Host / 各 IG | **配置意图应一致**（人工 / 部署约束）；参与同步的 IG 均应有场景 `EllipsoidModel`（有 `igConfig` 时引擎自动注入，§2.3） |
| 跨进程 init | **不**互查注入意图 / 半径 |
| 本地场景 + `igConfig` | 引擎自动注入椭球（2026-09 收敛，取代原「fail-fast 拒绝」——同步只 LLA，要同步就该有椭球，注入即可） |

依赖：(1) 各通道 JSON 部署一致；(2) 场景 `EllipsoidModel` 为运行时唯一判据。此为明确假设，不是遗漏实现。

### 2.6 与可视化 / 摆模的边界

| 术语 | 含义（本文） |
| --- | --- |
| **摆模** | 口语：把普通本地 `.vsgt` 经 `LocalToWorld` **摆放 / 钉到** 某 LLA（ECEF），便于肉眼看见模型；≠ 建模（做几何） |

- **本迭代不要求摆模**：无摆模时仍可用于单测与 BDD（自挂 `EllipsoidModel` + 断言 ECEF / LLA）。
- **联调观感**需瓦片（如 `readymap`）或另开摆模工作；画面空不表示 LLA 同步语义错误（见 §1 不保证项）。

---

## 3. 位姿语义

### 3.1 位置

- **对外 / 线上**：`lat°`、`lon°`、`alt`（米，相对椭球面）
- **渲染**：`EllipsoidModel::convertLatLongAltitudeToECEF` → `LookAt.eye`（ECEF）

### 3.2 姿态（当地 YPR）

- **参考系**：眼点 LLA 处的 **ENU**（东–北–天），由 `computeLocalToWorldTransform(lla)` 给出。
- **角度单位**：度；旋转顺序与本地模式一致：`Rz(yaw) * Rx(pitch) * Ry(roll)`，但轴在 **ENU 局部**解释，不是 ECEF 世界轴。
  - **对向量的生效顺序（钉死）**：先 roll、再 pitch、最后 yaw。**实现禁止**写成 `Qz*Qx*Qy` 连乘（VSG 四元数乘法是 **reverse-Hamilton**，`M(a*b)=M(b)·M(a)`，连乘实际得到 `Ry·Rx·Rz`）；须逐个作用 `roll→pitch→yaw`。同一约定用于写入、采样反解、`offsetDeg` 合成（§3.4）全链路。
- **轴约定（钉死实现时以此验收）**：

```text
ENU 局部：+X = East，+Y = North，+Z = Up（天）
前向（机头）：局部 +Y（北）
右侧：局部 +X（东）——右手系：facing +Y、up +Z ⇒ right = +X
上向：局部 +Z（天）
yaw=0 → 朝北
```

- **偏航符号（与本地模式同一套，方案 a）**：旋转仍为右手系 `Rz(+yaw) * Rx(pitch) * Ry(roll)`，**不**为迁就「航空航向顺时针为正」而改成 `Rz(-yaw)`。

```text
Rz(+θ) * (0,1,0) = (−sin θ, cos θ, 0)
⇒ yaw = +90° → (−1, 0, 0) = 西 = ENU −X
⇒ +yaw = 左转（朝西），不是右转朝东
```

本地世界系已约定同一事实：`Rz(+yaw)` 转向 **−X / 左侧**（见 [多通道同步模块设计.md](./多通道同步模块设计.md) §3.2），故左通道 `offsetDeg.yaw = +hFOV`、右通道 `−hFOV`。椭球 ENU 下沿用：**+yaw → 西 / 左**；邻通道符号不变。  
若对外需要「航向角顺时针从北起算」的 UI/任务语义，在进出本引擎 YPR 时单独换算（`heading_cw = -yaw` 或等价），**不要**改同步与 `setCameraPoseLla` 内部符号。

- **禁止**：把当地航向角直接塞进现有 `setCameraPose` 的世界轴 YPR（ECEF 的 +Z 是地轴，不是当地天）。同步层禁止在椭球模式下调用 `setCameraPose(lla或ecef, 当地YPR)` 冒充写入。
- **与 CIGI ICD**：EntityPosition 的 Yaw/Pitch/Roll 在标准文档中常作 Heading（多为顺时针）等，轴系未必等同本节。本项目规定：**线字段按本节（含 +yaw→西）解释，作为应用层约定**；不声称与任意第三方 CIGI 视景姿态一一兼容。

### 3.3 LookAt 构造（椭球）

```text
R_local = Rz(yaw) * Rx(pitch) * Ry(roll)     // 在 ENU 基下
forward_enu = R_local * (0, 1, 0)            // 方向，非点
up_enu      = R_local * (0, 0, 1)
M = computeLocalToWorldTransform(lla)        // ENU → ECEF
R = M 的 3×3 线性部分                         // 只旋转方向，勿把方向当点做 M * vec4(dir,1)
eye     = convertLatLongAltitudeToECEF(lla)
forward = normalize(R * forward_enu)
up      = normalize(R * up_enu)
center  = eye + forward * d                  // d：视线距，见下
→ LookAt(eye, center, up)
```

`up` **不得**固定为世界 `(0,0,1)`，也 **不宜**把 `normalize(ecef_eye)` 当作正式天向；正式用 ENU 的 Up 经 `R` 旋转。

#### 视线距 `d`（ECEF 下）

| 说法 | 判定 |
| --- | --- |
| `eye≈6.37e6` 时 double 下 `d=1` 精度不够 | **不成立**。double 在该量级 ULP≈1e-9 m，`eye+1` 与 `center-eye` 恢复方向足够 |
| 若下游用 **float** 存 `eye`/`center` 再做差 | **成立**。float 在 6e6 量级 ULP≈0.5–1 m，`float(eye+1)` 常与 `float(eye)` 相同 → 方向塌缩 |
| 因此改 `d=1000` 是「零成本保险」 | **不成立**。VSG `Trackball` 用 `length(center-eye)` 作平移/绕转尺度（见相机驱动总结）；`d` 从 1→1000 会把交互半径放大约 1000 倍。椭球 Trackball 还用 `center` 参与贴地等计算 |

**第一版约定**：

- 主路径（`LookAt` / view matrix）保持 **double**；`d` 与本地模式对齐，默认 **`d = 1.0`**（或与现 `setCameraPose` 的 `kLookDistance` 同一常量）。
- **不要**为 float 隐患盲目改 1000 而不评估 Trackball。若 IG 侧相机交互依赖 Trackball，改 `d` 须单独验收操作手感。
- 若确有 float 世界算子（阴影、拾取等）直接消费 ECEF 点：靠 **原点回退 / `CoordinateFrame`** 等精度方案，而不是指望加大 `d` 一劳永逸。
- 实现若发现某条 float 路径必须从 `center-eye` 取方向，可在该处用 double 算方向再窄化，或仅在那条路径用更大临时偏移——与写入 `LookAt` 的 `d` 解耦。

### 3.4 通道 `offsetDeg`

**刚性阵列旋转复合（rigid-array）**：offset 在 Host 眼点自身姿态系内旋转（机体系），再按 §3.2 约定反解 YPR 写相机。yaw 偏移绕 **Host 自身 up**（本地世界 Z-up / 椭球 ENU Up 经 Host 姿态旋转后的方向）旋转；因此 Host 有非零 roll 时各通道 up 轴保持平行，frustum 贴边不撕开。

```text
R_host = Rz(yaw)*Rx(pitch)*Ry(roll)                  // §3.2 写约定（ENU 基，椭球）
R_ig   = R_host · Rz(δy) · Rx(δp) · Ry(δr)           // offset 右乘：在 Host 自身系内旋转
反解 R_ig 得 composed YPR（与写约定同一套）→ 写 LookAt（lla 不变）
```

实现为 `SynchronSystem::compose`（本地 / 椭球同一套）。左 `+hFOV`、右 `−hFOV` 符号不变（§3.2）。

#### 仅 yaw 偏移：`R_ig = R_host · Rz(δ)`，up 轴平行（严格成立）

约定 `R(ypr) = Rz(yaw)*Rx(pitch)*Ry(roll)`。当 `offsetDeg.pitch = offsetDeg.roll = 0`、仅 `δ = offsetDeg.yaw` 时：

```text
R_ig = R_host · Rz(δ)
up_ig      = R_host · Rz(δ) · Up = R_host · Up = up_host       // Up：写约定下的 up 基（本地 Z / ENU Up）
forward_ig = R_host · (Rz(δ) · ForwardBase)                     // 绕 Host 自身 up 转 δ
```

即：**在 Host 姿态之上绕同一套 up 轴（显示阵列竖直轴）再转 δ**。各通道 `up` 完全一致，roll 时刚体阵列一起 roll，公共棱线连续。

> **分量相加（`ypr ⊕ offset`）为什么不成立**：它等价于 `Rz(y+δ)*Rx(p)*Ry(r) = Rz(δ)·R_host`，yaw 偏移绕**地理 Up**（世界 Z / ENU Up）左乘。当 Host `roll≠0` 时 `up_ig = Rz(δ)·up_host ≠ up_host`，各通道 up 拧散、frustum 不再贴边——即线上 roll 撕裂 bug。早期实现曾用 `Ry·Rx·Rz` 约定让分量相加巧合等价于 rigid-array；为保证写 / 采遵循同一 `Rz·Rx·Ry` 约定，统一改为显式旋转复合。

#### 第一版约束：`offsetDeg` 仅 yaw 有定义

| 字段 | 第一版 |
| --- | --- |
| `offsetDeg.yaw` | 用于邻通道水平拼接（左 `+hFOV`，右 `−hFOV`，符号见 §3.2 / 同步设计 §3.2） |
| `offsetDeg.pitch` / `roll` | 有定义要求为 **0**；**≠0 不测试**（未定义行为：欧拉分量相加一般≠绕两轴的复合旋转） |

验收只覆盖「Host 可含 pitch/roll + 通道仅 yaw 偏移」。实现按显式旋转复合（右乘 `Rz*Rx*Ry(offset)`），pitch/roll 通道偏移语义已由同一公式定义，只是**不测试**。禁止退回三分量相加冒充复合旋转。

第一版 **不**做通道平移偏移。

### 3.5 采样（LookAt → LLA + YPR）——已随拆进程移除

> **（2026-08 拆进程）**：Host 采样原由 engine 的 `HostPosePublisher`（`lookAtToLlaEye` / `lookAtToWorldLocalEye`）承担；engine 不再承担 Host 后该路径删除。Host 眼点由 viewhost 键盘累积直接产生 LLA + 当地 YPR（`applyManualStep`，见 [viewhost设计.md](../viewhost设计.md) §4.2），**不经 LookAt 采样**。下列换算仅作历史参考 / 线格式测试锚定。

```text
lla     = convertECEFToLatLongAltitude(eye)
M_inv   = computeWorldToLocalTransform(lla)
用 M_inv 的 3×3 将 ECEF forward/up 变到 ENU
再按与 §3.2 同一套约定解 yaw/pitch/roll
```

#### VSG `LocalToWorld` 轴（与 §3.2 一致，实现勿猜）

`EllipsoidModel::computeLocalToWorldTransform`（VSG）：列向量基为 **第 0 列 East、第 1 列 North、第 2 列 Up**（`operator()(column,row)`）。故 ENU 下 `(0,1,0)` 经 `R` 即 North，与「前向 = +Y = 北」一致。

奇异（俯仰 ±90°、近极点）：与本地系一样存在数值问题。验收以**中低纬**容差为主；近极点 / Trackball 任意拖拽后的往返 **不作为第一版硬性保证**（可另加用例标注）。

---

## 4. Engine / SynchronSystem 路径

### 4.1 API

| API | 角色 |
| --- | --- |
| `setCameraPose(position, eulerYprDeg)` | **保留**：仅本地模式；或调用方已给出 **当前 World 笛卡尔 + 世界轴 YPR** 的底层写入 |
| `setCameraPoseLla(lla, eulerYprDeg)`（名称可微调） | **新增**：椭球写入口；`eulerYprDeg` 为 §3.2 当地 YPR；内部按 §3.3 写 LookAt；无 `EllipsoidModel` 时返回 `false` |

同步层在椭球模式下 **必须**走 `setCameraPoseLla`（或内部等价、且姿态已按 ENU→ECEF 处理的路径）。禁止把当地 YPR 直接传入 `setCameraPose`。

`setCameraPoseLla` / 采样换算所需的 `EllipsoidModel`：见 §2.3 步骤 6。

**Host 无 offsetDeg（2026-08 拆进程）**：Host 为独立 viewhost 进程，不施加通道偏移，也不存在「权威窗 ⊕ offset 后采样再广播」的双重叠加问题（原约束源于 engine 同进程 Host+IG，已随拆进程移除）。各 IG 的 `offsetDeg` 只在各自 IG 进程施加。

### 4.2 `HostEyePose`（只 LLA，无 frame 判别）

**同步层只支持 LLA（2026-09 收敛）**：`HostEyePose` 不再需要 `frame` 判别字段，`position` 恒为 LLA（纬度°、经度°、海拔 米），`eulerYprDeg` 恒为当地 ENU YPR（§3.2）。

```text
struct HostEyePose {
  DVec3 position{};     // 纬度°、经度°、海拔 米（LLA）
  DVec3 eulerYprDeg{};  // 当地 ENU YPR（度）
};
```

**实现现状（2026-09）**：`HostEyePose`（`SyncConfig.h`）已删除 `HostEyeCoordFrame frame` 枚举，只保留 `DVec3 position`（LLA）+ `DVec3 eulerYprDeg`（当地 ENU YPR）。业务侧回调（`Engine::onEntityPositionCtrl`）按 `EntityID` 分流——ownship 眼点翻译为 LLA 入队决策器、命令实体摆放恒 `Detach`+LLA。原「本地 XYZ / 椭球 LLA 双语义 + variant」讨论随同步只 LLA 移除，不再需要编译期判别。

### 4.3 帧路径（IG 侧；Host 采样/扇出已随拆进程移除）

```text
update:
  handleEvents
  SynchronSystem::update()      // 决策；宿主取 takePendingCameraPose → setCameraPose / setCameraPoseLla
postFrame:
  无扇出（Host 眼点由 viewhost 独立扇出，见 viewhost设计.md §4）
```

无新包 / Freeze / ReuseLast / 断线保末帧：逻辑不变，缓存带位置类型的 `HostEyePose`。

**stale vs 防回声（一句话）**：

| 术语 | 含义 |
| --- | --- |
| **stale** | 本帧**没有**新的 Host 眼点包 → 按 `hostEyeStalePolicy`（`ReuseLast` / `Freeze`）决定是否仍用缓存写相机；细节沿用 [多通道同步模块设计.md](./多通道同步模块设计.md) §4.4 |
| **防回声** | **已随拆进程移除**（2026-08）：engine 不再采样出站，原「权威窗回灌后相机未再动则不广播」只存在于同进程 Host+IG，viewhost 无回灌相机、无该问题 |

二者正交：stale 管「没新包怎么办」；防回声已无宿主侧（§4.4 随拆进程移除）。

#### 场景重建时的缓存

注入须在相机创建前（§2.3）。同一进程换 `-c` / 重载场景 → 重建图形时：

| 缓存 | 动作 |
| --- | --- |
| `_lastApplied` / `_cachedHostEye` / pending（`_lastSent` / `_frameSample` 随 `HostPosePublisher` 删除） | **全部清空** |
| 触发点 | `initGraphics` 成功重建场景后（**不必**整网 `SynchronSystem::shutdown`）；或显式 `SynchronSystem::resetEyeCaches()`（名称实现定）与图形重建同调用链调用 |

当前仅 `shutdown()` 会清缓存不够：热重载 / 测试里只重建图形时必须走上述触发点，否则旧类型缓存残留。

模式切换后第一帧：Host（viewhost）按当前累积眼点直接扇出；engine IG 侧经 `resetEyeCaches()` 清缓存。

### 4.4 防回声（比较 LookAt，不反解 YPR）——已随拆进程移除

> **（2026-08 拆进程）**：防回声依赖「Host 采样 mainCamera」，仅同进程 Host+IG（engine 权威窗）存在；engine 不再承担 Host 后整节删除。viewhost 眼点来自键盘累积、无回灌相机，无防回声需求。下列判据保留作历史参考，不再实现。

```text
若有 _lastApplied:
  expected = lookAtFromPose(_lastApplied)   // 与 apply 同一套 setCameraPose / setCameraPoseLla
  actual   = 当前 mainCamera 的 LookAt
  若 eye、forward、up 均在 ε 内一致:
    不更新 _frameSample → postFrame 重发 _lastSent
  否则:
    从 actual 解出 HostEyePose（本地 XYZ+YPR 或 LLA+当地YPR）写入 _frameSample
```

| 比较量 | 定义 | ε（第一版可调，测试钉死） |
| --- | --- | --- |
| `eye` | 位置（米） | 本地 ~`1e-4`；椭球 ECEF ~`1e-3`～`1e-2`（按实测选定） |
| `forward` | `normalize(center - eye)` | 方向，如 `1 - dot < 1e-8` 或角 ~`1e-4` rad |
| `up` | LookAt 的 `up`（可先与 forward 正交化再比） | 同上 |

### 4.5 Host / IG 模式一致性（运行时无错配）

同步层只支持 LLA（2026-09 收敛）：眼点恒为 `Detach`+LLA，无 `Attach`+XYZ 路径。

参与同步（有 `igConfig`）的场景在装配时**自动注入椭球**（§2.3），因此运行时不存在「场景无椭球却收 LLA」的错配路径——无需运行时拒收。原 `eyePoseRejectedByFrameMismatch` 计数器、`setEllipsoidMode` 场景模式注入与运行时 frame mismatch 拒收已随同步只 LLA 删除（2026-09）。

单机本地渲染（无 `igConfig`、场景无椭球）不参与同步，不产生模式一致性问题。

---

## 5. 报文（CIGI）

同步只 LLA（2026-09 收敛）：`IGCtrl` + `EntityPositionCtrl` **Detach** + `Lat/Lon/Alt` + YPR。

| 项 | 约定 |
| --- | --- |
| 同 datagram | 仍为 `IGCtrl` +（有眼点时）`EntityPositionCtrl` |
| 位置 | **Detach** + Lat / Lon / Alt |
| 姿态 | Yaw / Pitch / Roll = §3.2 当地 YPR（应用约定，见 §3.2） |
| 模式 | 恒 Detach + LLA（同步只 LLA，2026-09 收敛；无 Attach+XYZ 路径） |
| 乱序 | 仍按 `frameCntr` 整包取新 |

**分层（写死，同步只 LLA）**：

| 层 | 说明 |
| --- | --- |
| **线格式** | 恒走 CIGI `EntityPositionCtrl` **Detach** + Lat/Lon/Alt |
| **`CigiWire::EyePose`（解包后的内部 struct）** | 恒 LLA 语义（x=纬度、y=经度、z=海拔），无 frame 判别字段 |
| **`HostEyePose`** | 应用层权威眼点，恒 LLA（§4.2） |

线格式无私有 frame 字段；`AttachState` 恒为 Detach（CIGI 字段布局，非业务坐标系选择）。

精度与合法范围：

- Lat / Lon / Alt：走 CCL **double** 接口（`SetLat` / `SetLon` 等），避免经纬被窄化为 float。
- Yaw / Pitch / Roll：CCL 多为 **float**；接受量化误差，测试容差覆盖。
- **CCL 默认开 bound check**：`Lat∈[-90,90]`，`Lon∈[-180,180]`，`Pitch∈[-90,90]`（越界抛异常或返回错误）。组包前必须保证落在范围内：采样后 **normalize lon 到 (-180,180]**，非法 lat/pitch **丢弃本帧眼点**（IGCtrl 仍发）并打日志 / 计数（可与 wire 校验共用或单列 `eyePoseRejectedByRange`）；**不要**关全局 `bndchk` 把脏值发出去。

#### Detach / EntityID / ParentID（同步只 LLA）

这些字段属于 **CIGI 实体控制报文**（`EntityPositionCtrl`），**不是** VSG 场景图的「有没有父 `MatrixTransform` / 是否挂在 scene root」。同步层只支持 LLA（2026-09 收敛），眼点恒为 `Detach` + LLA。

| 概念 | 在本设计中的含义 | **不是** |
| --- | --- | --- |
| **线格式** | UDP 里真正发出的 CIGI 字节（`IGCtrl` + 可选 `EntityPositionCtrl`） | 配置里的 `injectEllipsoidIfMissing`；进程内 `HostEyePose` |
| **`AttachState`** | CIGI 自带开关，恒为 `Detach`（同步只 LLA） | 场景节点是否 Attach 到父 Transform |
| **`EntityID`** | CIGI **实体槽编号**（本项目眼点固定用 `0`） | VSG 节点指针 / scene 子节点下标 |
| **`ParentID`** | `Detach` 下恒为 `0`（无父实体） | scene root；VSG 父节点 |

**为何眼点固定 `EntityID=0`**：眼点占用固定实体槽；`AttachState` 恒 Detach，不换槽。

**组包表（设计期写死）**：

| 模式 | `AttachState` | `EntityID` | `ParentID` | 位置字段 |
| --- | --- | --- | --- | --- |
| LLA（唯一） | **Detach** | **0** | **0**（无父；Detach 下必须） | Lat / Lon / Alt |

`appendEye`（`cigi_wire::appendEye(CigiOutgoingMsg&, const EyePose*)`，业务侧组装眼点；IGCtrl 由 `outMsgWithIgCtrlUdp()` 自动前置，2026-08-25 起 `appendHostFrame` 已删）：恒设置 `Detach` + LLA + `ParentID=0`；`unpack` 校验 `ParentID==0`（不符则丢弃眼点）。

相机侧闭环仍是：**解包 → LLA → `setCameraPoseLla` 写 LookAt**；不把眼点做成「挂在场景父链上再乘 world Transform」的实体节点。

---

## 6. 配置

本文只覆盖 **LLA 传输语义相关的通道字段**（`injectEllipsoidIfMissing` 意图、椭球注入、初始相机默认、`offsetDeg`、同步地址等）；实体 / 相机 pose 的 JSON 结构与解析规则见 [位姿配置设计.md](./位姿配置设计.md)。

**本迭代 LLA 侧最小集**（`injectEllipsoidIfMissing` / 注入 / 初始相机 / 半径 / 部署约束的完整语义见 **§2**）：

- 可选顶层 `injectEllipsoidIfMissing`（bool，缺省 `false`；§2.2）。参与同步的 IG 无需配置——引擎自动注入。
- 椭球来源与注入：§2.3、§2.4。
- **不**在 HELLO 中增加注入意图字段（§2.2 / §2.5）。

模型自带椭球时一律保留（无 fail-fast，§2.3）；本地单机渲染不参与同步。各通道意图与模型椭球来源应一致（§2.4–§2.5）。

**投影 near/far（Ellipsoid 专有）**：`EllipsoidPerspective` **每帧动态计算** near/far，由 eye 海拔和视线角度决定（见 [位姿配置设计.md](./位姿配置设计.md) §4.4）。这高度自适应飞行仿真场景，但会带来深度精度损失和数值边界 corner case 的 trade-off。配置中不直接控制 near/far。

---

## 7. 验收要点

在现有 `HostIGTests` 风格上增加椭球分支（可用自挂 `EllipsoidModel` + 简单几何，**不强制**在线瓦片）：

| 用例方向 | 期望 |
| --- | --- |
| LLA 本机往返 | **已移除**（2026-08 拆进程：依赖 LookAt 采样，随 `HostPosePublisher` 删除；改由 `setCameraPoseLla` 写入 + LookAt 字段断言覆盖） |
| LLA Host→IG 跟拍 | **跨进程真报文**：viewhost 发布 LLA 眼点 → IG `LookAt.eye`（ECEF）与 Host 同椭球换算一致（Host 无 `offsetDeg`；邻通道另测 ⊕ yaw） |
| `offsetDeg` | 仅 `yaw` 有定义；Host 眼点可含 pitch/roll，左/右仅 yaw 偏移时仍满足 `R_ig=R_host·Rz(δ)`（各通道 up 轴平行）；`offsetDeg.pitch/roll≠0` → **不测试** |
| 线契约 | Detach+LLA 打包/解包；**Detach 时 ParentID=0、EntityID=0**（同步只 LLA，无 Attach 路径） |
| 组包依据 | 恒 Detach+LLA；线上无私有 frame 字段 |
| 模式装配 | 按 §2：无椭球才看「启 IG 同步 / `injectEllipsoidIfMissing`」注入；模型自带椭球则保留；注入在相机创建前；默认初始相机由 AABB 决定，见 [位姿配置设计.md](./位姿配置设计.md) §4（Ellipsoid fallback 到北京上空，Local fallback 到原点上空） |
| 范围校验 | Lat/Lon/Pitch 越界不抛穿；丢弃眼点并计数；lon 归一化到 (-180,180] |
| 权威 offset | **已移除**（2026-08 拆进程：Host 为 viewhost、无 `offsetDeg`；原「权威窗全 0」约束不再适用） |
| 缓存复位 | `initGraphics` 后眼点缓存清空（不依赖整网 shutdown） |
| 半径 | 注入为 WGS-84；自带模型不覆盖；装配后日志打印半径；BDD 覆盖 Host/IG 半径不一致（fail 或显式 skip，禁默默通过） |
| 防回声 / stale | 防回声**已移除**（2026-08 拆进程，§4.4）；stale 沿用既有 ReuseLast/Freeze 用例 |
| 场景重建清缓存 | 同进程重载场景：SynchronSystem 位姿缓存清空（`resetEyeCaches`；`_lastSent` 已随拆进程删除） |
| `_lastSent` 换轨 | **已移除**（2026-08 拆进程：`_lastSent` 随 `HostPosePublisher` 删除，无 Host 重发路径） |
| 极区 / 任意 Trackball | 不作为第一版必过（可标 skip 或放宽） |

---

## 8. 不做

| 项 | 原因 |
| --- | --- |
| 本迭代把普通 `.vsgt` 钉到指定 LLA 作为主交付 | 演示 ECEF 摆模可另开；不挡传眼点（见 §1 不保证项） |
| 用 `normalize(eye)` 作为正式 `up` | 非正式椭球法向 |
| 仅 init 写 LookAt、推迟写入口 / 采样 / 报文 | 无法完成帧同步 |
| 让注入开关与场景 `EllipsoidModel` 各判各的、互不驱动 | 只允许 §2「注入意图 → 可选注入 → 场景判据」 |
| HELLO 携带 / 协商椭球注入意图 | 第一版靠部署一致 + 场景装配兜底（§2.5） |
| 本迭代强制摆模 / 瓦片才能跑单测 | 单测与 BDD 可自挂椭球断言数值（§2.6） |
| 显示 Genlock、精时钟、Host 拆进程上行变更 | 仍属同步文档 P2 / 后期 |

[坐标系统模块设计.md](./坐标系统模块设计.md) 中装配示意（`MatrixTransform(LocalToWorld)`、当地天向初始化）可作实现附录参考；**不作为本传输设计的范围定义**。

---

## 9. 实现顺序

1. 按 §2 完成 `injectEllipsoidIfMissing` 解析、`loadScene` 后注入/保留 `EllipsoidModel`、默认 LLA 初始相机（均在相机创建前）
2. `setCameraPoseLla` + LLA 本机往返（LookAt↔LLA/YPR 互逆；单测，无网络；方向用 3×3）
3. `HostEyePose` 收敛为只 LLA（删 `frame` 枚举；`compose` / `apply` / `_lastApplied` 全部按 LLA 语义；本地路径随同步只 LLA 删除）
4. `CigiWire`：`EyePose` 收敛为只 LLA（删 `EyeFrame`；`appendEye` 恒 Detach+LLA）
5. 椭球分支：`setCameraPoseLla`；Detach+double LLA；EntityID/ParentID 按 §5；参与同步场景自动注入椭球
6. 场景重建清空同步位姿缓存（§4.3）
7. BDD：LLA Host→IG 跟拍、`offsetDeg`、模式装配与初始相机、半径、场景重建清缓存
8. （可选）瓦片联调；HELLO 传半径等增强

与 [多通道同步模块设计.md](./多通道同步模块设计.md) §4.8 / §9 P1「椭球 / ECEF 相机驱动」对齐；落地后更新该节状态为已实现，并收回「未实现前勿假设 XYZ 眼点在椭球下正确」的警告。
