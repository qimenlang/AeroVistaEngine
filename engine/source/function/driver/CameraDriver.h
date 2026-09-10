#pragma once

#include <aerovista/sync/SyncConfig.h>

#include <vsg/maths/vec3.h>

#include <optional>

class CigiEntityPositionCtrlV4;

/// 通道眼点：LLA（纬度°、经度°、海拔 米）+ 当地 ENU YPR（度）。
/// compose 之后是本通道位姿（Host ⊕ offset），不是 Host 原始眼点。
struct ChannelEye
{
    vsg::dvec3 lla{};
    vsg::dvec3 eulerYprDeg{};
};

/// 眼点相机驱动器：CCL 翻译、offset 合成。不持有 Engine / 相机。
/// 收到 Host 眼点即与 `_offsetDeg` 合成，结果留在 `_lastApplied`；由 Engine 在
/// handleEvents 之后读出并 `setCameraPoseLla`。
class CameraDriver
{
public:
    /// 订阅回调入口（Engine 转发）：EntityID==0 ownship 眼点翻译为 ChannelEye 后合成。
    /// 非眼点报文（EntityID≠0）由卫语句过滤。
    void onOwnshipEyePose(const CigiEntityPositionCtrlV4& pose);

    void setOffsetDeg(const aerovista::sync::OffsetDeg& offset);
    const aerovista::sync::OffsetDeg& offsetDeg() const { return _offsetDeg; }

    /// 最近合成的通道位姿（Host ⊕ 偏移），若有。
    std::optional<ChannelEye> lastAppliedEye() const { return _lastApplied; }

    void resetEyeCaches();

    /// 刚性阵列通道偏移合成：R_ig = R_host · R_offset（lla设计 §3.4）。写入 `_lastApplied`。
    ChannelEye compose(const ChannelEye& host);

private:
    std::optional<ChannelEye> _lastApplied;
    aerovista::sync::OffsetDeg _offsetDeg{};
};
