// TcpSocket 单元测试：本机 loopback 验证 socket 基础功能分层。
// 覆盖：listen/accept/connect、sendAll/recvAll、recv 的 DATA/PEER_CLOSED/TIMEOUT、
//       connect 失败、accept 回填 peerIp、close 幂等。
// 不依赖真实网络（127.0.0.1），也不走 HostSync/IgSync 业务层。

#include <catch2/catch_test_macros.hpp>

#include <aerovista/sync/TcpSocket.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>

using aerovista::sync::RecvKind;
using aerovista::sync::RecvOutcome;
using aerovista::sync::TcpSocket;

namespace
{
    // TCP 一次连接需要三个 socket：服务端监听 socket（只 bind/listen/accept，不收发数据）、
    // accept 产出的已连接 socket（server，服务端收发）、connect 产出的已连接 socket（client，客户端收发）。
    struct LoopbackPair
    {
        TcpSocket listener;
        TcpSocket server; // accept 产物
        TcpSocket client; // connect 产物
        int port = 0;
    };

    bool establishPair(LoopbackPair& pair)
    {
        if (!pair.listener.listen(0))
            return false;
        pair.port = pair.listener.localPort();
        if (pair.port <= 0)
            return false;
        if (!pair.client.connect("127.0.0.1", pair.port, 1000))
            return false;
        // connect 返回即握手完成，非阻塞 accept 应立即可用；重试兜底极小竞态。
        for (int i = 0; i < 50 && !pair.listener.accept(pair.server); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return pair.server.valid();
    }
} // namespace

TEST_CASE("TcpSocket listen/accept/connect 建立 loopback 连接", "[unit]")
{
    LoopbackPair pair;
    REQUIRE(establishPair(pair));
    REQUIRE(pair.server.valid());
    REQUIRE(pair.client.valid());
    REQUIRE(pair.listener.listening());
    REQUIRE_FALSE(pair.server.listening());
    REQUIRE_FALSE(pair.client.listening());
}

TEST_CASE("TcpSocket sendAll/recvAll 双向收发完整字节", "[unit]")
{
    LoopbackPair pair;
    REQUIRE(establishPair(pair));

    const char msg[] = "hello-tcp-socket";
    REQUIRE(pair.client.sendAll(msg, sizeof(msg)));

    char buf[64]{};
    REQUIRE(pair.server.recvAll(buf, sizeof(msg), 1000));
    REQUIRE(std::memcmp(buf, msg, sizeof(msg)) == 0);
}

TEST_CASE("TcpSocket recv 区分 PEER_CLOSED(对端关闭)", "[unit]")
{
    LoopbackPair pair;
    REQUIRE(establishPair(pair));

    pair.client.close();

    char buf[64]{};
    const RecvOutcome outcome = pair.server.recv(buf, sizeof(buf));
    REQUIRE(outcome.kind == RecvKind::PEER_CLOSED);
}

TEST_CASE("TcpSocket recv 无数据超时返回 TIMEOUT", "[unit]")
{
    LoopbackPair pair;
    REQUIRE(establishPair(pair));

    pair.server.setRecvTimeout(100);

    char buf[64]{};
    const RecvOutcome outcome = pair.server.recv(buf, sizeof(buf));
    REQUIRE(outcome.kind == RecvKind::TIMEOUT);
    REQUIRE(outcome.bytes == 0);
}

TEST_CASE("TcpSocket connect 到未监听端口失败", "[unit]")
{
    // 先 listen(0) 取端口再关闭，得到一个确定无监听的端口。
    TcpSocket listener;
    REQUIRE(listener.listen(0));
    const int port = listener.localPort();
    REQUIRE(port > 0);
    listener.close();

    TcpSocket client;
    std::string error;
    REQUIRE_FALSE(client.connect("127.0.0.1", port, 500, &error));
    REQUIRE_FALSE(client.valid());
}

TEST_CASE("TcpSocket accept 回填对端 IPv4", "[unit]")
{
    TcpSocket listener;
    REQUIRE(listener.listen(0));
    const int port = listener.localPort();
    REQUIRE(port > 0);

    TcpSocket client;
    REQUIRE(client.connect("127.0.0.1", port, 1000));

    TcpSocket server;
    std::string peerIp;
    for (int i = 0; i < 50 && !listener.accept(server, &peerIp); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    REQUIRE(server.valid());
    REQUIRE(peerIp == "127.0.0.1");
}

TEST_CASE("TcpSocket close 幂等", "[unit]")
{
    LoopbackPair pair;
    REQUIRE(establishPair(pair));
    REQUIRE(pair.server.valid());

    pair.server.close();
    REQUIRE_FALSE(pair.server.valid());
    pair.server.close(); // 幂等，不崩溃
    REQUIRE_FALSE(pair.server.valid());
}
