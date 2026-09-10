#include "CameraDriver.h"

#include "engine.h"

#include <vsg/maths/common.h>
#include <vsg/maths/quat.h>
#include <vsg/maths/vec3.h>

#include "CigiEntityPositionCtrlV4.h"

#include <cmath>

namespace
{
    // 眼点 offset 合成 helper（2026-09 随 compose 从 Engine 移入 CameraDriver）。
    constexpr double kPi = 3.14159265358979323846;

    vsg::dvec3 toVsg(const aerovista::sync::DVec3& v)
    {
        return vsg::dvec3(v.x, v.y, v.z);
    }
    aerovista::sync::DVec3 toSync(const vsg::dvec3& v)
    {
        return aerovista::sync::DVec3{v.x, v.y, v.z};
    }
    double rad2deg(double r)
    {
        return r * (180.0 / kPi);
    }
    double clampd(double v, double lo, double hi)
    {
        return v < lo ? lo : (v > hi ? hi : v);
    }

    /// setCameraPose 旋转 Rz(yaw)*Rx(pitch)*Ry(roll) 的逆，Y-forward Z-up。
    /// 从正交 forward/up 基恢复 YPR（度）。
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
} // namespace

CameraDriver::CameraDriver(Engine& engine, aerovista::sync::SynchronSystem& sync) :
    _engine(engine), _sync(sync)
{
}

void CameraDriver::onOwnshipEyePose(const CigiEntityPositionCtrlV4& pose)
{
    if (pose.GetEntityID() != 0)
        return;
    // ownship 眼点（§4.1）：翻译 CCL → HostEyePose（恒 LLA，2026-09 收敛），入队本帧眼点输入；
    // offset 合成 / stale / 断线决策在 update() 帧路径（applyComposed）。
    aerovista::sync::HostEyePose eye;
    eye.eulerYprDeg = {pose.GetYaw(), pose.GetPitch(), pose.GetRoll()};
    eye.position = {pose.GetLat(), pose.GetLon(), pose.GetAlt()};
    queueHostEyePose(eye);
}

void CameraDriver::queueHostEyePose(const aerovista::sync::HostEyePose& pose)
{
    _pendingHostEye = pose;
}

void CameraDriver::setOffsetDeg(const aerovista::sync::OffsetDeg& offset)
{
    _offsetDeg = offset;
}

void CameraDriver::setHostEyeStalePolicy(aerovista::sync::HostEyeStalePolicy policy)
{
    _stalePolicy = policy;
}

void CameraDriver::resetEyeCaches()
{
    _pendingHostEye.reset();
    _cachedHostEye.reset();
    _lastApplied.reset();
    _hasPendingApplied = false;
}

aerovista::sync::HostEyePose CameraDriver::compose(const aerovista::sync::HostEyePose& host,
                                                   const aerovista::sync::OffsetDeg& offset)
{
    // 刚性阵列通道偏移：R_ig = R_host · R_offset（Hamilton）。对纯 yaw 偏移，绕 Host 自身 up 轴
    // 旋转 Host 的 forward，每个通道 up 与 Host 保持平行——边缘对边缘 frustum 拼接在 Host roll
    // 下仍成立。分量式 YPR 相加得 Rz(δ)·R_host，roll≠0 时各通道 up 轴分开（roll 撕裂 bug；lla设计 §3.4）。
    // VSG operator*(a,b) = Hamilton(b⊗a)，写 qOffset * qHost 得 M(qHost)·M(qOffset) = R_host·R_offset。
    const vsg::dvec3 hostEuler = toVsg(host.eulerYprDeg);
    const vsg::dquat qHost =
        vsg::dquat(vsg::radians(hostEuler.z), vsg::dvec3(0.0, 1.0, 0.0)) *
        vsg::dquat(vsg::radians(hostEuler.y), vsg::dvec3(1.0, 0.0, 0.0)) *
        vsg::dquat(vsg::radians(hostEuler.x), vsg::dvec3(0.0, 0.0, 1.0));
    const vsg::dquat qOffset =
        vsg::dquat(vsg::radians(offset.roll), vsg::dvec3(0.0, 1.0, 0.0)) *
        vsg::dquat(vsg::radians(offset.pitch), vsg::dvec3(1.0, 0.0, 0.0)) *
        vsg::dquat(vsg::radians(offset.yaw), vsg::dvec3(0.0, 0.0, 1.0));
    const vsg::dquat qIg = qOffset * qHost;

    const vsg::dvec3 forward = vsg::normalize(qIg * vsg::dvec3(0.0, 1.0, 0.0));
    const vsg::dvec3 up = vsg::normalize(qIg * vsg::dvec3(0.0, 0.0, 1.0));
    // 在同一 Rz·Rx·Ry 约定下重新提取 YPR，使 applySyncCameraPose 的 setCameraPose 精确写入合成后的旋转。
    aerovista::sync::HostEyePose out = host;
    vsg::dvec3 eulerYprDeg;
    extractYprDegFromBasis(forward, up, eulerYprDeg);
    out.eulerYprDeg = toSync(eulerYprDeg);
    return out;
}

void CameraDriver::applyComposed(const aerovista::sync::HostEyePose& hostEye)
{
    _lastApplied = compose(hostEye, _offsetDeg);
    _hasPendingApplied = true;
}

void CameraDriver::applySyncCameraPose(const aerovista::sync::HostEyePose& pose)
{
    if (!_engine.hasGraphics())
        return;
    const vsg::dvec3 lla(pose.position.x, pose.position.y, pose.position.z);
    const vsg::dvec3 eulerYprDeg(pose.eulerYprDeg.x, pose.eulerYprDeg.y, pose.eulerYprDeg.z);
    _engine.setCameraPoseLla(lla, eulerYprDeg);
}

void CameraDriver::update()
{
    _hasPendingApplied = false;
    const bool linked = _sync.igLinked();

    // 未连接：本帧新输入无效（从未连接不得应用）；断线后保留最后一帧 Host 眼点。
    // 已连接：有新输入 → 直接应用；无新输入 → 走 stale 策略（REUSE_LAST 重用末帧；FREEZE 不合成）。
    if (!linked)
    {
        _pendingHostEye.reset();
        if (_cachedHostEye)
            applyComposed(*_cachedHostEye);
    }
    else if (_pendingHostEye)
    {
        _cachedHostEye = *_pendingHostEye;
        _pendingHostEye.reset();
        applyComposed(*_cachedHostEye);
    }
    else if (_cachedHostEye && _stalePolicy == aerovista::sync::HostEyeStalePolicy::REUSE_LAST)
    {
        applyComposed(*_cachedHostEye);
    }

    // 应用本帧合成位姿到相机（无 graphics 时仅更新 _lastApplied 供观测）。
    if (_hasPendingApplied)
    {
        _hasPendingApplied = false;
        applySyncCameraPose(*_lastApplied);
    }
}
