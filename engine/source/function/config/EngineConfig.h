#pragma once

#include "function/sync/SyncConfig.h"

#include <string>
#include <vector>

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

/// Per-process Engine channel config (see engine/resources/config/*.json, design §3.1).
struct EngineChannelConfig
{
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
