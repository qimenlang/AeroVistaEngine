#pragma once

#include "Network.h"
#include "SyncConfig.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef WIN32
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <winsock2.h>
using SocketHandle = SOCKET;
#else
using SocketHandle = int;
#endif

/// Host-side sync endpoint: UDP sync plane + TCP command listen.
class HostSync
{
public:
    HostSync() = default;
    ~HostSync();

    HostSync(const HostSync&) = delete;
    HostSync& operator=(const HostSync&) = delete;

    bool initialize(const AddressConfig& local);
    void shutdown();

    void run();
    struct EyePose
    {
        double x = 0, y = 0, z = 0;
        double yawDeg = 0, pitchDeg = 0, rollDeg = 0;
    };
    /// Fan-out IGCtrl (+ optional Host eye) to all ready IGs.
    void update(double simTimeMs = 0.0, const EyePose* eye = nullptr);
    void setPaceConfig(const SyncPaceConfig& pace);

    const AddressConfig& addressConfig() const { return _local; }

    HostStatus status() const;
    bool hasReadyIg() const;
    int readyIgCount() const;

    std::uint32_t igCtrlSentCount() const;
    std::uint32_t sofReceivedCount() const;

private:
    struct IgPeer
    {
        SocketHandle tcp = static_cast<SocketHandle>(-1);
        std::string ip;
        uint32_t udpRecvPort = 0;
        bool tcpReady = false;
        bool udpReady = false;
    };

    void acceptLoop();
    void udpLoop();
    void handleClient(SocketHandle client, std::string peerIp);
    void closeSocket(SocketHandle& s);
    void joinClientThreads();
    int countReadyUnlocked() const;
    void processUdpDatagram(const unsigned char* buf, int n, const char* fromIp);
    void pollUdp();

    AddressConfig _local{};
    SyncPaceConfig _pace{};
    Network _udp;
    SocketHandle _listenSocket = static_cast<SocketHandle>(-1);

    std::atomic<bool> _threadsRunning{false};
    std::atomic<HostStatus> _status{HostStatus::Idle};
    std::thread _acceptThread;
    std::thread _udpThread;

    mutable std::mutex _peersMutex;
    std::vector<IgPeer> _peers;
    std::unordered_map<uint32_t, std::string> _earlyUdpSyncByPort;

    std::mutex _clientThreadsMutex;
    std::vector<std::thread> _clientThreads;

    mutable std::mutex _udpMutex;

    std::atomic<std::uint32_t> _igCtrlSentCount{0};
    std::atomic<std::uint32_t> _sofReceivedCount{0};
    std::uint32_t _frameCounter = 0;
};
