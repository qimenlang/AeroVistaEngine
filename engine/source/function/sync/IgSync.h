#pragma once

#include "Network.h"
#include "SyncConfig.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#ifdef WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <winsock2.h>
using IgSocketHandle = SOCKET;
#else
using IgSocketHandle = int;
#endif

/// IG-side sync endpoint: connects to Host, UDP sync + TCP command client.
class IgSync
{
public:
    IgSync() = default;
    ~IgSync();

    IgSync(const IgSync&) = delete;
    IgSync& operator=(const IgSync&) = delete;

    struct HostEye
    {
        double x = 0, y = 0, z = 0;
        double yawDeg = 0, pitchDeg = 0, rollDeg = 0;
        /// From wire AttachState (lla设计 §5); true = Detach+LLA.
        bool isLla = false;
    };

    /// Host 每帧 IGCtrl 携带的时间戳信息（时钟同步方案.md §3 / §4）。
    /// `rawTimeStamp` = CIGI IGCtrl.TimeStamp（uint32，10µs tick）；
    /// `receivedAtUs` = 本机单调时钟收到时刻（us）。注入式测试直接传该值；
    /// 真实链路下由 `IgSync::update` 记录 `vsg::clock::now()` 转 us。
    struct HostTimeStamp
    {
        std::uint32_t frameCntr = 0;
        std::uint32_t rawTimeStamp = 0;
        std::uint64_t receivedAtUs = 0;
    };

    bool initialize(const AddressConfig& local);
    bool connect(const AddressConfig& hostEndpoint);
    void shutdown();

    void update(bool sendSof = true);

    /// Consume Host eye received during the last Update (if any).
    std::optional<HostEye> takeReceivedHostEye();

    /// Test / injection: enqueue a Host time stamp as if received this frame.
    /// Phase-unwraps `rawTimeStamp` → `lastSimTimeUs` and records `lastReceivedAtUs`.
    /// Returns true if accepted (frameCntr >= last processed), false if dropped as an old frame.
    bool queueHostTimeStamp(const HostTimeStamp& stamp);

    /// Session reset (design §3): TCP reconnect / Host restart clears phase-unwrap state,
    /// so the next packet starts a fresh absolute base (does not inherit the old large value).
    void resetHostSession();

    /// Most recent Host time stamp converted to us (design §3), 0 if none yet.
    std::uint64_t lastHostSimTimeUs() const;

    /// Current compensated simulation time: internal nowUs = vsg::clock::now().
    std::uint64_t simTimeUs() const;

    /// Compensated simulation time at an explicit monotonic-clock instant (test-controllable).
    std::uint64_t simTimeUsAt(std::uint64_t nowUs) const;

    /// Extrapolate-freeze threshold (design §4.3).
    void setExtrapolateTimeoutUs(std::uint64_t timeoutUs);

    /// Explicit freeze check: nowUs - lastReceivedAtUs > timeout → frozen (design §4.3).
    void updateFreeze(std::uint64_t nowUs);

    /// True once extrapolate timeout exceeded and no new frame arrived.
    bool frozen() const;

    const AddressConfig& addressConfig() const { return _local; }

    bool tcpConnected() const;
    bool udpSynced() const;
    IgStatus status() const;

    std::uint32_t igCtrlReceivedCount() const;
    std::uint32_t sofSentCount() const;
    /// FrameCntr from the most recently processed Host IGCtrl (0 if none yet).
    std::uint32_t lastIgCtrlFrameCntr() const;

private:
    static constexpr int tcpConnectTimeoutMs = 200;
    static constexpr int handshakeTimeoutMs = 1000;
    static constexpr int tcpRetryAttempts = 16;
    static constexpr int handshakeRetryAttempts = 8;

    void closeTcp();
    void drainUdp();
    bool tcpConnect(const std::string& ip, int port, int timeoutMs);
    bool sendAll(IgSocketHandle s, const void* data, int len);
    bool recvAll(IgSocketHandle s, void* data, int len, int timeoutMs);
    bool waitUdpAck(int timeoutMs);
    bool connectOnce(const AddressConfig& hostEndpoint);
    void sendSofPacket(std::uint32_t frameCntr);
    /// Poll TCP for peer close (Host offline). Clears both plane flags when dead.
    void refreshConnectionState() const;
    bool isTcpPeerAlive() const;
    void markDisconnected();
    /// 相位展开：把 raw（uint32, 10µs tick）累进 64 位单调 extendedTime（时钟同步方案.md §3）。
    void applyPhaseUnwrap(std::uint32_t raw);

    AddressConfig _local{};
    AddressConfig _hostEndpoint{};
    Network _udp;
    IgSocketHandle _tcp = static_cast<IgSocketHandle>(-1);

    std::atomic<bool> _initialized{false};
    std::atomic<bool> _tcpConnected{false};
    std::atomic<bool> _udpSynced{false};
    std::atomic<IgStatus> _status{IgStatus::IDLE};

    std::atomic<std::uint32_t> _igCtrlReceivedCount{0};
    std::atomic<std::uint32_t> _sofSentCount{0};
    std::uint32_t _lastFrameCntr = 0;
    bool _hasReceivedEye = false;
    HostEye _receivedEye{};

    // 时钟同步（时钟同步方案.md §3 / §4）
    bool _hasTimeStamp = false;
    std::uint32_t _lastRawTimeStamp = 0;          ///< 最近收到 raw（uint32，10µs tick）
    std::uint64_t _extendedTimeTicks = 0;         ///< 相位展开后的 64 位单调 tick
    std::uint64_t _lastSimTimeUs = 0;             ///< = _extendedTimeTicks * 10（us）
    std::uint64_t _lastReceivedAtUs = 0;          ///< 收到该包时的本机单调时钟（us）
    std::uint64_t _extrapolateTimeoutUs = 200000; ///< 默认 200ms
    bool _frozen = false;
};
