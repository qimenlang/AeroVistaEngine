# UE5 植被快速上手

学习笔记：在 UE 里尽快看到一片会动的林，并对照近景网格、远景收束和风。不是设计契约，不进入 AeroVistaEngine 的实现。对照目标见 [功能目标梳理.md](../../design/植被系统/功能目标梳理.md)，各家画法见 [调研.md](../../design/植被系统/调研.md) §7。日期：2026-10-29。

编辑器用 5.7 或 5.8。下面第 2 节的 Megascans 树可以在这两个版本里刷。第 4 节的 Megaplants / Procedural Vegetation Editor 是实验功能，5.7 做的图在 5.8 里打不开。

## 1. 先刷现成的树

第一天的目标是地上有树、近处能看出树冠、风在动。树形、PCG 规则、SpeedTree 建模都放在看见这三件事之后。

一小时路径用 **Megascans 树** 的静态网格，用 Foliage 笔刷种到地形上。风由该资源包里的 Global Foliage Actor 驱动。

**Megaplants** 是另一批资源，走 Procedural Vegetation Editor，输出 Nanite 骨骼植被。它的风不由 Global Foliage Actor 控制。两包不要混用控件。

## 2. 一小时路径

### 2.1 地形

新建项目时选 **Open World**，关卡里已经有地形。

空白项目：模式栏选 Landscape（Shift+2），Create 一块。官方步骤见 [Landscape Quick Start](https://dev.epicgames.com/documentation/unreal-engine/landscape-quick-start-guide-in-unreal-engine)。

### 2.2 素材

内容浏览器顶部打开 **Fab**，搜 Megascans 的树。Quixel 的入门视频用的是 **European Black Alder**（当时在 Marketplace 免费提供）。加入工程后，几何目录里有 forest 和 field 两套网格：field 是受光更充分的各年龄形态，forest 是林中争光、树冠偏上的形态。预设目录里有 **Global Foliage Actor**。

视频：<https://www.youtube.com/watch?v=-QK5gaeEAig>

### 2.3 笔刷

模式栏选 **Foliage**（Shift+3）。把树的静态网格拖进 Foliage 列表。类型用 **Static Mesh Foliage**：硬件实例化，很多棵可以一批画完。**Actor Foliage** 的开销和往场景里放普通 Actor 一样，密林不要用。

选 Paint，在地形上拖。按住 Shift 是擦。不要用 Fill 铺整张大地形。

官方说明：<https://dev.epicgames.com/documentation/en-us/unreal-engine/foliage-mode-in-unreal-engine>

### 2.4 这种树的参数

选中列表里的这种树，在细节里：

| 项 | 怎么设 |
| --- | --- |
| Align to Normal | 关掉。树保持竖直，不跟坡面躺倒 |
| Ground Slope | 限制陡坡不种 |
| Scale | 给一个最小和最大 |
| Density，以及笔刷上的 Paint Density | 一起决定疏密。Point Density 是类型上 Density 的倍数 |
| Radius | 两棵之间的最小间距 |
| Cull Distance | 树留得比草远 |

### 2.5 风

把该 Megascans 包里的 **Global Foliage Actor** 拖进关卡。细节里调：

- 风向：Actor 上的蓝色箭头是风来的方向，风袋会跟着参数动
- Wind Strength：从微风到强风
- Wind Speed：动画快慢
- Wind Noise / Wind Tunneling：噪声平滑还是碎、阵风尺度大还是碎

灯光一节里指定本关卡的定向光，着色器靠它算遮挡。不放这个 Actor，这些网格站着不动。

同一 Actor 上还有季节和健康度滑杆。本笔记不使用；季节不在当前植被目标里。

### 2.6 草

用同一种笔刷再拖一丛草的网格。草的 Cull Distance 设得很近，退开后地面只剩地表颜色。草关掉动态阴影，避免近处透明片把阴影算重。

草要跟着地形图层长出来时，再改用 Landscape Grass Type：右键菜单 Foliage → Landscape Grass Type，在地形材质里用 Grass 输出接到对应图层。第一天用笔刷就够。

## 3. 刷完看三个位置

| 站在哪 | 应看到 |
| --- | --- |
| 走到树下 | 树干，以及一片片叶卡 |
| 往后退 | 网格收成远景替身，中间是淡化 |
| 移到树的正上方 | 树冠仍是一团。竖着的公告牌在这个角度会变成一条边 |

这三眼对应巡航往下看、贴地看树冠、以及近远切换不要整片跳变。

## 4. 第二天再改树形

要改树的枝干和叶子分布时，用 **Procedural Vegetation Editor**（PVE）。它是实验插件，默认关闭。

1. Edit → Plugins，搜 Procedural Vegetation Editor（Experimental / Other），勾上并重启。
2. Fab 搜 **Megaplants**。UE 5.7 发布说明写明首批五种、可免费下载，带大小和结构变化。
3. 打开该资源包里自带的 PVE 图，改现成节点看树形。没有 SpeedTree 建模器时，不必从空白 Grower 长一棵。
4. 图的输出只在图的视口里看。放进关卡的是导出之后的资产。

PVE 建在 PCG 上，输出是 Nanite 植被：实例、GPU 骨骼风、远处体素。官方文档写明 5.7 的 PVE 资产不能在 5.8 打开。5.8 去掉了 Procedural Vegetation Preset Loader，预设改走 Growth Data Loader 和 Profile Loader。

文档：<https://dev.epicgames.com/documentation/unreal-engine/procedural-vegetation-editor-in-unreal-engine>

5.7 发布说明里关于 Megaplants 和 Nanite Foliage 的段落：<https://forums.unrealengine.com/t/unreal-engine-5-7-released/2673913>

## 5. 和本项目的关系

这片林只活在 UE 里，用来对效果。种在哪、排除区、高空融进航拍、贴地才出树冠和草，仍按 [功能目标梳理.md](../../design/植被系统/功能目标梳理.md)。UE 不提供全球林斑，也不把树染成所在航拍的颜色。
