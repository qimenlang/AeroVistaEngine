#pragma once

#include <vsg/all.h>

#include <aerovista/sync/HostSync.h>
#include <aerovista/sync/SynchronSystem.h>

#include <optional>

/// Host 侧采样/扇出：LookAt → HostEyePose（WorldLocal / LLA）+ 防回声 + IGCtrl 扇出。
///
/// 与 Engine 解耦：依赖（椭球 / lastApplied / HostSync）全部显式传入，不持有 Engine 引用。
/// 内聚 Host 扇出状态（本帧采样 `_frameSample` + 最近发送 `_lastSent`）。
/// 见 doc/design/lla位姿传输设计.md §3.3 / §4.3 / §4.4、多通道同步模块设计.md。
class HostPosePublisher
{
public:
    /// handleEvents 后采样权威眼点：LookAt → HostEyePose（按椭球有无分 LLA / WorldLocal），
    /// 并对 `lastApplied` 做防回声比对（一致则丢弃本帧采样）。
    void captureAuthorityEye(const vsg::LookAt& lookAt,
                             const vsg::EllipsoidModel* ellipsoid,
                             const std::optional<aerovista::sync::HostEyePose>& lastApplied);

    /// 把本帧采样（或上次发送兜底）打包成 IGCtrl 扇出到 HostSync。
    /// IGCtrl（帧号/时间戳）由 HostSync::outMsgWithIgCtrlUdp() 自动填充（§7.1 自计时），本方法只追加眼点。
    void postHostFrame(aerovista::sync::HostSync& host, const vsg::EllipsoidModel* ellipsoid);

    /// 图形重建 / 模式切换后清空缓存（lla §4.3）。
    void reset();

    /// 本会话最近一次已发送的权威眼点（防回声 BDD / 模式切换丢弃用）。
    std::optional<aerovista::sync::HostEyePose> lastSent() const { return _lastSent; }
    /// 测试 / 注入：为模式切换的类型丢弃 ATTD 种子（lla §4.3 / §7）。
    void seedLastSent(const aerovista::sync::HostEyePose& pose) { _lastSent = pose; }

private:
    std::optional<aerovista::sync::HostEyePose> _lastSent;
    std::optional<aerovista::sync::HostEyePose> _frameSample;
};
