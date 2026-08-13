#pragma once

#include <aerovista/sync/SyncConfig.h>

#include <string>
#include <vector>

// 具体 using 声明（非 using namespace，符合 cpp-vsg-style.mdc）。
using aerovista::sync::HostConfig;
using aerovista::sync::HostEyeStalePolicy;
using aerovista::sync::IgConfig;
using aerovista::sync::OffsetDeg;
using aerovista::sync::parseHostConfig;
using aerovista::sync::parseIgConfig;
using aerovista::sync::SyncRoleConfig;

struct WindowConfig
{
    int x = 0;
    int y = 0;
    int width = 1920;
    int height = 1080;
};

/// JSON `coordFrame` intent: inject EllipsoidModel only when scene has none (lla设计 §2).
enum class CoordFrameIntent
{
    LOCAL,
    ELLIPSOID
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

/// One `entities[]` item (位姿配置设计.md).
struct EntityConfig
{
    int id = 0;
    std::string name;
    std::string model;
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

/// SynchronSystem 装配属性（sync模块化设计.md §4.2）。
/// IG 侧消费为主（offset/stale/requireIgConnect），channelId 两端标识。
struct SyncSystemConfig
{
    int channelId = 0;
    OffsetDeg offsetDeg{};
    HostEyeStalePolicy hostEyeStalePolicy = HostEyeStalePolicy::REUSE_LAST;
    bool requireIgConnect = false;
};

/// Per-process Engine channel config (see engine/resources/config/*.json, design §3.1).
struct EngineChannelConfig
{
    // syncSystem 组：channelId / offsetDeg / hostEyeStalePolicy / requireIgConnect。
    // 解析时若 JSON 有 `syncSystem` 组则用之；否则回退下面的旧扁平字段（平滑迁移，见 sync模块化设计.md §4.2）。
    SyncSystemConfig syncSystem{};

    // 旧扁平字段（兼容）：解析回退目标 + 旧访问点保持可用。
    int channelId = 0;
    OffsetDeg offsetDeg{};
    HostConfig hostConfig{};
    IgConfig igConfig{};
    std::string model = "models/lz.vsgt";
    WindowConfig window{};
    HostEyeStalePolicy hostEyeStalePolicy = HostEyeStalePolicy::REUSE_LAST;
    CoordFrameIntent coordFrame = CoordFrameIntent::LOCAL;
    bool requireIgConnect = false;

    /// Set when the corresponding JSON object key is present (parent-key enable).
    bool hasHostConfig = false;
    bool hasIgConfig = false;

    std::vector<EntityConfig> entities;
    bool hasCamera = false;
    CameraConfig camera{};

    bool enableHost() const { return hasHostConfig; }
    bool enableIg() const { return hasIgConfig; }

    SyncRoleConfig toSyncRole() const;
};

bool loadEngineChannelConfig(const std::string& path, EngineChannelConfig& out, std::string* error = nullptr);
