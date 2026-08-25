# CIGI 报文梳理（CIGI 4.0）

CIGI（Common Image Generator Interface，通用图像生成器接口）是 Host（仿真主机）与 IG（图像生成器）之间的标准报文协议。本项目 SDK 位于 `thirdparty/cigi`，工程使用 **CIGI 4.0**（V4）。本文档只收录 **V4 支持的报文**，报文名用 V4 报文类名。

方向约定：

- **Host → IG**：控制、定义、请求类报文；
- **IG → Host**：帧起始、响应、通知类报文；
- **Host ↔ IG**：双向通用报文。

链路与频率约定（判据写死，2026-08；与 [状态同步设计初版.md](../design/状态同步设计初版.md) §8.1 注册总纲一致）：

- **链路按发送频率选择**：**持续 / 每帧下发走 UDP**（丢包自愈、周期覆盖）；**一次性 / 配置 / 请求走 TCP**（传输层可靠送达，无业务回执）。
- **方向决定注册端点**：Host→IG 报文在 `IgSync` 注册收包 processor，IG→Host 报文在 `HostSync` 注册。

> 语义说明：CIGI 报文名后缀 `Ctrl` 为控制、`Def` 为定义（一次配置）、`Req` 为请求、`Resp` 为响应、`XResp` 为扩展响应、`SOF` 为帧起始。分组按功能归类，**不含具体字段内容**。

---

## 1. 基础帧控制类

| 报文 | 方向 | 链路 | 频率 | 功能语义 |
| --- | --- | --- | --- | --- |
| `CigiIGCtrlV4` | Host → IG | UDP | 每帧 | 会话级控制报文，每帧必发：携带帧计数、CIGI 版本、数据库 ID、IG 模式等，是所有 Host→IG 报文的起始报文 |
| `CigiSOFV4` | IG → Host | UDP | 每帧 | 帧起始报文，IG 每帧返回：携带帧计数、时间戳，标识一帧开始，是 IG→Host 报文的起始报文 |
| `CigiIGMsgV4` | Host ↔ IG | TCP | 事件性 | 通用自由文本消息通道，用于双方传递 ASCII 字符串 |
| `CigiEventNotificationV4` | IG → Host | TCP | 事件性 | 事件通知：IG 主动上报异步事件（如进入区域、命中指定目标等） |
| `CigiAnimationStopV4` | IG → Host | TCP | 事件性 | 动画播放结束通知：实体动画自然播放完毕后由 IG 上报 |

## 2. 实体与运动控制类

| 报文 | 方向 | 链路 | 频率 | 功能语义 |
| --- | --- | --- | --- | --- |
| `CigiEntityCtrlV4` | Host → IG | TCP | 一次性 | 实体控制：设置实体位姿、状态码、可见性等 |
| `CigiEntityPositionCtrlV4` | Host → IG | UDP（实时）/ TCP（一次性摆放） | 持续 | 位置+姿态+线速度一体控制（取代 V3 的 `RateCtrl`+`TrajectoryDef` 组合） |
| `CigiArtPartCtrlV4` | Host → IG | TCP | 一次性 | 部件控制：控制模型子部件（零件）的位姿/状态 |
| `CigiShortArtPartCtrlV4` | Host → IG | TCP | 一次性 | 部件控制短格式：通过 ID 映射压缩报文，省带宽 |
| `CigiCompCtrlV4` | Host → IG | TCP | 一次性 | 组件控制：控制组件（灯光、挂点、传感器挂载等）状态 |
| `CigiShortCompCtrlV4` | Host → IG | TCP | 一次性 | 组件控制短格式：ID 映射压缩版本 |
| `CigiConfClampEntityCtrlV4` | Host → IG | UDP | 持续 | 冲突/钳制实体控制：让实体在碰撞/地形约束下被钳制修正 |
| `CigiAnimationCtrlV4` | Host → IG | TCP | 一次性 | 实体动画播放/暂停/停止/选段控制 |
| `CigiVelocityCtrlV4` | Host → IG | UDP | 持续 | 线速度控制 |
| `CigiAccelerationCtrlV4` | Host → IG | UDP | 持续 | 加速度控制 |

## 3. 视景与传感器类

| 报文 | 方向 | 链路 | 频率 | 功能语义 |
| --- | --- | --- | --- | --- |
| `CigiViewDefV4` | Host → IG | TCP | 一次性 | 视图定义：将视图 ID 与通道/实体绑定（一次性配置） |
| `CigiViewCtrlV4` | Host → IG | UDP | 每帧 | 视图控制：每帧下发视点位置/朝向（主观察视角） |
| `CigiSensorCtrlV4` | Host → IG | TCP | 一次性 | 传感器控制：设置传感器视场角（FOV）等参数 |
| `CigiSensorRespV4` | IG → Host | TCP | 一次性响应 | 传感器响应：按请求回报传感器状态 |
| `CigiSensorXRespV4` | IG → Host | TCP | 一次性响应 | 传感器扩展响应：携带更多传感器数据 |
| `CigiMotionTrackCtrlV4` | Host → IG | TCP | 一次性 | 运动跟踪控制：定义 IG 需跟踪的实体/目标 |

## 4. 环境与气象类

| 报文 | 方向 | 链路 | 频率 | 功能语义 |
| --- | --- | --- | --- | --- |
| `CigiAtmosCtrlV4` | Host → IG | TCP | 一次性 | 大气控制：大气条件、能见度、温度等 |
| `CigiCelestialCtrlV4` | Host → IG | TCP | 一次性 | 天体控制：太阳/月亮位置、星历、星辰亮度等 |
| `CigiEnvRgnCtrlV4` | Host → IG | TCP | 一次性 | 环境区域控制：定义局部区域的环境参数 |
| `CigiWeatherCtrlV4` | Host → IG | TCP | 一次性 | 天气控制：云、雾、风、降水、雷暴等 |
| `CigiWeatherCondRespV4` | IG → Host | TCP | 一次性响应 | 天气条件响应：回报当前天气状态 |
| `CigiEnvCondReqV4` | Host → IG | TCP | 一次性请求 | 环境条件查询请求 |
| `CigiAerosolRespV4` | IG → Host | TCP | 一次性响应 | 气溶胶响应：回报大气气溶胶状态 |
| `CigiMaritimeSurfaceCtrlV4` | Host → IG | TCP | 一次性 | 海洋表面控制：海面参数（风向、洋流等） |
| `CigiMaritimeSurfaceRespV4` | IG → Host | TCP | 一次性响应 | 海洋表面响应 |
| `CigiTerrestrialSurfaceCtrlV4` | Host → IG | TCP | 一次性 | 陆地表面控制：地表参数（土壤湿度、植被等） |
| `CigiTerrestrialSurfaceRespV4` | IG → Host | TCP | 一次性响应 | 陆地表面响应 |
| `CigiWaveCtrlV4` | Host → IG | TCP | 一次性 | 波浪控制：海浪频谱、波高、方向等 |

## 5. 碰撞检测类

| 报文 | 方向 | 链路 | 频率 | 功能语义 |
| --- | --- | --- | --- | --- |
| `CigiCollDetSegDefV4` | Host → IG | TCP | 一次性 | 定义碰撞检测线段（起终点+命中条件） |
| `CigiCollDetSegRespV4` | IG → Host | TCP | 一次性响应 | 线段碰撞检测结果回报 |
| `CigiCollDetVolDefV4` | Host → IG | TCP | 一次性 | 定义碰撞检测体积（包围盒/球+命中条件） |
| `CigiCollDetVolRespV4` | IG → Host | TCP | 一次性响应 | 体积碰撞检测结果回报 |

## 6. 视线 / 寻的 Request–Response 类

| 报文 | 方向 | 链路 | 频率 | 功能语义 |
| --- | --- | --- | --- | --- |
| `CigiLosSegReqV4` | Host → IG | TCP | 一次性请求 | 视线线段请求：对一条线段做遮挡/命中查询 |
| `CigiLosRespV4` | IG → Host | TCP | 一次性响应 | 视线响应：命中/遮挡结果回报 |
| `CigiLosVectReqV4` | Host → IG | TCP | 一次性请求 | 视线矢量请求：对若干方向矢量做命中查询 |
| `CigiLosXRespV4` | IG → Host | TCP | 一次性响应 | 视线扩展响应：携带完整命中信息 |
| `CigiHatHotReqV4` | Host → IG | TCP | 一次性请求 | 方位/俯仰合一请求 |
| `CigiHatHotRespV4` | IG → Host | TCP | 一次性响应 | 方位/俯仰响应 |
| `CigiHatHotXRespV4` | IG → Host | TCP | 一次性响应 | 方位/俯仰扩展响应 |

## 7. 位置查询类（Request–Response）

| 报文 | 方向 | 链路 | 频率 | 功能语义 |
| --- | --- | --- | --- | --- |
| `CigiPositionReqV4` | Host → IG | TCP | 一次性请求 | 位置请求：查询实体当前世界位置 |
| `CigiPositionRespV4` | IG → Host | TCP | 一次性响应 | 位置响应：回报实体位置（及姿态） |

## 8. 地球模型类

| 报文 | 方向 | 链路 | 频率 | 功能语义 |
| --- | --- | --- | --- | --- |
| `CigiEarthModelDefV4` | Host → IG | TCP | 一次性 | 地球模型定义：椭球体参数、投影方式、坐标基准 |

## 9. 符号类

| 报文 | 方向 | 链路 | 频率 | 功能语义 |
| --- | --- | --- | --- | --- |
| `CigiSymbolCtrlV4` | Host → IG | TCP | 一次性 | 符号控制：显示/隐藏、平移旋转缩放、颜色等 |
| `CigiShortSymbolCtrlV4` | Host → IG | TCP | 一次性 | 符号控制短格式：ID 映射压缩版本 |
| `CigiSymbolSurfaceDefV4` | Host → IG | TCP | 一次性 | 面符号定义：屏幕空间面符号 |
| `CigiSymbolTextDefV4` | Host → IG | TCP | 一次性 | 文本符号定义：字符串符号 |
| `CigiSymbolCircleDefV4` | Host → IG | TCP | 一次性 | 圆符号定义 |
| `CigiSymbolPolygonDefV4` | Host → IG | TCP | 一次性 | 多边形符号定义 |
| `CigiSymbolTexturedCircleDefV4` | Host → IG | TCP | 一次性 | 纹理圆符号定义 |
| `CigiSymbolTexturedPolygonDefV4` | Host → IG | TCP | 一次性 | 纹理多边形符号定义 |
| `CigiSymbolCloneV4` | Host → IG | TCP | 一次性 | 符号克隆：复制已有符号 |

---

## V4 相对旧版本的替代关系

| V4 报文 | 替代/合并来源 |
| --- | --- |
| `CigiEntityPositionCtrlV4` | 取代 V3 的 `CigiRateCtrlV3` + `CigiTrajectoryDefV3`（V4 移除） |
| `CigiVelocityCtrlV4` / `CigiAccelerationCtrlV4` | V4 新增 |
| `CigiAnimationCtrlV4` | V4 新增 |
| `CigiAtmosCtrlV4` / `CigiCelestialCtrlV4` / `CigiEnvRgnCtrlV4` | 由 V1/V2 的 `CigiEnvCtrlV2` 拆分而来 |
| `CigiHatHotReqV4` / `CigiHatHotRespV4` | 合并 V1/V2 的 `CigiHatReqV1` / `CigiHotReqV2` |
| `CigiSensorXRespV4` / `CigiLosXRespV4` / `CigiHatHotXRespV4` | V3+ 新增扩展响应 |
| `CigiSymbolPolygonDefV4` | 取代 V3.3 的 `CigiSymbolLineDefV3_3` |

V4 **不再支持**的旧报文：`CigiRateCtrlV3`、`CigiTrajectoryDefV3`、`CigiEnvCtrlV2`、`CigiSpecEffDefV2`、`CigiHatReqV1`、`CigiHotReqV2`、`CigiHatRespV2`、`CigiHotRespV2`、`CigiSymbolLineDefV3_3`。

## 本项目实现状态（2026-08 全 9 类支持）

**收包侧已全支持**：按上表「链路」列，Host→IG 报文在 `IgSync` 的对应 session 注册通用捕获 processor，IG→Host 报文在 `HostSync` 的对应 session 注册；业务/测试经 `igSync().takeReceived<PacketT>()` / `hostSync().takeReceived<PacketT>()` 按类型取走（值拷贝，取走即清）。具体报文列表见 [状态同步设计初版.md](../design/状态同步设计初版.md) §8.1 注册总纲与 `EventProcess.h` 的 `PacketCaptureProc`。

| 方向端点 | UDP session（持续/每帧） | TCP session（一次性/配置/请求/响应） |
| --- | --- | --- |
| `IgSync`（IG 收 Host→IG） | IGCtrl、EntityPositionCtrl、ConfClampEntityCtrl、VelocityCtrl、AccelerationCtrl、ViewCtrl | EntityCtrl、ArtPart/Short、Comp/Short、AnimationCtrl、ViewDef、SensorCtrl、MotionTrackCtrl、AtmosCtrl、CelestialCtrl、EnvRgnCtrl、WeatherCtrl、Maritime/Terrestrial Surface Ctrl、WaveCtrl、EarthModelDef、CollDetSeg/VolDef、HatHotReq、LosSeg/VectReq、PositionReq、EnvCondReq、Symbol 全族（Ctrl/Short/Def/Clone） |
| `HostSync`（Host 收 IG→Host） | SOF | IGMsg、EventNotification、AnimationStop、HatHotResp/X、LosResp/X、SensorResp/X、PositionResp、WeatherCondResp、AerosolResp、Maritime/Terrestrial Surface Resp、CollDetSeg/VolResp |

## 本项目实际使用情况

| 报文 | 链路 | 频率 | 用途 |
| --- | --- | --- | --- |
| `CigiIGCtrlV4` | UDP | 每帧 | 数据面帧启动报文（`outMsgWithIgCtrlUdp()` 自动前置，`engine` / 测试） |
| `CigiEntityPositionCtrlV4` | UDP（数据面眼点）/ TCP（命令面摆放） | 每帧 / 一次性 | 数据面 ownship 眼点（EntityID=0）；命令面实体摆放（EntityID≠0，`place` 命令，§4.1 过滤） |
| `CigiSymbolTextDefV4` | TCP | 一次性 | **扩展复用**：作为通用文本命令载体（见 `doc/design/状态同步设计初版.md` §4.1） |
| `CigiCollDetVolDefV4` / `CigiCollDetVolRespV4` | TCP | 一次性 / 一次性响应 | 碰撞检测体积定义（Host→IG）与响应（IG→Host），基础设施 processor 已支持 |
| `CigiSOFV4` | UDP | 每帧 | IG 数据面每帧回显帧号（IG TCP 上报消息头也是 SOF，Host 双 session 注册） |

> 来源：`thirdparty/cigi`（Boeing CIGI SDK，V4 报文处理表 `CigiOutgoingMsg.cpp` 的 `SetOutgoingHostV4Tbls` / `SetOutgoingIGV4Tbls`；入站表 `CigiIncomingMsg.cpp` 的 `SetIncomingHostV4Tbls` / `SetIncomingIGV4Tbls`）。
