#pragma once

#include "Network.h"
#include "SyncConfig.h"

#include <atomic>
#include <cstdint>
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

    bool Initialize(const AddressConfig& local);
    bool Connect(const AddressConfig& hostEndpoint);
    void Shutdown();

    void Update(bool sendSof = true);

    const AddressConfig& addressConfig() const { return _local; }

    bool tcpConnected() const;
    bool udpSynced() const;
    IgStatus status() const;

    std::uint32_t igCtrlReceivedCount() const;
    std::uint32_t sofSentCount() const;

private:
    static constexpr int TcpConnectTimeoutMs = 200;
    static constexpr int HandshakeTimeoutMs = 1000;
    static constexpr int TcpRetryAttempts = 16;
    static constexpr int HandshakeRetryAttempts = 8;

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
    std::atomic<IgStatus> _status{IgStatus::Idle};

    std::atomic<std::uint32_t> _igCtrlReceivedCount{0};
    std::atomic<std::uint32_t> _sofSentCount{0};
    std::uint32_t _lastFrameCntr = 0;
};
