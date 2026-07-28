#include "SynchronSystem.h"

#include "engine.h"

#include <cmath>
#include <iostream>

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    double rad2deg(double r)
    {
        return r * (180.0 / kPi);
    }

    double clampd(double v, double lo, double hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /// Inverse of Engine::setCameraPose rotation Rz(yaw)*Rx(pitch)*Ry(roll), Y-forward Z-up.
    bool lookAtToHostEye(const vsg::LookAt& lookAt, HostEyePose& out)
    {
        out.position = lookAt.eye;
        const vsg::dvec3 forward = vsg::normalize(lookAt.center - lookAt.eye);
        if (vsg::length(forward) < 1e-12)
            return false;

        const double yawRad = std::atan2(-forward.x, forward.y);
        const double pitchRad = std::asin(clampd(forward.z, -1.0, 1.0));

        const vsg::dquat qYawPitch =
            vsg::dquat(yawRad, vsg::dvec3(0.0, 0.0, 1.0)) *
            vsg::dquat(pitchRad, vsg::dvec3(1.0, 0.0, 0.0));
        const vsg::dvec3 expectedUp = vsg::normalize(qYawPitch * vsg::dvec3(0.0, 0.0, 1.0));
        const vsg::dvec3 expectedRight = vsg::normalize(qYawPitch * vsg::dvec3(1.0, 0.0, 0.0));
        const vsg::dvec3 up = vsg::normalize(lookAt.up);
        const double rollRad = std::atan2(vsg::dot(up, expectedRight), vsg::dot(up, expectedUp));

        out.eulerYprDeg = vsg::dvec3(rad2deg(yawRad), rad2deg(pitchRad), rad2deg(rollRad));
        return true;
    }
} // namespace

SynchronSystem::SynchronSystem() = default;

SynchronSystem::~SynchronSystem()
{
    shutdown();
}

bool SynchronSystem::initialize(const SyncRoleConfig& role, bool requireIgConnect)
{
    shutdown();
    _role = role;

    if (role.enableHost)
    {
        _host = std::make_unique<HostSync>();
        if (!_host->initialize(role.hostLocal))
        {
            std::cerr << "SynchronSystem: HostSync initialize failed\n";
            shutdown();
            return false;
        }
        _host->setPaceConfig(SyncPaceConfig{});
        _host->run();
    }

    if (role.enableIg)
    {
        _ig = std::make_unique<IgSync>();
        if (!_ig->initialize(role.igLocal))
        {
            std::cerr << "SynchronSystem: IgSync initialize failed\n";
            shutdown();
            return false;
        }

        if (!_ig->connect(role.hostEndpoint))
        {
            if (requireIgConnect)
            {
                std::cerr << "SynchronSystem: IgSync connect failed\n";
                shutdown();
                return false;
            }
        }
    }

    return true;
}

void SynchronSystem::shutdown()
{
    if (_ig)
    {
        _ig->shutdown();
        _ig.reset();
    }
    if (_host)
    {
        _host->shutdown();
        _host.reset();
    }
    _role = {};
    _hasPendingEye = false;
    _pendingEye = {};
    _cachedHostEye.reset();
    _lastApplied.reset();
    _lastSent.reset();
    _frameSample.reset();
}

void SynchronSystem::preFrame()
{
    if (!_ig)
        return;

    _ig->update(/*sendSof=*/true);
    if (auto eye = _ig->takeReceivedHostEye())
    {
        HostEyePose pose;
        pose.position = vsg::dvec3(eye->x, eye->y, eye->z);
        pose.eulerYprDeg = vsg::dvec3(eye->yawDeg, eye->pitchDeg, eye->rollDeg);
        queueHostEyePose(pose);
    }
}

void SynchronSystem::captureAuthorityEye(Engine& engine)
{
    if (!_host)
        return;

    auto camera = engine.mainCamera();
    if (!camera)
        return;

    auto lookAt = camera->viewMatrix.cast<vsg::LookAt>();
    if (!lookAt)
        return;

    HostEyePose sample{};
    if (!lookAtToHostEye(*lookAt, sample))
        return;

    // If LookAt still equals what we last applied (Host⊕offset), the user has not
    // moved since overwrite — do not treat that as new intent (would echo Pose_old).
    // postFrame will resend _lastSent instead.
    if (_lastApplied)
    {
        constexpr double kEps = 1e-4;
        if (vsg::length(sample.position - _lastApplied->position) < kEps &&
            vsg::length(sample.eulerYprDeg - _lastApplied->eulerYprDeg) < kEps)
        {
            _frameSample.reset();
            return;
        }
    }

    _frameSample = sample;
}

HostEyePose SynchronSystem::compose(const HostEyePose& host, const OffsetDeg& offset)
{
    HostEyePose out = host;
    out.eulerYprDeg.x += offset.yaw;
    out.eulerYprDeg.y += offset.pitch;
    out.eulerYprDeg.z += offset.roll;
    return out;
}

void SynchronSystem::applyHostEye(Engine& engine, const HostEyePose& hostEye)
{
    const HostEyePose composed = compose(hostEye, _offsetDeg);
    if (engine.hasGraphics())
        engine.setCameraPose(composed.position, composed.eulerYprDeg);
    _lastApplied = composed;
}

void SynchronSystem::update(Engine& engine)
{
    const bool linked = igLinked();

    if (linked)
    {
        if (_hasPendingEye)
        {
            _cachedHostEye = _pendingEye;
            _hasPendingEye = false;
            applyHostEye(engine, *_cachedHostEye);
        }
        else if (_cachedHostEye)
        {
            if (_stalePolicy == HostEyeStalePolicy::REUSE_LAST)
                applyHostEye(engine, *_cachedHostEye);
            // Freeze: leave camera as-is
        }
        return;
    }

    // Not linked: discard any injected pending eye (never-connected must not apply).
    _hasPendingEye = false;

    // After disconnect (or if we still hold a cache from a prior link), keep last Host eye.
    if (_cachedHostEye)
        applyHostEye(engine, *_cachedHostEye);
}

void SynchronSystem::postFrame(double simTimeMs)
{
    if (!_host)
        return;

    const HostEyePose* sendEye = nullptr;
    HostEyePose eyeStorage{};

    if (_frameSample)
    {
        eyeStorage = *_frameSample;
        sendEye = &eyeStorage;
        _frameSample.reset();
    }
    else if (_lastSent)
    {
        eyeStorage = *_lastSent;
        sendEye = &eyeStorage;
    }

    HostSync::EyePose wire{};
    const HostSync::EyePose* wirePtr = nullptr;
    if (sendEye)
    {
        wire.x = sendEye->position.x;
        wire.y = sendEye->position.y;
        wire.z = sendEye->position.z;
        wire.yawDeg = sendEye->eulerYprDeg.x;
        wire.pitchDeg = sendEye->eulerYprDeg.y;
        wire.rollDeg = sendEye->eulerYprDeg.z;
        wirePtr = &wire;
        _lastSent = *sendEye;
    }

    _host->update(simTimeMs, wirePtr);
}

HostSync& SynchronSystem::hostSync()
{
    return *_host;
}

IgSync& SynchronSystem::igSync()
{
    return *_ig;
}

void SynchronSystem::setOffsetDeg(const OffsetDeg& offset)
{
    _offsetDeg = offset;
}

void SynchronSystem::setHostEyeStalePolicy(HostEyeStalePolicy policy)
{
    _stalePolicy = policy;
}

void SynchronSystem::queueHostEyePose(const HostEyePose& pose)
{
    _pendingEye = pose;
    _hasPendingEye = true;
}

bool SynchronSystem::igLinked() const
{
    return _ig && _ig->tcpConnected() && _ig->udpSynced();
}
