# 外部库 SDK 调研

外部公开资料整理，不是法律意见，也不是设计契约。许可结论只复述公布文本，不代替律师判断。日期：2026-10-28。

三件事：MSFS 2024 的植被编辑器和 SDK 能不能接到自有视景；SpeedTree Runtime SDK 能不能接到自有渲染引擎、商用怎么收费、内部试用要不要钱、源码是否公开；CAE、AMST、MVRsimulation 这三家训练视景公开写过的植被做法。SpeedTree 在绘制上能做什么，见 [调研.md](./调研.md) §6。三家视景的材料在 §3，不进入 [调研.md](./调研.md) §13 的评分表。

## 1. MSFS 2024

问题：编辑器在植被上能改什么；这套 SDK 能不能直接接到自有飞行视景里用。

### 1.1 Impostor 是什么

Impostor（也写作 imposter）是远处物体的替身。近处用真正的三维网格；远到叶子已经只有几个像素时，改画一张从很多视角预先拍好的图，贴在很少的几个三角形上。看起来仍像那棵树，三角形数少一个数量级。

MSFS 2024 的物种文档写明，自动植被用的是八面体 impostor：把树从八面体（或半球）上的一组方向拍进一张方形图集，运行时按视线方向取样。物种 XML 里的 `frames` 是图集每边的帧数，`stiffness` 是这张替身抗风的程度，`textureIndex` 选图集里的哪一套。树干可以用颜色遮罩排除，避免被航拍染色染到树干上。

它不是一种可单独调用的库，也不是编辑器里的一个网格导出按钮。它是 MSFS 自己在画自动植被时用的远景表示。

### 1.2 编辑器在植被上能做什么

SDK 附带的是模拟器里的 DevMode，不是一台独立的植被建模器。做植被附加内容要经过三个窗口：Project Editor 建包，Biomes Editor 定规则，Scenery Editor 把规则用到一块地上。物种和生物群系的落盘格式是包内 `vegetation` 目录里的 XML。官方说明：目前只能用 XML，编辑器是在改这些规则，不是在雕刻树的枝干。

文档：

- 生物群系检查器：<https://docs.flightsimulator.com/msfs2024/retail/devmode/editors/biomes-editor/the-biomes-inspector/>
- 建一个生物群系：<https://docs.flightsimulator.com/msfs2024/html/2_DevMode/Biomes_Editor/Creating_A_Biome.htm>
- 地编多边形：<https://docs.flightsimulator.com/msfs2024/flighting/devmode/editors/scenery-editor/objects/polygon-objects/>
- 物种与生物群系 XML：<https://docs.flightsimulator.com/msfs2024/html/5_Content_Configuration/Environment/Vegetation/Biomes_And_Vegetation.htm>

#### 1.2.1 Biomes Editor

在 Project Editor 里建一个 Content-type 为 MISC、资产类型为 Biomes 的包，编译后在对应机场开自由飞行，再把该资产 Load In Editor。编辑器里新建 ruleset，在 Biomes Inspector 里加规则。没有树定义时模拟器不加载这个生物群系；不想要树就把每公顷株数设为 0。

检查器里和植被直接相关的项：

| 分组 | 能设的 |
| --- | --- |
| 树，全局 | 每公顷株数；人工地表（柏油、停机坪一类）上的缩放 |
| 树，每个物种 | 从模拟器自带物种表里选名字；按海拔（相对海平面，米）在最小、最大出现比例之间插值；均匀分布，或关掉均匀分布改成会聚团、会出空档的随机 |
| 草 | 从自带草表里选；限定地表类型（泥、岩、沙、雪、沥青、建筑混凝、高/低植被等）；是否用航拍颜色遮罩去染草的反照率；能否出现在沥青、城区、砖、碎石等人工地表 |
| 其他植物 | 从自带植物表里选，并限定地表类型 |
| 作物 | 从自带作物表里选，出现比例 0 到 1，数量仍受每公顷株数约束 |
| 生态区 | 这条规则用在哪些生态区，列表顺序是优先级 |
| 潜在自然植被 | 规则里的物种出问题时的后备植被类型，同样按列表优先级 |

一条规则还可以标成默认：某个生态区没有别的生物群系时用它。和已经挂载的其他包冲突时，检查器用绿、黄、灰标出是本包、别的包，还是尚未挂载。

给地编多边形用的生物群系不要填生态区和潜在自然植被。这两项是用来匹配世界上某一类地区的。

#### 1.2.2 Scenery Editor 的多边形

在地编里画多边形，打开植被遮罩覆盖后，这块多边形按所在区域的生物群系长满植被；也可以指定另一个生物群系，包括自制的。World Hub 机场不能改生物群系覆盖。

多边形上还能设：

- 排除：这块里面不长植被，其余植被选项随之关掉。
- 缩放、密度。密度高于当前画面设置里的 Trees 上限时，编辑器会警告，看到的不是所填密度。
- 衰减距离：多边形边缘向外一段，植被密度过渡到周围地形。场景里用虚线表示。

打开植被覆盖或排除时，多边形原来的材质不再显示，直到关掉该选项。

#### 1.2.3 编辑器改不到的

物种检查器里的名字来自模拟器自带表。自定义物种要另写物种 XML：变体、高度范围、impostor 的帧数和抗风、材质 GUID、航拍染色遮罩。编辑器不提供枝干级建模，也不把树导出成可以在别的引擎里画的网格加着色器。

编完的结果是一个 MSFS 包。模拟器启动后按自己的全球掩码、生态区和这些规则去种、去画。包不能被另一套渲染器当作植被运行时加载。

### 1.3 能不能直接用这套 SDK

不能接到自有飞行视景里用。SDK 的用途被许可写成一件事：开发 MSFS 2024 的附加内容。附加内容包括附加、修改和插件，不限于付费 DLC，免费场景也在这个用途里。它不是「只能做 DLC」，但也不是「可以拿去给别的模拟器用」。

依据是微软公布的 SDK EULA：<https://docs.flightsimulator.com/msfs2024/retail/introduction/sdk-eula/>。其中和本题直接相关的原意是：

- 软件是 Microsoft Flight Simulator Software Development Kit。目的（Purpose）是开发 MSFS 2024 Add-Ons。安装和使用仅限于该目的。
- 示例内容（模型、网格、仪表、声音等）只能在内部、为了这个目的使用。禁止对外分发、展示或表演这些示例内容。
- 你做出来的附加内容，在不触碰微软既有权利的前提下，权利仍归你。不得暗示微软为你或你的附加内容背书。
- 禁止把该软件及其任何组件用于开发竞争的飞行模拟器产品，也禁止用于竞争性基准、竞争分析、人工智能或机器学习、情报收集。
- 软件是许可不是出售。未经微软书面同意，不得反向工程、反编译或反汇编；不得分享、出版、分发或出借该软件，不得把它当成可供他人使用的独立托管方案，不得把软件或协议转给第三方。
- 违反许可时协议可终止，届时必须销毁软件及其组件的全部副本。

因此：

- 用 DevMode 做场景、生物群系、物种包，再在 MSFS 2024 里加载，这是许可写明的用途。
- 把 SDK、编辑器、示例树、示例网格或其中的组件链进、或改造成另一套飞行视景，和「仅用于开发 MSFS 2024 附加内容」以及竞争条款都对不上。
- 公开文档里的概念（八面体 impostor、每公顷株数、按生态区选物种）可以当作行业做法来了解。实现要走公开算法和自有数据，不能从 SDK 里取出实现、模型或示例内容放进自有引擎。八面体 impostor 的方法来源是 Ryan Brucks 2018 年的公开文章，不是微软的运行库。

若业务上确实需要把微软的软件或内容用在 MSFS 以外的产品里，EULA 写的出路是事先取得微软书面同意。上面只是对公布文本的阅读，不是法律意见。

## 2. SpeedTree SDK

问题：Runtime SDK 能不能编进自有渲染引擎并商用；公开标价有多少；只在内部用、先不发行，要不要付钱；这套 SDK 开不开源。

材料：

- Runtime SDK 10 手册：<https://docs.unity3d.com/speedtree-runtime-sdk/manual/index.html>
- 集成分层：<https://docs.unity3d.com/speedtree-runtime-sdk/manual/how-to-use-this-sdk.html>
- 商店档位：<https://store.speedtree.com/buy-try/>
- 附加条款，页首写的更新日期是 2026-09-10：<https://unity.com/legal/professional-artistry-additional-terms/>
- SDK 自带的第三方许可清单：<https://docs.unity3d.com/speedtree-runtime-sdk/license/third-party-licenses.html>

### 2.1 自有渲染引擎

可以接到自有渲染引擎。Runtime SDK 10 手册写明，这套文档给的就是把 SpeedTree 集成进自己引擎的人；获取方式是写信给 `sales@speedtree.com`。Unity、Unreal、Lumberyard 已经有现成集成，那些引擎看的是建模器文档，不是这份手册。

商店把产品和 SDK 拆开。Indie、Pro 的清单是建模器和游戏向、影视向导出，不含 Runtime SDK。Runtime SDK 列在 Enterprise 里。导出 FBX、OBJ 等到任意实时引擎或 DCC，商店说明写的是不需要按引擎另买一份建模器许可。风、运行时 LOD、公告牌若要 SpeedTree 自己的运行时来算，才把 Runtime SDK 编进引擎；只拿网格、自己写绘制，不经过这套 SDK。绘制能力分层见 [调研.md](./调研.md) §6。

公开附加条款管的是建模器（Speedtree Software）和网站上下载的资产。和接到实时视景直接相关的原意是：

- 飞行模拟、训练应用、交互式仿真这一类实时作品，必须买 Games 许可。Cinema 许可不能用于任何交互或实时作品。条款写明，用错档或把 Cinema 用进实时作品时，Unity 可以停用软件。
- 许可是有限的、非独占、不可转让、不可再许可。Unity 保留未明确授予的权利。作品（Licensed Production）是订单或 Offering Identification 里点名的那一部。订单还可以写使用期限、按平台、按作品的发行限制。
- 库资产和自己用建模器做出来的资产，可以嵌进这部点名作品里复制、公开展示和发行，包括实体宣传物料。资产必须和并非从 Unity 获得的实质性原创内容放在一起，并且只作为作品的嵌入部分，不得构成该作品的主要部分。2024 年 11 月 Unity 员工在论坛确认：仍不允许把资产作为独立产品再分发或转售。<https://discussions.unity.com/t/commercial-use-query/1554762>
- 除非订单另行写明并另付许可费，附加包、扩展包，以及为额外费用开发、且会用到 SpeedTree 软件、自建资产或库资产的同类内容，不在已购的 Licensed Production 里。
- 许可终止后，对自建资产和库资产的权利随之停止（订单另有写法的除外）。终止前尚未向公众发行的作品，不能再带着这些资产发行；也不能发到订单未列、且终止当日尚未在用的平台。
- 发行的每份作品要保留 Unity 提供的专有声明。制作人员名单和文档中要有：`Portions of this software utilize [产品名] ©[年份] Unity Technologies`。版权年份与 Unity 标在所交付代码上的年份相同，显示不得弱于自己的版权声明。
- 评估版：订阅期内只能内部评估和学习如何制作资产。评估版和学习版里的自建资产、库资产不能导出，也不能以任何方式发给第三方。

Runtime SDK 二进制和源码的再分发（静态链接还是动态库、能否交给另一家公司、能否修改后随产品发出）没有写在这份公开附加条款里。商店把它放进 Enterprise，手册让你联系销售。能不能编进引擎、能不能随模拟器发出去，以当时的订单为准。没有那份订单时，不能把 SDK 放进自有引擎的发行包。

### 2.2 商用费用

Runtime SDK 没有公开标价。商店把含 Runtime SDK 的 Enterprise 标成 Custom，向 `sales@speedtree.com` 询价。能看到的金额都是建模器，清单里不含 Runtime SDK：

| 档 | 近十二个月资金上限 | 公开价格 | 含 Runtime SDK |
| --- | --- | --- | --- |
| Indie | 10 万美元 | 19 美元/月 | 否 |
| Pro | 100 万美元 | 899 美元/年 | 否 |
| 树木库附加 | 100 万美元 | 999 美元/年 | 否 |
| Enterprise | 超过 100 万美元 | 单谈 | 是 |

Enterprise 清单同时含建模器、Runtime SDK、Games 与 Cinema 树木库选项、节点锁定或浮动许可，以及全部游戏向和影视向导出。浮动许可用 RLM，Enterprise 用户拿着采购单写信给 `licensing@speedtree.com` 申请。

旧的按作品、按平台的 Games 全许可同样不公布金额，只写联系销售。商用接到自有引擎上的费用，就是 Enterprise 的报价，公开页面给不出一个数字。

### 2.3 只在内部用

内部使用、暂不对外发行，不因此免许可费。附加条款里的档位按资金算，不按「这个项目还没卖」算。

近十二个月的 Total Finances：

- 法律实体自己用：整个实体的总收入和融资，不论来源。非营利组织按整个实体的预算算，不按使用部门算。
- 个人或法律实体给第三方提供服务：按客户的 Total Finances 算。
- 个人自己用、不给第三方提供服务：只算与使用 SpeedTree 相关的收入，不算与此无关的其他工作收入。

金额按美元折算。Indie 不超过 10 万美元，超过则只能用 Games Pro 或 Cinema Pro。Pro 不超过 100 万美元，超过则只能用 Games Enterprise 或 Cinema Enterprise。Enterprise 无上限。项目做到一半超档，条款写的是升级许可后可以继续。不同档不能同时用；超档要把全部订阅一起升级。使用即表示你保证自己符合该档，Unity 可以对照公开的收入估计来核查，并在不合规时停用。

免费能拿到的是建模器：下载后可开 30 天试用，导出受限。近十二个月资金超过 100 万美元的工作室或企业，完整评估要写信给 `sales@speedtree.com`，不是自己下载 Indie。评估版和学习版做出的资产不能导出，也不能发给第三方。Runtime SDK 10 手册没有免费下载入口。

### 2.4 是否开源

不开源。Runtime SDK 是专有的 C++ 中间件，手册要求向销售索取。

深度集成的客户会拿到整份 Runtime SDK 的源码。集成说明写的用途，是让接入方去掉或改掉一部分绘制功能；库代码放在 `SpeedTree` 命名空间里。这份源码随商业许可交付，没有公开仓库，也不以开源许可证发放。

SDK 另外捆绑了第三方代码，许可清单里包括半精度浮点例程、GLFW、ImGui，各自带 MIT 或类似的宽松许可。这些片段的许可不覆盖 SpeedTree 自己的 Core、Forest 和绘制库。

上面是对商店页、附加条款和 Runtime SDK 手册的阅读，不是报价，也不是法律意见。

## 3. 三家训练视景

问题：CAE、AMST（阿姆斯特）、MVRsimulation 的飞行视景，植被用的是什么方案。材料是产品说明和用户手册。三家都没有把植被写成 SpeedTree。能核对到的是能力清单、地物文件或贴图风，没有写到可以和 [调研.md](./调研.md) §2 逐条对照的放置规则和绘制结构，所以不进该文 §13 的评分表。

### 3.1 CAE Medallion-6000MR

军用视景机，产品说明覆盖固定翼和直升机。2025 年 3 月的 Medallion-6000MR 说明里，和植被直接有关的是：有动画的三维草和三维树；弹坑、武器效果和局部破坏可以在运行时作用到任意三维内容或植被上。同一份说明写支持 OGC CDB，并用 CDB 做运行时改地形。旋翼回流和下洗在同一页里写成粒子效果，没有写成树的风。

<https://www.cae.com/content/docs/defense-security/CAE_Medallion6000MR_March2025.pdf>

CDB 是开放地形库格式。这种库里的树通常是矢量点，再引用一套可复用的三维模型。CAE 的说明只写了支持这个格式，没有写 Medallion 是按土地覆盖撒点、用公告牌，还是给树冠采样航拍颜色。不能把「支持 CDB」写成他们的植被生成方案。

### 3.2 AMST VISIM

奥地利 AMST 的视景机，用于固定翼和直升机训练设备，页面写明满足 EASA CS-FSTD(A)、(H) 与 FAA 14 CFR Part 60 直到 Level D 的视景要求。他们的 A320neo 全动模拟机用 VISIM 通过了 EASA CS-FSTD(A) Issue 2 Level D。

<https://www.amst.co.at/visual-systems/visim-image-generator/>

<https://www.amst.co.at/news-visual-systems/visim-ig-level-d-qualification/>

页面写的数据库是：全球用高程和卫星影像；离所模拟机场越近，细节越高，以符合鉴定标准。建筑、电线、路、植被、河湖都属于这一圈高细节。物体很多时帧率保持住。冬夏各有一套场景。着色是 PBR。

植被只出现在「机场周围高细节区包含植被」这一句里。种在哪、近处网格还是远处替身、风怎么吹，页面都没有写。

另有一次离心机项目用 blackshark.ai 做某一地区的三维模型，再通过 SDK 放进 VISIM。公开报道说的是这块地景，没有把它说成植被生成。<https://afm.aero/provider-of-simulation-and-training-solutions-amst-uses-blackshark-ai-technology-to-advance-high-g-training>

### 3.3 MVRsimulation VRSG

训练视景。用户手册把树写成地形上的静态地物，和建筑、灯杆一类，不是每帧由主机驱动的实体。一个地区的地物在 `Culture` 目录的 `vrsg.clt` 里写模型和位置，视景启动时随该地形载入。大片树用实例绘制。

手册：<https://mvrsimulation.com/sites/default/files/PDF/MVRsimulation_VRSG.pdf>

Terrain Tools 1.6 的发布说明写了两件和飞行视景直接相关的事：树和其他植被模型按底下的正射影像自动配色；大面积实例树用 JSON 里的 LOD 概率权重，并支持 `LOD_Scale`。<https://www.mvrsimulation.com/sites/default/files/PressReleases/MVRsimulation%20release%20Terrain%20Tools%201.6%20-%20FINAL_0.pdf>

风写在植被贴图上，不写在场景节点上。`vrsg.irm` 里用 `-windSensitivity=` 给贴图一个灵敏度；模型底要放在原点，这条才起作用。风向通常来自外部系统，手册里用 W 键循环风向做试验。`@mvr:treeShader` 把 alpha 低于 50% 的像素收成全透明、高于的收成全不透明，用来去掉树的半透明晕边。`@mvr:grassShader` 在此之外再错位贴图的水平坐标，用来做出草或树叶在动。

公告牌是另一条路径。手册把「会转向的二维纹理，例如树，或火焰、爆炸」列为公告牌特效，和上面的三维植被模型分开。部分树模带四季外观和落叶损坏状态。红外材料表里给树叶、植被和树冠单独的材料号。全球按遥感掩码自动铺林，这份手册没有写。

### 3.4 和已有调研的关系

摆放、高度带和绘制写清楚、能和功能目标逐条对照的，仍是 [调研.md](./调研.md) 里的 MSFS 和 X-Plane 12。本节只记录这三家训练视景已经公开的说法。CAE 和 AMST 停在能力清单；VRSG 能核对到地物文件、实例、贴影像配色和贴图风。三家都不作为「先接一家的 SDK，再自研替换」的样板：公开材料里没有一份可链进自有引擎的植被运行时。
