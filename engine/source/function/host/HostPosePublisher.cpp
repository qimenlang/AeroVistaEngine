#include "HostPosePublisher.h"

#include <aerovista/sync/CigiWire.h>

#include <cmath>

using aerovista::sync::HostEyeCoordFrame;
using aerovista::sync::HostEyePose;
using aerovista::sync::HostSync;
namespace cigi_wire = aerovista::sync::cigi_wire;

namespace
{
    double rad2deg(double r)
    {
        return r * (180.0 / 3.14159265358979323846);
    }

    double clampd(double v, double lo, double hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /// Rz(yaw)*Rx(pitch)*Ry(roll) 的逆：从正交 forward/up 基恢复 YPR（度）。
    bool extractYprDegFromBasis(const vsg::dvec3& forward, const vsg::dvec3& up, vsg::dvec3& eulerYprDegOut)
    {
        const double yawRad = std::atan2(-forward.x, forward.y);
        const double pitchRad = std::asin(clampd(forward.z, -1.0, 1.0));

        // yaw+pitch 足够：先 pitch 后 yaw（VSG reverse-Hamilton）。
        const vsg::dvec3 afterPitchUp =
            vsg::dquat(pitchRad, vsg::dvec3(1.0, 0.0, 0.0)) * vsg::dvec3(0.0, 0.0, 1.0);
        const vsg::dvec3 afterPitchRight =
            vsg::dquat(pitchRad, vsg::dvec3(1.0, 0.0, 0.0)) * vsg::dvec3(1.0, 0.0, 0.0);
        const vsg::dvec3 expectedUp =
            vsg::normalize(vsg::dquat(yawRad, vsg::dvec3(0.0, 0.0, 1.0)) * afterPitchUp);
        const vsg::dvec3 expectedRight =
            vsg::normalize(vsg::dquat(yawRad, vsg::dvec3(0.0, 0.0, 1.0)) * afterPitchRight);
        const double rollRad = std::atan2(vsg::dot(up, expectedRight), vsg::dot(up, expectedUp));

        eulerYprDegOut = vsg::dvec3(rad2deg(yawRad), rad2deg(pitchRad), rad2deg(rollRad));
        return true;
    }

    /// R = Rz(yaw)*Rx(pitch)*Ry(roll)，按 roll→pitch→yaw 顺序作用轴四元数（VSG reverse-Hamilton）。
    vsg::dvec3 rotateByEulerYprDeg(const vsg::dvec3& eulerYprDeg, const vsg::dvec3& v)
    {
        const vsg::dvec3 afterRoll =
            vsg::dquat(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0)) * v;
        const vsg::dvec3 afterPitch =
            vsg::dquat(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) * afterRoll;
        return vsg::dquat(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0)) * afterPitch;
    }

    /// ENU → ECEF 方向（正交基点积）。
    vsg::dvec3 rotateEnuToEcef(const vsg::dvec3& east, const vsg::dvec3& north, const vsg::dvec3& up,
                               const vsg::dvec3& enuDir)
    {
        return enuDir.x * east + enuDir.y * north + enuDir.z * up;
    }

    /// LookAt → WorldLocal HostEyePose。
    bool lookAtToWorldLocalEye(const vsg::LookAt& lookAt, HostEyePose& out)
    {
        out.frame = HostEyeCoordFrame::WORLD_LOCAL;
        out.position = {lookAt.eye.x, lookAt.eye.y, lookAt.eye.z};
        const vsg::dvec3 forward = vsg::normalize(lookAt.center - lookAt.eye);
        if (vsg::length(forward) < 1e-12)
            return false;

        vsg::dvec3 eulerYprDeg;
        if (!extractYprDegFromBasis(forward, vsg::normalize(lookAt.up), eulerYprDeg))
            return false;
        out.eulerYprDeg = {eulerYprDeg.x, eulerYprDeg.y, eulerYprDeg.z};
        return true;
    }

    /// LookAt → LLA HostEyePose（逆写路径，lla设计 §3.3）。
    bool lookAtToLlaEye(const vsg::LookAt& lookAt, const vsg::EllipsoidModel& ellipsoid, HostEyePose& out)
    {
        out.frame = HostEyeCoordFrame::LLA;
        const vsg::dvec3 lla = ellipsoid.convertECEFToLatLongAltitude(lookAt.eye);
        out.position = {lla.x, lla.y, lla.z};
        const vsg::dvec3 forwardEcef = vsg::normalize(lookAt.center - lookAt.eye);
        if (vsg::length(forwardEcef) < 1e-12)
            return false;

        // 逆写路径（§3.3）：ENU 基 = LocalToWorld 的正交列。
        const vsg::dmat4 localToWorld = ellipsoid.computeLocalToWorldTransform(lla);
        const vsg::dvec3 east = vsg::normalize(vsg::dvec3(localToWorld(0, 0), localToWorld(0, 1), localToWorld(0, 2)));
        const vsg::dvec3 north = vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
        const vsg::dvec3 upAxis = vsg::normalize(vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
        const auto toEnu = [&](const vsg::dvec3& ecefDir) {
            return vsg::normalize(
                vsg::dvec3(vsg::dot(ecefDir, east), vsg::dot(ecefDir, north), vsg::dot(ecefDir, upAxis)));
        };
        const vsg::dvec3 forward = toEnu(forwardEcef);
        const vsg::dvec3 up = toEnu(vsg::normalize(lookAt.up));

        vsg::dvec3 eulerYprDeg;
        if (!extractYprDegFromBasis(forward, up, eulerYprDeg))
            return false;
        out.eulerYprDeg = {eulerYprDeg.x, eulerYprDeg.y, eulerYprDeg.z};
        return true;
    }

    /// 防回声：当前 LookAt 与 `applied` 重建结果一致（lla设计 §4.4）。
    bool lookAtMatchesApplied(const vsg::LookAt& actual, const HostEyePose& applied,
                              const vsg::EllipsoidModel* ellipsoid)
    {
        const vsg::dvec3 actualEye = actual.eye;
        const vsg::dvec3 actualForward = vsg::normalize(actual.center - actual.eye);
        const vsg::dvec3 actualUp = vsg::normalize(actual.up);

        if (applied.frame == HostEyeCoordFrame::LLA)
        {
            if (!ellipsoid)
                return false;
            const vsg::dvec3 euler(applied.eulerYprDeg.x, applied.eulerYprDeg.y, applied.eulerYprDeg.z);
            const vsg::dvec3 lla(applied.position.x, applied.position.y, applied.position.z);
            const vsg::dvec3 forwardEnu = rotateByEulerYprDeg(euler, vsg::dvec3(0.0, 1.0, 0.0));
            const vsg::dvec3 upEnu = rotateByEulerYprDeg(euler, vsg::dvec3(0.0, 0.0, 1.0));
            const vsg::dmat4 localToWorld = ellipsoid->computeLocalToWorldTransform(lla);
            const vsg::dvec3 east = vsg::normalize(vsg::dvec3(localToWorld(0, 0), localToWorld(0, 1), localToWorld(0, 2)));
            const vsg::dvec3 north = vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
            const vsg::dvec3 upAxis = vsg::normalize(vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
            const vsg::dvec3 eye = ellipsoid->convertLatLongAltitudeToECEF(lla);
            const vsg::dvec3 forward = vsg::normalize(rotateEnuToEcef(east, north, upAxis, forwardEnu));
            const vsg::dvec3 up = vsg::normalize(rotateEnuToEcef(east, north, upAxis, upEnu));

            constexpr double kEyeEps = 1e-2;
            constexpr double kDirEps = 1e-6;
            return vsg::length(actualEye - eye) < kEyeEps &&
                   vsg::length(actualForward - forward) < kDirEps &&
                   vsg::length(actualUp - up) < kDirEps;
        }

        const vsg::dvec3 euler(applied.eulerYprDeg.x, applied.eulerYprDeg.y, applied.eulerYprDeg.z);
        const vsg::dvec3 position(applied.position.x, applied.position.y, applied.position.z);
        const vsg::dvec3 forward = rotateByEulerYprDeg(euler, vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dvec3 up = rotateByEulerYprDeg(euler, vsg::dvec3(0.0, 0.0, 1.0));
        constexpr double kEps = 1e-4;
        return vsg::length(actualEye - position) < kEps &&
               vsg::length(actualForward - vsg::normalize(forward)) < kEps &&
               vsg::length(actualUp - vsg::normalize(up)) < kEps;
    }
} // namespace

void HostPosePublisher::captureAuthorityEye(const vsg::LookAt& lookAt,
                                            const vsg::EllipsoidModel* ellipsoid,
                                            const std::optional<HostEyePose>& lastApplied)
{
    HostEyePose sample{};
    if (ellipsoid)
    {
        if (!lookAtToLlaEye(lookAt, *ellipsoid, sample))
            return;
    }
    else
    {
        if (!lookAtToWorldLocalEye(lookAt, sample))
            return;
    }

    // 防回声：把 LookAt 与最近应用位姿重建比对（lla §4.4）；不减偏移。
    if (lastApplied && lookAtMatchesApplied(lookAt, *lastApplied, ellipsoid))
    {
        _frameSample.reset();
        return;
    }

    _frameSample = sample;
}

void HostPosePublisher::postHostFrame(HostSync& host, const vsg::EllipsoidModel* ellipsoid)
{
    const HostEyePose* sendEye = nullptr;
    HostEyePose eyeStorage{};
    const bool ellipsoidMode = static_cast<bool>(ellipsoid);

    if (_frameSample)
    {
        eyeStorage = *_frameSample;
        _frameSample.reset();
        const bool sampleLla = (eyeStorage.frame == HostEyeCoordFrame::LLA);
        if (sampleLla == ellipsoidMode)
            sendEye = &eyeStorage;
        // 否则丢弃不匹配的采样（若采样与场景匹配则不应发生）
    }
    else if (_lastSent)
    {
        const bool sentLla = (_lastSent->frame == HostEyeCoordFrame::LLA);
        if (sentLla != ellipsoidMode)
        {
            // lla §4.3：类型与场景不再匹配 —— 丢弃，不扇出。
            _lastSent.reset();
        }
        else
        {
            eyeStorage = *_lastSent;
            sendEye = &eyeStorage;
        }
    }

    cigi_wire::EyePose wire{};
    const cigi_wire::EyePose* wirePtr = nullptr;
    if (sendEye)
    {
        wire.x = sendEye->position.x;
        wire.y = sendEye->position.y;
        wire.z = sendEye->position.z;
        wire.yawDeg = sendEye->eulerYprDeg.x;
        wire.pitchDeg = sendEye->eulerYprDeg.y;
        wire.rollDeg = sendEye->eulerYprDeg.z;
        wire.frame = (sendEye->frame == HostEyeCoordFrame::LLA) ? cigi_wire::EyeFrame::LLA
                                                                : cigi_wire::EyeFrame::WORLD_LOCAL;
        wirePtr = &wire;
        _lastSent = *sendEye;
    }

    // IGCtrl 由 outMsgWithIgCtrlUdp() 自动前置（帧号/自计时时间戳，§7.1）；此处只追加眼点。
    auto& omsg = host.outMsgWithIgCtrlUdp();
    cigi_wire::appendEye(omsg, wirePtr);
    host.flushUdp();
}

void HostPosePublisher::reset()
{
    _lastSent.reset();
    _frameSample.reset();
}
