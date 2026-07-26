#pragma once

#include "HostSync.h"
#include "IgSync.h"
#include "SyncConfig.h"

#include <memory>
#include <vsg/all.h>

/// Engine-facing sync facade (loop preFrame / postFrame).
/// Owns IgSync and optionally HostSync. See doc/多通道同步模块设计.md.
class SynchronSystem : public vsg::Inherit<vsg::Object, SynchronSystem>
{
public:
    SynchronSystem();
    ~SynchronSystem() override;

    bool Initialize(const SyncRoleConfig& role);
    void Shutdown();

    void preFrame();
    void postFrame(double simTimeMs);

    bool hasHost() const { return static_cast<bool>(_host); }
    bool hasIg() const { return static_cast<bool>(_ig); }

    HostSync& hostSync();
    IgSync& igSync();

private:
    SyncRoleConfig _role{};
    std::unique_ptr<HostSync> _host;
    std::unique_ptr<IgSync> _ig;
};
