#pragma once

#include <aerovista/sync/SyncConfig.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using aerovista::sync::HostConfig;
using aerovista::sync::IgConfig;
using aerovista::sync::SyncRoleConfig;

class TempConfigFile
{
public:
    explicit TempConfigFile(const std::string& jsonBody)
    {
        _path = (std::filesystem::temp_directory_path() /
                 ("ave_engine_cfg_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".json"))
                    .string();
        std::ofstream out(_path, std::ios::binary);
        out << jsonBody;
    }

    ~TempConfigFile()
    {
        std::error_code ec;
        std::filesystem::remove(_path, ec);
    }

    const std::string& path() const { return _path; }

private:
    std::string _path;
};

// =============================================================================
// 测试共用：Host/IG 网络角色辅助（E2E 用）。
// 各测试文件调用时传不同 base 隔离端口，避免并行冲突。
// 命名 makeTest*，避免与 HostIGTests 握手期专用（无 base）的 makeHostLocal 冲突。
// =============================================================================

inline HostConfig makeTestHostConfig(int base)
{
    return HostConfig{"127.0.0.1", base + 1, base, base + 100};
}

inline IgConfig makeTestIgConfig(int udpRecvPort, int base)
{
    return IgConfig{"127.0.0.1", base, udpRecvPort, "127.0.0.1", base + 100, base};
}

inline SyncRoleConfig makeTestHostIgRole(int igUdpRecv, int base)
{
    SyncRoleConfig role{};
    role.enableHost = true;
    role.enableIg = true;
    role.hostConfig = makeTestHostConfig(base);
    role.igConfig = makeTestIgConfig(igUdpRecv, base);
    return role;
}

inline SyncRoleConfig makeTestIgOnlyRole(int igUdpRecv, int base)
{
    SyncRoleConfig role{};
    role.enableHost = false;
    role.enableIg = true;
    role.igConfig = makeTestIgConfig(igUdpRecv, base);
    return role;
}

inline SyncRoleConfig makeTestHostOnlyRole(int base)
{
    SyncRoleConfig role{};
    role.enableHost = true;
    role.enableIg = false;
    role.hostConfig = makeTestHostConfig(base);
    return role;
}