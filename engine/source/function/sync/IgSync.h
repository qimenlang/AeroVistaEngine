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

    bool tcpConnected() const;
    bool udpSynced() const;

private:
    static constexpr int TcpConnectTimeoutMs = 200;
    static constexpr int HandshakeTimeoutMs = 400;
    static constexpr int TcpRetryAttempts = 8;
    static constexpr int HandshakeRetryAttempts = 2;

    void closeTcp();
    void drainUdp();
    bool tcpConnect(const std::string& ip, int port, int timeoutMs);
    bool sendAll(IgSocketHandle s, const void* data, int len);
    bool recvAll(IgSocketHandle s, void* data, int len, int timeoutMs);
    bool waitUdpAck(int timeoutMs);
    bool connectOnce(const AddressConfig& hostEndpoint);

    AddressConfig _local{};
    Network _udp;
    IgSocketHandle _tcp = static_cast<IgSocketHandle>(-1);

    std::atomic<bool> _initialized{false};
    std::atomic<bool> _tcpConnected{false};
    std::atomic<bool> _udpSynced{false};
};
