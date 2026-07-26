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

    bool Initialize(const AddressConfig& local);
    void Shutdown();

    bool hasReadyIg() const;
    int readyIgCount() const;

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

    AddressConfig _local{};
    Network _udp;
    SocketHandle _listenSocket = static_cast<SocketHandle>(-1);

    std::atomic<bool> _running{false};
    std::thread _acceptThread;
    std::thread _udpThread;

    mutable std::mutex _peersMutex;
    std::vector<IgPeer> _peers;
    std::unordered_map<uint32_t, std::string> _earlyUdpSyncByPort;

    std::mutex _clientThreadsMutex;
    std::vector<std::thread> _clientThreads;
};
