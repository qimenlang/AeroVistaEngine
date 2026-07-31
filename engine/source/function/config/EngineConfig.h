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
    REUSE_LAST,
    FREEZE
};

/// Per-process Engine channel config (see engine/resources/config/*.json, design §3.1).
struct EngineChannelConfig
{
    int channelId = 0;
    OffsetDeg offsetDeg{};
    AddressConfig igLocal{};
    AddressConfig hostEndpoint{};
    AddressConfig hostLocal{};
    std::string model = "models/lz.vsgt";
    WindowConfig window{};
    HostEyeStalePolicy hostEyeStalePolicy = HostEyeStalePolicy::REUSE_LAST;
    bool requireIgConnect = false;

    /// Set when the corresponding JSON object key is present (parent-key enable).
    bool hasHostLocal = false;
    bool hasIgLocal = false;

    bool enableHost() const { return hasHostLocal; }
    bool enableIg() const { return hasIgLocal; }

    SyncRoleConfig toSyncRole() const;
};

bool loadEngineChannelConfig(const std::string& path, EngineChannelConfig& out, std::string* error = nullptr);
