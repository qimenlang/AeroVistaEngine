#pragma once

#include <aerovista/sync/SyncConfig.h>

#include <optional>
#include <string>
#include <vector>

// 具体 using 声明（非 using namespace，符合 cpp-vsg-style.mdc）。
// engine schema 不含 hostConfig；Host 进程用 sync 库 loadHostConfig。
using aerovista::sync::IgConfig;
using aerovista::sync::OffsetDeg;
using aerovista::sync::parseIgConfig;
using aerovista::sync::SyncSystemConfig;

struct WindowConfig
{
    int x = 0;
    int y = 0;
    int width = 1920;
    int height = 1080;
};

struct Vec3Config
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

struct LocalPoseConfig
{
    Vec3Config position{};
    Vec3Config eulerYprDeg{};
};

struct EllipsoidPoseConfig
{
    Vec3Config lla{}; // lat°, lon°, alt m
    Vec3Config eulerYprDeg{};
};

/// 实体初始显隐状态（entities.json `initialEntityState`，字面值契约见 实体与运动控制设计.md §5）。
/// 仅 Active/Standby 两值（缺省 Active）；Destroyed/Remove 及非法值在解析期 fail-fast。
enum class EntityInitialState
{
    STANDBY = 0,
    ACTIVE = 1
};

/// One `entities[]` item (位姿配置设计.md)。
struct EntityConfig
{
    int id = 0;
    std::string name;
    std::string model;
    EntityInitialState initialEntityState = EntityInitialState::ACTIVE;
    bool hasPose = false;
    bool hasPoseLocal = false;
    bool hasPoseEllipsoid = false;
    LocalPoseConfig localPose{};
    EllipsoidPoseConfig ellipsoidPose{};
};

struct CameraConfig
{
    bool hasPose = false;
    bool hasPoseLocal = false;
    bool hasPoseEllipsoid = false;
    LocalPoseConfig localPose{};
    EllipsoidPoseConfig ellipsoidPose{};
};

/// 每进程 Engine 通道配置（见 engine/resources/config/*.json，多通道同步模块设计.md §3.1）。
struct EngineChannelConfig
{
    // syncSystem 组：channelId / offsetDeg / requireConnectedIg。
    SyncSystemConfig syncSystem{};

    /// IG 传输配置；nullopt = 未启用同步（无 igConfig）。optional 承载「有/无」语义。
    std::optional<IgConfig> igConfig{};

    /// 相机初始位姿；nullopt = 无 camera 键（默认相机走场景 AABB，位姿配置设计.md §4）。
    std::optional<CameraConfig> camera{};

    std::string model = "models/lz.vsgt";
    WindowConfig window{};
    /// 场景模型无自带 EllipsoidModel 时，是否注入一个 WGS-84 椭球（lla设计 §2）。
    /// 仅对「单机椭球渲染」（无 igConfig）生效；启用 IG 同步（有 igConfig）时引擎自动注入，无需此开关。
    /// 运行时坐标系由「场景有无 EllipsoidModel」决定，与此开关解耦。
    bool injectEllipsoidIfMissing = false;

    /// 实体目录文件路径（entities.json）；空 = 不装实体（单模型）。
    /// engine 通道配置不再内嵌 entities；配置指向独立文件（实体与运动控制设计.md §5/§13）。
    std::string entitiesFilePath;
};

bool loadEngineChannelConfig(const std::string& path, EngineChannelConfig& out, std::string* error = nullptr);

/// 从独立实体目录文件 `entities.json` 解析 EntityConfig 列表（顶层 `{ "entities": [...] }`）。
/// 契约：实体与运动控制设计.md §5（id 1..65535 唯一 / model 必填 / name 缺省 basename /
/// initialEntityState 仅 "Active"|"Standby" / pose 双轨可选 / 空表与未知键 fail-fast）。
bool loadEntitiesFile(const std::string& path, std::vector<EntityConfig>& out, std::string* error = nullptr);
