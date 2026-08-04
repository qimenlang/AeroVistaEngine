#include "SynchronSystem.h"

#include "engine.h"

#include <cmath>
#include <iostream>

namespace
{
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kLookDistance = 1.0;

    double rad2deg(double r)
    {
        return r * (180.0 / kPi);
    }

    double clampd(double v, double lo, double hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    vsg::ref_ptr<vsg::EllipsoidModel> sceneEllipsoid(Engine& engine)
    {
        auto camera = engine.mainCamera();
        if (!camera || !camera->projectionMatrix)
            return {};
        auto ep = camera->projectionMatrix.cast<vsg::EllipsoidPerspective>();
        if (!ep)
            return {};
        return ep->ellipsoidModel;
    }

    bool sceneIsEllipsoid(Engine& engine)
    {
        // Prefer Engine flag (works for sync-only IG without Vulkan Device).
        return engine.sceneHasEllipsoidModel();
    }

    void enuBasisFromLocalToWorld(const vsg::dmat4& localToWorld, vsg::dvec3& east, vsg::dvec3& north, vsg::dvec3& upAxis)
    {
        east = vsg::normalize(vsg::dvec3(localToWorld(0, 0), localToWorld(0, 1), localToWorld(0, 2)));
        north = vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
        upAxis = vsg::normalize(vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
    }

    vsg::dvec3 rotateEnuToEcef(const vsg::dmat4& localToWorld, const vsg::dvec3& enuDir)
    {
        vsg::dvec3 east, north, upAxis;
        enuBasisFromLocalToWorld(localToWorld, east, north, upAxis);
        return enuDir.x * east + enuDir.y * north + enuDir.z * upAxis;
    }

    /// R = Rz(yaw)*Rx(pitch)*Ry(roll). Apply axis quats roll→pitch→yaw
    /// (VSG quat*quat is reverse-Hamilton).
    vsg::dvec3 rotateByEulerYprDeg(const vsg::dvec3& eulerYprDeg, const vsg::dvec3& v)
    {
        const vsg::dvec3 afterRoll =
            vsg::dquat(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0)) * v;
        const vsg::dvec3 afterPitch =
            vsg::dquat(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) * afterRoll;
        return vsg::dquat(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0)) * afterPitch;
    }

    /// Inverse of Engine::setCameraPose rotation Rz(yaw)*Rx(pitch)*Ry(roll), Y-forward Z-up.
    bool lookAtToWorldLocalEye(const vsg::LookAt& lookAt, HostEyePose& out)
    {
        out.frame = HostEyeCoordFrame::WORLD_LOCAL;
        out.position = lookAt.eye;
        const vsg::dvec3 forward = vsg::normalize(lookAt.center - lookAt.eye);
        if (vsg::length(forward) < 1e-12)
            return false;

        const double yawRad = std::atan2(-forward.x, forward.y);
        const double pitchRad = std::asin(clampd(forward.z, -1.0, 1.0));

        // yaw+pitch only: apply pitch then yaw (VSG reverse-Hamilton).
        const vsg::dvec3 afterPitchUp =
            vsg::dquat(pitchRad, vsg::dvec3(1.0, 0.0, 0.0)) * vsg::dvec3(0.0, 0.0, 1.0);
        const vsg::dvec3 afterPitchRight =
            vsg::dquat(pitchRad, vsg::dvec3(1.0, 0.0, 0.0)) * vsg::dvec3(1.0, 0.0, 0.0);
        const vsg::dvec3 expectedUp =
            vsg::normalize(vsg::dquat(yawRad, vsg::dvec3(0.0, 0.0, 1.0)) * afterPitchUp);
        const vsg::dvec3 expectedRight =
            vsg::normalize(vsg::dquat(yawRad, vsg::dvec3(0.0, 0.0, 1.0)) * afterPitchRight);
        const vsg::dvec3 up = vsg::normalize(lookAt.up);
        const double rollRad = std::atan2(vsg::dot(up, expectedRight), vsg::dot(up, expectedUp));

        out.eulerYprDeg = vsg::dvec3(rad2deg(yawRad), rad2deg(pitchRad), rad2deg(rollRad));
        return true;
    }

    bool lookAtToLlaEye(const vsg::LookAt& lookAt, const vsg::EllipsoidModel& ellipsoid, HostEyePose& out)
    {
        out.frame = HostEyeCoordFrame::LLA;
        out.position = ellipsoid.convertECEFToLatLongAltitude(lookAt.eye);
        const vsg::dvec3 forwardEcef = vsg::normalize(lookAt.center - lookAt.eye);
        if (vsg::length(forwardEcef) < 1e-12)
            return false;

        // Invert write path (§3.3): ENU basis = orthonormal columns of LocalToWorld.
        // Prefer column dots over worldToLocal*dir — keeps sample inverse of setCameraPoseLla.
        const vsg::dmat4 localToWorld = ellipsoid.computeLocalToWorldTransform(out.position);
        const vsg::dvec3 east = vsg::normalize(vsg::dvec3(localToWorld(0, 0), localToWorld(0, 1), localToWorld(0, 2)));
        const vsg::dvec3 north = vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
        const vsg::dvec3 upAxis = vsg::normalize(vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
        const auto toEnu = [&](const vsg::dvec3& ecefDir) {
            return vsg::normalize(vsg::dvec3(vsg::dot(ecefDir, east), vsg::dot(ecefDir, north), vsg::dot(ecefDir, upAxis)));
        };

        const vsg::dvec3 forward = toEnu(forwardEcef);
        const vsg::dvec3 up = toEnu(vsg::normalize(lookAt.up));

        const double yawRad = std::atan2(-forward.x, forward.y);
        const double pitchRad = std::asin(clampd(forward.z, -1.0, 1.0));
        const vsg::dvec3 afterPitchUp =
            vsg::dquat(pitchRad, vsg::dvec3(1.0, 0.0, 0.0)) * vsg::dvec3(0.0, 0.0, 1.0);
        const vsg::dvec3 afterPitchRight =
            vsg::dquat(pitchRad, vsg::dvec3(1.0, 0.0, 0.0)) * vsg::dvec3(1.0, 0.0, 0.0);
        const vsg::dvec3 expectedUp =
            vsg::normalize(vsg::dquat(yawRad, vsg::dvec3(0.0, 0.0, 1.0)) * afterPitchUp);
        const vsg::dvec3 expectedRight =
            vsg::normalize(vsg::dquat(yawRad, vsg::dvec3(0.0, 0.0, 1.0)) * afterPitchRight);
        const double rollRad = std::atan2(vsg::dot(up, expectedRight), vsg::dot(up, expectedUp));

        out.eulerYprDeg = vsg::dvec3(rad2deg(yawRad), rad2deg(pitchRad), rad2deg(rollRad));
        return true;
    }

    bool lookAtMatchesApplied(const vsg::LookAt& actual, const HostEyePose& applied, Engine& engine)
    {
        auto lookAt = vsg::LookAt::create();
        if (applied.frame == HostEyeCoordFrame::LLA)
        {
            auto em = sceneEllipsoid(engine);
            if (!em)
                return false;
            const vsg::dvec3 forwardEnu = rotateByEulerYprDeg(applied.eulerYprDeg, vsg::dvec3(0.0, 1.0, 0.0));
            const vsg::dvec3 upEnu = rotateByEulerYprDeg(applied.eulerYprDeg, vsg::dvec3(0.0, 0.0, 1.0));
            const vsg::dmat4 localToWorld = em->computeLocalToWorldTransform(applied.position);
            const vsg::dvec3 eye = em->convertLatLongAltitudeToECEF(applied.position);
            const vsg::dvec3 forward = vsg::normalize(rotateEnuToEcef(localToWorld, forwardEnu));
            const vsg::dvec3 up = vsg::normalize(rotateEnuToEcef(localToWorld, upEnu));
            lookAt->eye = eye;
            lookAt->center = eye + forward * kLookDistance;
            lookAt->up = up;
            constexpr double kEyeEps = 1e-2;
            constexpr double kDirEps = 1e-6;
            const vsg::dvec3 af = vsg::normalize(actual.center - actual.eye);
            const vsg::dvec3 ef = vsg::normalize(lookAt->center - lookAt->eye);
            return vsg::length(actual.eye - lookAt->eye) < kEyeEps &&
                   vsg::length(af - ef) < kDirEps &&
                   vsg::length(vsg::normalize(actual.up) - vsg::normalize(lookAt->up)) < kDirEps;
        }

        const vsg::dvec3 forward = rotateByEulerYprDeg(applied.eulerYprDeg, vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dvec3 up = rotateByEulerYprDeg(applied.eulerYprDeg, vsg::dvec3(0.0, 0.0, 1.0));
        constexpr double kEps = 1e-4;
        const vsg::dvec3 af = vsg::normalize(actual.center - actual.eye);
        return vsg::length(actual.eye - applied.position) < kEps &&
               vsg::length(af - vsg::normalize(forward)) < kEps &&
               vsg::length(vsg::normalize(actual.up) - vsg::normalize(up)) < kEps;
    }

    bool eyeFrameMatchesScene(const HostEyePose& eye, Engine& engine)
    {
        const bool wantLla = (eye.frame == HostEyeCoordFrame::LLA);
        return wantLla == sceneIsEllipsoid(engine);
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
    resetEyeCaches();
}

void SynchronSystem::resetEyeCaches()
{
    _hasPendingEye = false;
    _pendingEye = {};
    _cachedHostEye.reset();
    _lastApplied.reset();
    _lastSent.reset();
    _frameSample.reset();
    _frameMismatchErrorLogged = false;
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
        pose.frame = eye->isLla ? HostEyeCoordFrame::LLA : HostEyeCoordFrame::WORLD_LOCAL;
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
    if (auto em = sceneEllipsoid(engine))
    {
        if (!lookAtToLlaEye(*lookAt, *em, sample))
            return;
    }
    else
    {
        if (!lookAtToWorldLocalEye(*lookAt, sample))
            return;
    }

    // Anti-echo: compare LookAt to `_lastApplied` rebuild (lla §4.4); do not subtract offset.
    if (_lastApplied && lookAtMatchesApplied(*lookAt, *_lastApplied, engine))
    {
        _frameSample.reset();
        return;
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

bool SynchronSystem::tryAcceptPendingEye(Engine& engine)
{
    if (!_hasPendingEye)
        return false;

    if (!eyeFrameMatchesScene(_pendingEye, engine))
    {
        ++_eyePoseRejectedByFrameMismatch;
        if (!_frameMismatchErrorLogged)
        {
            const char* expected = sceneIsEllipsoid(engine) ? "Lla/Detach" : "WorldLocal/Attach";
            const char* got = (_pendingEye.frame == HostEyeCoordFrame::LLA) ? "Lla/Detach" : "WorldLocal/Attach";
            std::cerr << "[ERROR] eye pose rejected by frame mismatch: expected " << expected
                      << ", got " << got << " (channelId=" << engine.config.channelId << ")\n";
            _frameMismatchErrorLogged = true;
        }
        _hasPendingEye = false;
        return false;
    }

    _cachedHostEye = _pendingEye;
    _hasPendingEye = false;
    return true;
}

void SynchronSystem::applyHostEye(Engine& engine, const HostEyePose& hostEye)
{
    const HostEyePose composed = compose(hostEye, _offsetDeg);
    if (engine.hasGraphics())
    {
        if (composed.frame == HostEyeCoordFrame::LLA)
        {
            if (!engine.setCameraPoseLla(composed.position, composed.eulerYprDeg))
                return;
        }
        else
        {
            if (!engine.setCameraPose(composed.position, composed.eulerYprDeg))
                return;
        }
    }
    _lastApplied = composed;
}

void SynchronSystem::update(Engine& engine)
{
    const bool linked = igLinked();

    if (linked)
    {
        if (_hasPendingEye)
        {
            if (tryAcceptPendingEye(engine))
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

void SynchronSystem::postFrame(Engine& engine, double simTimeMs)
{
    if (!_host)
        return;

    const HostEyePose* sendEye = nullptr;
    HostEyePose eyeStorage{};
    const bool ellipsoid = sceneIsEllipsoid(engine);

    if (_frameSample)
    {
        eyeStorage = *_frameSample;
        _frameSample.reset();
        const bool sampleLla = (eyeStorage.frame == HostEyeCoordFrame::LLA);
        if (sampleLla == ellipsoid)
            sendEye = &eyeStorage;
        // else drop mismatched sample (should not happen if capture matches scene)
    }
    else if (_lastSent)
    {
        const bool sentLla = (_lastSent->frame == HostEyeCoordFrame::LLA);
        if (sentLla != ellipsoid)
        {
            // lla §4.3: type no longer matches scene — discard, do not fan out.
            _lastSent.reset();
        }
        else
        {
            eyeStorage = *_lastSent;
            sendEye = &eyeStorage;
        }
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
        wire.isLla = (sendEye->frame == HostEyeCoordFrame::LLA);
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

void SynchronSystem::seedLastSentHostEye(const HostEyePose& pose)
{
    _lastSent = pose;
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
