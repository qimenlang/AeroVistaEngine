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
    };

    bool initialize(const AddressConfig& local);
    bool connect(const AddressConfig& hostEndpoint);
    void shutdown();

    void update(bool sendSof = true);

    /// Consume Host eye received during the last Update (if any).
    std::optional<HostEye> takeReceivedHostEye();

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
};
