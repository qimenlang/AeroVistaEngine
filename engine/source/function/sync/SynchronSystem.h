#pragma once

#include "HostSync.h"
#include "IgSync.h"
#include "SyncConfig.h"
#include "function/config/EngineConfig.h"

#include <memory>
#include <optional>
#include <vsg/all.h>

class Engine;

/// World-space Host eye (pos + Euler YPR degrees). See doc/多通道同步模块设计.md §1.2.
struct HostEyePose
{
    vsg::dvec3 position{};
    vsg::dvec3 eulerYPR_deg{};
};

/// Engine-facing sync facade (loop preFrame / update / postFrame).
/// Owns IgSync and optionally HostSync. See doc/多通道同步模块设计.md.
class SynchronSystem : public vsg::Inherit<vsg::Object, SynchronSystem>
{
public:
    SynchronSystem();
    ~SynchronSystem() override;

    /// If requireIgConnect is false, IgSync is initialized locally even when connect fails.
    bool initialize(const SyncRoleConfig& role, bool requireIgConnect = true);
    void shutdown();

    void preFrame();
    /// Sample authority LookAt after handleEvents (Host engines only).
    void captureAuthorityEye(Engine& engine);
    /// Apply Host eye ⊕ offsetDeg to the engine camera when linked (or keep-last after disconnect).
    void update(Engine& engine);
    void postFrame(double simTimeMs);

    bool hasHost() const { return static_cast<bool>(_host); }
    bool hasIg() const { return static_cast<bool>(_ig); }

    HostSync& hostSync();
    IgSync& igSync();

    void setOffsetDeg(const OffsetDeg& offset);
    const OffsetDeg& offsetDeg() const { return _offsetDeg; }

    void setHostEyeStalePolicy(HostEyeStalePolicy policy);
    HostEyeStalePolicy hostEyeStalePolicy() const { return _stalePolicy; }

    /// Test / injection: enqueue a Host eye as if received this frame (with IGCtrl).
    void queueHostEyePose(const HostEyePose& pose);

    /// IG TCP+UDP both ready.
    bool igLinked() const;

    /// Last pose written by update (Host ⊕ offset), if any.
    std::optional<HostEyePose> lastAppliedHostEye() const { return _lastApplied; }

    /// Last authority eye Host packed for fan-out this session (for anti-echo BDD).
    std::optional<HostEyePose> lastSentHostEye() const { return _lastSent; }

private:
    void applyHostEye(Engine& engine, const HostEyePose& hostEye);
    static HostEyePose compose(const HostEyePose& host, const OffsetDeg& offset);

    SyncRoleConfig _role{};
    std::unique_ptr<HostSync> _host;
    std::unique_ptr<IgSync> _ig;

    OffsetDeg _offsetDeg{};
    HostEyeStalePolicy _stalePolicy = HostEyeStalePolicy::ReuseLast;

    bool _hasPendingEye = false;
    HostEyePose _pendingEye{};
    std::optional<HostEyePose> _cachedHostEye;
    std::optional<HostEyePose> _lastApplied;
    std::optional<HostEyePose> _lastSent;
    std::optional<HostEyePose> _frameSample;
};
