#include "IgSync.h"
#include "SyncProtocol.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <thread>

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
    constexpr IgSocketHandle kInvalid = INVALID_SOCKET;
    bool isValidSock(IgSocketHandle s)
    {
        return s != INVALID_SOCKET;
    }
#else
    constexpr IgSocketHandle kInvalid = -1;
    bool isValidSock(IgSocketHandle s)
    {
        return s >= 0;
    }
#endif
} // namespace

IgSync::~IgSync()
{
    shutdown();
}

void IgSync::closeTcp()
{
    if (!isValidSock(_tcp))
        return;
#ifdef WIN32
    closesocket(_tcp);
#else
    close(_tcp);
#endif
    _tcp = kInvalid;
}

void IgSync::drainUdp()
{
    unsigned char drain[64];
    while (_udp.recv(drain, sizeof(drain)) > 0)
    {
    }
}

void IgSync::markDisconnected()
{
    closeTcp();
    _tcpConnected = false;
    _udpSynced = false;
    _status = IgStatus::IDLE;
}

bool IgSync::isTcpPeerAlive() const
{
    if (!isValidSock(_tcp))
        return false;

    fd_set rfds;
    fd_set efds;
    FD_ZERO(&rfds);
    FD_ZERO(&efds);
    FD_SET(_tcp, &rfds);
    FD_SET(_tcp, &efds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 0;

#ifdef WIN32
    const int sel = select(0, &rfds, nullptr, &efds, &tv);
#else
    const int sel = select(static_cast<int>(_tcp) + 1, &rfds, nullptr, &efds, &tv);
#endif
    if (sel < 0)
        return false;
    if (sel == 0)
        return true;

    if (FD_ISSET(_tcp, &efds))
        return false;

    if (!FD_ISSET(_tcp, &rfds))
        return true;

    // Peek without blocking: Host FIN → recv returns 0; pending bytes → still alive.
#ifdef WIN32
    u_long nonBlock = 1;
    ioctlsocket(_tcp, FIONBIO, &nonBlock);
    char peek = 0;
    const int n = recv(_tcp, &peek, 1, MSG_PEEK);
    u_long block = 0;
    ioctlsocket(_tcp, FIONBIO, &block);
    if (n == 0)
        return false;
    if (n < 0)
        return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    const int flags = fcntl(_tcp, F_GETFL, 0);
    fcntl(_tcp, F_SETFL, flags | O_NONBLOCK);
    char peek = 0;
    const int n = static_cast<int>(::recv(_tcp, &peek, 1, MSG_PEEK));
    fcntl(_tcp, F_SETFL, flags);
    if (n == 0)
        return false;
    if (n < 0)
        return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
    return true;
}

void IgSync::refreshConnectionState() const
{
    if (!_tcpConnected.load())
        return;
    if (isTcpPeerAlive())
        return;
    const_cast<IgSync*>(this)->markDisconnected();
}

bool IgSync::tcpConnected() const
{
    refreshConnectionState();
    return _tcpConnected;
}

bool IgSync::udpSynced() const
{
    refreshConnectionState();
    return _udpSynced;
}

bool IgSync::initialize(const AddressConfig& local)
{
    shutdown();
    _local = local;
    _tcpConnected = false;
    _udpSynced = false;
    _status = IgStatus::IDLE;
    _igCtrlReceivedCount = 0;
    _sofSentCount = 0;
    _lastFrameCntr = 0;
    _hasReceivedEye = false;
    _receivedEye = {};
    _hostEndpoint = {};

    if (!_udp.openSocket(_local.addr.c_str(), _local.udpPortSend, _local.udpPortRecv))
    {
        std::cerr << "IgSync: UDP open failed\n";
        return false;
    }

    _initialized = true;
    return true;
}

void IgSync::shutdown()
{
    closeTcp();
    _tcpConnected = false;
    _udpSynced = false;
    _status = IgStatus::IDLE;
    if (_udp.isValid())
        _udp.closeSocket();
    _initialized = false;
}

IgStatus IgSync::status() const
{
    refreshConnectionState();
    return _status.load();
}

std::uint32_t IgSync::igCtrlReceivedCount() const
{
    return _igCtrlReceivedCount.load();
}

std::uint32_t IgSync::sofSentCount() const
{
    return _sofSentCount.load();
}

std::uint32_t IgSync::lastIgCtrlFrameCntr() const
{
    return _lastFrameCntr;
}

std::optional<IgSync::HostEye> IgSync::takeReceivedHostEye()
{
    if (!_hasReceivedEye)
        return std::nullopt;
    _hasReceivedEye = false;
    return _receivedEye;
}

void IgSync::sendSofPacket(std::uint32_t frameCntr)
{
    if (_hostEndpoint.addr.empty())
        return;

    sync_proto::SofMsg sof{};
    sof.magic = sync_proto::kMagic;
    sof.type = static_cast<uint32_t>(sync_proto::MsgType::SOF);
    sof.frameCntr = frameCntr;
    _udp.sendTo(_hostEndpoint.addr.c_str(), _hostEndpoint.udpPortRecv,
                reinterpret_cast<const unsigned char*>(&sof), sizeof(sof));
    _sofSentCount.fetch_add(1);
}

void IgSync::update(bool sendSof)
{
    refreshConnectionState();
    if (!_initialized || !_udpSynced)
        return;

    if (_tcpConnected && _udpSynced)
        _status = IgStatus::RUNNING;

    unsigned char buf[128]{};
    for (;;)
    {
        const int n = _udp.recv(buf, sizeof(buf));
        if (n < static_cast<int>(sizeof(sync_proto::WireMsg)))
            break;

        sync_proto::WireMsg header{};
        std::memcpy(&header, buf, sizeof(header));
        if (header.magic != sync_proto::kMagic)
            continue;

        if (header.type == static_cast<uint32_t>(sync_proto::MsgType::IG_CTRL))
        {
            // Accept full IgCtrlMsg (with optional eye) or legacy 20-byte header-only layout.
            if (n < 20)
                continue;

            sync_proto::IgCtrlMsg igCtrl{};
            const int copyN = n < static_cast<int>(sizeof(igCtrl)) ? n : static_cast<int>(sizeof(igCtrl));
            std::memcpy(&igCtrl, buf, static_cast<size_t>(copyN));

            // Drop whole older bundles (FrameCntr strictly less than last processed).
            if (_igCtrlReceivedCount.load() > 0 && igCtrl.frameCntr < _lastFrameCntr)
                continue;

            _lastFrameCntr = igCtrl.frameCntr;
            _igCtrlReceivedCount.fetch_add(1);

            if (n >= static_cast<int>(sizeof(sync_proto::IgCtrlMsg)) && igCtrl.hasEye != 0)
            {
                _receivedEye.x = igCtrl.posX;
                _receivedEye.y = igCtrl.posY;
                _receivedEye.z = igCtrl.posZ;
                _receivedEye.yawDeg = igCtrl.yawDeg;
                _receivedEye.pitchDeg = igCtrl.pitchDeg;
                _receivedEye.rollDeg = igCtrl.rollDeg;
                _hasReceivedEye = true;
            }

            if (sendSof)
                sendSofPacket(_lastFrameCntr);
        }
        // Ignore handshake acks / other traffic during Update.
    }
}

bool IgSync::sendAll(IgSocketHandle s, const void* data, int len)
{
    const char* p = static_cast<const char*>(data);
    int sent = 0;
    while (sent < len)
    {
#ifdef WIN32
        const int n = send(s, p + sent, len - sent, 0);
#else
        const int n = static_cast<int>(::send(s, p + sent, static_cast<size_t>(len - sent), 0));
#endif
        if (n <= 0)
            return false;
        sent += n;
    }
    return true;
}

bool IgSync::recvAll(IgSocketHandle s, void* data, int len, int timeoutMs)
{
#ifdef WIN32
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    char* p = static_cast<char*>(data);
    int got = 0;
    while (got < len)
    {
#ifdef WIN32
        const int n = recv(s, p + got, len - got, 0);
#else
        const int n = static_cast<int>(::recv(s, p + got, static_cast<size_t>(len - got), 0));
#endif
        if (n <= 0)
            return false;
        got += n;
    }
    return true;
}

bool IgSync::tcpConnect(const std::string& ip, int port, int timeoutMs)
{
#ifdef WIN32
    _tcp = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    _tcp = socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (!isValidSock(_tcp))
        return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = inet_addr(ip.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE)
    {
        closeTcp();
        return false;
    }

#ifdef WIN32
    u_long nonBlock = 1;
    ioctlsocket(_tcp, FIONBIO, &nonBlock);
#else
    const int flags = fcntl(_tcp, F_GETFL, 0);
    fcntl(_tcp, F_SETFL, flags | O_NONBLOCK);
#endif

    const int cr = ::connect(_tcp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
#ifdef WIN32
    if (cr == 0)
    {
        nonBlock = 0;
        ioctlsocket(_tcp, FIONBIO, &nonBlock);
        return true;
    }
    {
        const int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS)
        {
            closeTcp();
            return false;
        }
    }
#else
    if (cr == 0)
    {
        fcntl(_tcp, F_SETFL, flags);
        return true;
    }
    if (errno != EINPROGRESS)
    {
        closeTcp();
        return false;
    }
#endif

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(_tcp, &wfds);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

#ifdef WIN32
    const int sel = select(0, nullptr, &wfds, nullptr, &tv);
#else
    const int sel = select(_tcp + 1, nullptr, &wfds, nullptr, &tv);
#endif
    if (sel <= 0)
    {
        closeTcp();
        return false;
    }

    int soError = 0;
#ifdef WIN32
    int optLen = sizeof(soError);
    getsockopt(_tcp, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &optLen);
    nonBlock = 0;
    ioctlsocket(_tcp, FIONBIO, &nonBlock);
#else
    socklen_t optLen = sizeof(soError);
    getsockopt(_tcp, SOL_SOCKET, SO_ERROR, &soError, &optLen);
    fcntl(_tcp, F_SETFL, flags);
#endif
    if (soError != 0)
    {
        closeTcp();
        return false;
    }
    return true;
}

bool IgSync::waitUdpAck(int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    unsigned char buf[64]{};
    while (std::chrono::steady_clock::now() < deadline)
    {
        const int n = _udp.recv(buf, sizeof(buf));
        if (n >= static_cast<int>(sizeof(sync_proto::WireMsg)))
        {
            sync_proto::WireMsg msg{};
            std::memcpy(&msg, buf, sizeof(msg));
            if (msg.magic == sync_proto::kMagic &&
                msg.type == static_cast<uint32_t>(sync_proto::MsgType::UDP_SYNC_ACK))
                return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool IgSync::connectOnce(const AddressConfig& hostEndpoint)
{
    // Assumes TCP already connected on _tcp.
    sync_proto::WireMsg hello{};
    hello.magic = sync_proto::kMagic;
    hello.type = static_cast<uint32_t>(sync_proto::MsgType::HELLO);
    hello.udpRecvPort = static_cast<uint32_t>(_local.udpPortRecv);
    if (!sendAll(_tcp, &hello, sizeof(hello)))
        return false;

    sync_proto::WireMsg ack{};
    if (!recvAll(_tcp, &ack, sizeof(ack), handshakeTimeoutMs) || ack.magic != sync_proto::kMagic ||
        ack.type != static_cast<uint32_t>(sync_proto::MsgType::HELLO_ACK))
        return false;

    sync_proto::WireMsg udpSync{};
    udpSync.magic = sync_proto::kMagic;
    udpSync.type = static_cast<uint32_t>(sync_proto::MsgType::UDP_SYNC);
    udpSync.udpRecvPort = static_cast<uint32_t>(_local.udpPortRecv);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(handshakeTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        _udp.sendTo(hostEndpoint.addr.c_str(), hostEndpoint.udpPortRecv,
                    reinterpret_cast<const unsigned char*>(&udpSync), sizeof(udpSync));
        if (waitUdpAck(50))
            return true;
    }
    return false;
}

bool IgSync::connect(const AddressConfig& hostEndpoint)
{
    if (!_initialized)
        return false;

    _tcpConnected = false;
    _udpSynced = false;

    // TCP retries: Host may still be starting (reconnect BDD).
    // Handshake retries (few): rare UDP loss — wrong UDP port fails quickly.
    int handshakeFails = 0;
    for (int attempt = 0; attempt < tcpRetryAttempts; ++attempt)
    {
        closeTcp();
        drainUdp();

        if (!tcpConnect(hostEndpoint.addr, hostEndpoint.tcpPort, tcpConnectTimeoutMs))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            continue;
        }

        if (connectOnce(hostEndpoint))
        {
            _hostEndpoint = hostEndpoint;
            _tcpConnected = true;
            _udpSynced = true;
            _status = IgStatus::RUNNING;
            return true;
        }

        closeTcp();
        if (++handshakeFails >= handshakeRetryAttempts)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    closeTcp();
    _tcpConnected = false;
    _udpSynced = false;
    return false;
}
