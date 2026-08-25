#pragma once

#include <aerovista/sync/SyncConfig.h>

#include <optional>
#include <string>
#include <vector>

// 具体 using 声明（非 using namespace，符合 cpp-vsg-style.mdc）。
// hostConfig 已于 2026-08 拆 Host 进程时移出 engine schema（Host 配置归 sync 库 loadHostConfig）。
using aerovista::sync::HostEyeStalePolicy;
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

/// JSON `coordFrame` 意图：场景无椭球时按意图注入 EllipsoidModel（lla设计 §2）。
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

/// 每进程 Engine 通道配置（见 engine/resources/config/*.json，设计 §3.1）。
struct EngineChannelConfig
{
    // syncSystem 组：channelId / offsetDeg / hostEyeStalePolicy / requireConnectedIg。
    SyncSystemConfig syncSystem{};

    IgConfig igConfig{};
    std::string model = "models/lz.vsgt";
    WindowConfig window{};
    CoordFrameIntent coordFrame = CoordFrameIntent::LOCAL;

    /// 对应 JSON 对象键出现时置位（父键 enable）。
    bool hasIgConfig = false;

    std::vector<EntityConfig> entities;
    bool hasCamera = false;
    CameraConfig camera{};

    bool enableIg() const { return hasIgConfig; }

    /// IG 传输配置：无 `igConfig`（未启同步）返回空。
    std::optional<IgConfig> toIgConfig() const;
};

bool loadEngineChannelConfig(const std::string& path, EngineChannelConfig& out, std::string* error = nullptr);
