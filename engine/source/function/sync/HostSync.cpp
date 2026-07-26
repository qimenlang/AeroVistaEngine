#include "HostSync.h"
#include "SyncProtocol.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>

#ifdef WIN32
#    include <ws2tcpip.h>
#else
#    include <arpa/inet.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#endif

namespace
{
#ifdef WIN32
    constexpr SocketHandle kInvalid = INVALID_SOCKET;
    bool isValidSock(SocketHandle s)
    {
        return s != INVALID_SOCKET;
    }
#else
    constexpr SocketHandle kInvalid = -1;
    bool isValidSock(SocketHandle s)
    {
        return s >= 0;
    }
#endif
} // namespace

HostSync::~HostSync()
{
    Shutdown();
}

void HostSync::closeSocket(SocketHandle& s)
{
    if (!isValidSock(s))
        return;
#ifdef WIN32
    closesocket(s);
#else
    close(s);
#endif
    s = kInvalid;
}

void HostSync::joinClientThreads()
{
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(_clientThreadsMutex);
        threads.swap(_clientThreads);
    }
    for (auto& t : threads)
    {
        if (t.joinable())
            t.join();
    }
}

int HostSync::countReadyUnlocked() const
{
    int n = 0;
    for (const auto& p : _peers)
    {
        if (p.tcpReady && p.udpReady)
            ++n;
    }
    return n;
}

bool HostSync::hasReadyIg() const
{
    std::lock_guard lock(_peersMutex);
    return countReadyUnlocked() > 0;
}

int HostSync::readyIgCount() const
{
    std::lock_guard lock(_peersMutex);
    return countReadyUnlocked();
}

HostStatus HostSync::status() const
{
    return _status.load();
}

std::uint32_t HostSync::igCtrlSentCount() const
{
    return _igCtrlSentCount.load();
}

std::uint32_t HostSync::sofReceivedCount() const
{
    const_cast<HostSync*>(this)->pollUdp();
    return _sofReceivedCount.load();
}

void HostSync::SetPaceConfig(const SyncPaceConfig& pace)
{
    _pace = pace;
}

void HostSync::Run()
{
    _status = HostStatus::Running;
}

void HostSync::pollUdp()
{
    struct Packet
    {
        unsigned char buf[64]{};
        char fromIp[64]{};
        int n = 0;
    };
    std::vector<Packet> packets;

    {
        std::lock_guard lock(_udpMutex);
        if (!_udp.isValid())
            return;

        for (;;)
        {
            Packet p{};
            int fromPort = 0;
            p.n = _udp.recvFrom(p.buf, sizeof(p.buf), p.fromIp, sizeof(p.fromIp), &fromPort);
            if (p.n <= 0)
                break;
            packets.push_back(p);
        }
    }

    for (const auto& p : packets)
        processUdpDatagram(p.buf, p.n, p.fromIp);
}

void HostSync::Update(double simTimeMs)
{
    if (_status.load() != HostStatus::Running)
        return;

    // FreeRun: never block on SOF. Barrier reserved for later.
    (void)_pace;

    pollUdp();

    sync_proto::IgCtrlMsg msg{};
    msg.magic = sync_proto::kMagic;
    msg.type = static_cast<uint32_t>(sync_proto::MsgType::IgCtrl);
    msg.frameCntr = _frameCounter++;
    msg.simTimeMs = simTimeMs;

    std::vector<std::pair<std::string, uint32_t>> targets;
    {
        std::lock_guard lock(_peersMutex);
        targets.reserve(_peers.size());
        for (const auto& p : _peers)
        {
            if (p.tcpReady && p.udpReady)
                targets.emplace_back(p.ip, p.udpRecvPort);
        }
    }

    {
        std::lock_guard lock(_udpMutex);
        for (const auto& t : targets)
        {
            _udp.sendTo(t.first.c_str(), static_cast<int>(t.second),
                        reinterpret_cast<const unsigned char*>(&msg), sizeof(msg));
        }
    }

    _igCtrlSentCount.fetch_add(1);
}

bool HostSync::Initialize(const AddressConfig& local)
{
    Shutdown();
    _local = local;
    _status = HostStatus::Idle;
    _igCtrlSentCount = 0;
    _sofReceivedCount = 0;
    _frameCounter = 0;

    if (!_udp.openSocket(_local.addr.c_str(), _local.udpPortSend, _local.udpPortRecv))
    {
        std::cerr << "HostSync: UDP open failed\n";
        return false;
    }

#ifdef WIN32
    _listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    _listenSocket = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (!isValidSock(_listenSocket))
    {
        _udp.closeSocket();
        return false;
    }

    int yes = 1;
#ifdef WIN32
    setsockopt(_listenSocket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
#else
    setsockopt(_listenSocket, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(_local.tcpPort));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(_listenSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        std::cerr << "HostSync: TCP bind failed on " << _local.tcpPort << "\n";
        closeSocket(_listenSocket);
        _udp.closeSocket();
        return false;
    }

    if (listen(_listenSocket, 16) != 0)
    {
        closeSocket(_listenSocket);
        _udp.closeSocket();
        return false;
    }

#ifdef WIN32
    u_long nonBlock = 1;
    ioctlsocket(_listenSocket, FIONBIO, &nonBlock);
#else
    fcntl(_listenSocket, F_SETFL, O_NONBLOCK);
#endif

    _threadsRunning = true;
    _acceptThread = std::thread(&HostSync::acceptLoop, this);
    _udpThread = std::thread(&HostSync::udpLoop, this);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return true;
}

void HostSync::Shutdown()
{
    _threadsRunning = false;
    _status = HostStatus::Idle;
    closeSocket(_listenSocket);

    if (_acceptThread.joinable())
        _acceptThread.join();
    if (_udpThread.joinable())
        _udpThread.join();
    joinClientThreads();

    {
        std::lock_guard lock(_peersMutex);
        for (auto& p : _peers)
            closeSocket(p.tcp);
        _peers.clear();
        _earlyUdpSyncByPort.clear();
    }

    if (_udp.isValid())
        _udp.closeSocket();
}

void HostSync::acceptLoop()
{
    while (_threadsRunning)
    {
        sockaddr_in clientAddr{};
#ifdef WIN32
        int len = sizeof(clientAddr);
#else
        socklen_t len = sizeof(clientAddr);
#endif
        SocketHandle client = accept(_listenSocket, reinterpret_cast<sockaddr*>(&clientAddr), &len);
        if (!isValidSock(client))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        char ipBuf[64]{};
#ifdef WIN32
        strncpy_s(ipBuf, inet_ntoa(clientAddr.sin_addr), _TRUNCATE);
#else
        std::snprintf(ipBuf, sizeof(ipBuf), "%s", inet_ntoa(clientAddr.sin_addr));
#endif

        std::thread worker(&HostSync::handleClient, this, client, std::string(ipBuf));
        {
            std::lock_guard lock(_clientThreadsMutex);
            _clientThreads.push_back(std::move(worker));
        }
    }
}

void HostSync::handleClient(SocketHandle client, std::string peerIp)
{
#ifdef WIN32
    DWORD timeoutMs = 1000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    sync_proto::WireMsg hello{};
#ifdef WIN32
    const int n = recv(client, reinterpret_cast<char*>(&hello), sizeof(hello), 0);
#else
    const int n = static_cast<int>(::recv(client, &hello, sizeof(hello), 0));
#endif
    if (n != static_cast<int>(sizeof(hello)) || hello.magic != sync_proto::kMagic ||
        hello.type != static_cast<uint32_t>(sync_proto::MsgType::Hello))
    {
        closeSocket(client);
        return;
    }

    bool udpAlready = false;
    {
        std::lock_guard lock(_peersMutex);
        IgPeer peer;
        peer.tcp = client;
        peer.ip = peerIp;
        peer.udpRecvPort = hello.udpRecvPort;
        peer.tcpReady = true;
        udpAlready = _earlyUdpSyncByPort.erase(hello.udpRecvPort) > 0;
        peer.udpReady = udpAlready;
        _peers.push_back(std::move(peer));
    }

    sync_proto::WireMsg ack{};
    ack.magic = sync_proto::kMagic;
    ack.type = static_cast<uint32_t>(sync_proto::MsgType::HelloAck);
    ack.udpRecvPort = static_cast<uint32_t>(_local.udpPortRecv);
#ifdef WIN32
    send(client, reinterpret_cast<const char*>(&ack), sizeof(ack), 0);
#else
    ::send(client, &ack, sizeof(ack), 0);
#endif

    if (udpAlready)
    {
        sync_proto::WireMsg udpAck{};
        udpAck.magic = sync_proto::kMagic;
        udpAck.type = static_cast<uint32_t>(sync_proto::MsgType::UdpSyncAck);
        udpAck.udpRecvPort = static_cast<uint32_t>(_local.udpPortRecv);
        std::lock_guard lock(_udpMutex);
        _udp.sendTo(peerIp.c_str(), static_cast<int>(hello.udpRecvPort),
                    reinterpret_cast<const unsigned char*>(&udpAck), sizeof(udpAck));
    }
}

void HostSync::processUdpDatagram(const unsigned char* buf, int n, const char* fromIp)
{
    if (n < static_cast<int>(sizeof(sync_proto::WireMsg)))
        return;

    sync_proto::WireMsg header{};
    std::memcpy(&header, buf, sizeof(header));
    if (header.magic != sync_proto::kMagic)
        return;

    if (header.type == static_cast<uint32_t>(sync_proto::MsgType::Sof))
    {
        if (n >= static_cast<int>(sizeof(sync_proto::SofMsg)))
            _sofReceivedCount.fetch_add(1);
        return;
    }

    if (header.type != static_cast<uint32_t>(sync_proto::MsgType::UdpSync))
        return;

    const uint32_t replyPort = header.udpRecvPort;
    std::string replyIp = fromIp;
    {
        std::lock_guard lock(_peersMutex);
        bool matched = false;
        for (auto& p : _peers)
        {
            if (p.tcpReady && p.udpRecvPort == header.udpRecvPort)
            {
                p.udpReady = true;
                if (!p.ip.empty())
                    replyIp = p.ip;
                matched = true;
                break;
            }
        }
        if (!matched)
            _earlyUdpSyncByPort[header.udpRecvPort] = replyIp;
    }

    sync_proto::WireMsg ack{};
    ack.magic = sync_proto::kMagic;
    ack.type = static_cast<uint32_t>(sync_proto::MsgType::UdpSyncAck);
    ack.udpRecvPort = static_cast<uint32_t>(_local.udpPortRecv);
    {
        std::lock_guard lock(_udpMutex);
        _udp.sendTo(replyIp.c_str(), static_cast<int>(replyPort),
                    reinterpret_cast<const unsigned char*>(&ack), sizeof(ack));
    }
}

void HostSync::udpLoop()
{
    while (_threadsRunning)
    {
        pollUdp();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
