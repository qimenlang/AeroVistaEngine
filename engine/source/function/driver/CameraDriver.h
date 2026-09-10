#pragma once

#include <aerovista/sync/SyncConfig.h>
#include <aerovista/sync/SynchronSystem.h>

#include <optional>

class CigiEntityPositionCtrlV4;
class Engine;

/// 眼点相机驱动器（2026-09 从 Engine 抽出）：用 Host 眼点（经 SynchronSystem 收包回调）经
/// offset 合成 / stale / 断线决策驱动 Engine 相机。区别于 Trackball 本地交互驱动。
///
/// 职责边界：只负责「眼点 → 相机」的业务策略。不接触收发（SynchronSystem）与渲染（Engine）。
/// Engine 持有本类，在 update/stepSync 每帧调 update()；眼点回调由 Engine::registerIgCallbacks
/// 转发到 onOwnshipEyePose（决策 A 方案 1：Engine 管注册，本类只提供处理函数）。
class CameraDriver
{
public:
    CameraDriver(Engine& engine, aerovista::sync::SynchronSystem& sync);

    /// 订阅回调入口（Engine 转发）：EntityPositionCtrlV4 ownship 眼点（EntityID==0，§4.1）——
    /// CCL → HostEyePose（恒 LLA），入队本帧眼点输入。非眼点报文（EntityID≠0）由卫语句过滤。
    void onOwnshipEyePose(const CigiEntityPositionCtrlV4& pose);
    /// 入队一个 Host 眼点（如同本帧随 IGCtrl 收到）。测试注入入口。
    void queueHostEyePose(const aerovista::sync::HostEyePose& pose);

    void setOffsetDeg(const aerovista::sync::OffsetDeg& offset);
    const aerovista::sync::OffsetDeg& offsetDeg() const { return _offsetDeg; }
    void setHostEyeStalePolicy(aerovista::sync::HostEyeStalePolicy policy);
    aerovista::sync::HostEyeStalePolicy hostEyeStalePolicy() const { return _stalePolicy; }

    /// update 最近写入的合成位姿（Host ⊕ 偏移），若有。
    std::optional<aerovista::sync::HostEyePose> lastAppliedHostEye() const { return _lastApplied; }

    /// 图形重建后清空眼点缓存（lla §4.3）。
    void resetEyeCaches();
    /// 帧级决策（断线/stale/新输入）+ 合成 + 应用相机。Engine::update/stepSync 每帧调。
    void update();

    /// 刚性阵列通道偏移合成：R_ig = R_host · R_offset（lla设计 §3.4）。纯函数。
    static aerovista::sync::HostEyePose compose(const aerovista::sync::HostEyePose& host,
                                                const aerovista::sync::OffsetDeg& offset);

private:
    void applyComposed(const aerovista::sync::HostEyePose& hostEye);
    /// 把合成位姿写入 Engine 相机（恒 LLA → setCameraPoseLla；无 graphics 时仅更新 _lastApplied 供观测）。
    void applySyncCameraPose(const aerovista::sync::HostEyePose& pose);

    Engine& _engine;
    aerovista::sync::SynchronSystem& _sync;

    std::optional<aerovista::sync::HostEyePose> _pendingHostEye;
    std::optional<aerovista::sync::HostEyePose> _cachedHostEye;
    std::optional<aerovista::sync::HostEyePose> _lastApplied;
    bool _hasPendingApplied = false;
    aerovista::sync::OffsetDeg _offsetDeg{};
    aerovista::sync::HostEyeStalePolicy _stalePolicy = aerovista::sync::HostEyeStalePolicy::REUSE_LAST;
};
