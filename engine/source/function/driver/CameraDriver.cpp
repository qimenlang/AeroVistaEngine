#include "CameraDriver.h"

#include <vsg/maths/common.h>
#include <vsg/maths/quat.h>
#include <vsg/maths/vec3.h>

#include "CigiEntityPositionCtrlV4.h"

#include <cmath>

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

    /// setCameraPose 旋转 Rz(yaw)*Rx(pitch)*Ry(roll) 的逆，Y-forward Z-up。
    bool extractYprDegFromBasis(const vsg::dvec3& forward, const vsg::dvec3& up, vsg::dvec3& eulerYprDegOut)
    {
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
        eulerYprDegOut = vsg::dvec3(rad2deg(yawRad), rad2deg(pitchRad), rad2deg(rollRad));
        return true;
    }
} // namespace

void CameraDriver::onOwnshipEyePose(const CigiEntityPositionCtrlV4& pose)
{
    if (pose.GetEntityID() != 0)
        return;
    ChannelEye eye;
    eye.lla = {pose.GetLat(), pose.GetLon(), pose.GetAlt()};
    eye.eulerYprDeg = {pose.GetYaw(), pose.GetPitch(), pose.GetRoll()};
    compose(eye);
}

void CameraDriver::setOffsetDeg(const aerovista::sync::OffsetDeg& offset)
{
    _offsetDeg = offset;
}

void CameraDriver::resetEyeCaches()
{
    _lastApplied.reset();
}

ChannelEye CameraDriver::compose(const ChannelEye& host)
{
    // 刚性阵列通道偏移：R_ig = R_host · R_offset（Hamilton）。对纯 yaw 偏移，绕 Host 自身 up 轴
    // 旋转 Host 的 forward，每个通道 up 与 Host 保持平行——边缘对边缘 frustum 拼接在 Host roll
    // 下仍成立。分量式 YPR 相加得 Rz(δ)·R_host，roll≠0 时各通道 up 轴分开（roll 撕裂 bug；lla设计 §3.4）。
    const vsg::dquat qHost =
        vsg::dquat(vsg::radians(host.eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0)) *
        vsg::dquat(vsg::radians(host.eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) *
        vsg::dquat(vsg::radians(host.eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0));
    const vsg::dquat qOffset =
        vsg::dquat(vsg::radians(_offsetDeg.roll), vsg::dvec3(0.0, 1.0, 0.0)) *
        vsg::dquat(vsg::radians(_offsetDeg.pitch), vsg::dvec3(1.0, 0.0, 0.0)) *
        vsg::dquat(vsg::radians(_offsetDeg.yaw), vsg::dvec3(0.0, 0.0, 1.0));
    const vsg::dquat qIg = qOffset * qHost;

    const vsg::dvec3 forward = vsg::normalize(qIg * vsg::dvec3(0.0, 1.0, 0.0));
    const vsg::dvec3 up = vsg::normalize(qIg * vsg::dvec3(0.0, 0.0, 1.0));
    ChannelEye out = host;
    extractYprDegFromBasis(forward, up, out.eulerYprDeg);
    _lastApplied = out;
    return out;
}
