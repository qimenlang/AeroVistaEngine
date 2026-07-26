#pragma once

#include "function/sync/SyncConfig.h"

#include <string>

struct OffsetDeg
{
    double yaw = 0.0;
    double pitch = 0.0;
    double roll = 0.0;
};

struct WindowConfig
{
    int x = 0;
    int y = 0;
    int width = 1920;
    int height = 1080;
};

enum class HostEyeStalePolicy
{
    ReuseLast,
    Freeze
};

/// Per-process Engine channel config (see engine/resources/config/*.json).
struct EngineChannelConfig
{
    int channelId = 0;
    OffsetDeg offsetDeg{};
    AddressConfig igLocal{};
    AddressConfig hostEndpoint{};
    AddressConfig hostLocal{};
    std::string model = "models/lz.vsgt";
    WindowConfig window{};
    HostEyeStalePolicy hostEyeStalePolicy = HostEyeStalePolicy::ReuseLast;

    bool enableHost() const { return channelId == 0; }
    bool enableIg() const { return true; }

    SyncRoleConfig toSyncRole() const;
};

bool loadEngineChannelConfig(const std::string& path, EngineChannelConfig& out, std::string* error = nullptr);
