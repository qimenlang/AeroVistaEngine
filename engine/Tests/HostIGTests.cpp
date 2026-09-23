#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine.h"
#include <aerovista/sync/CigiIncludes.h>
#include <aerovista/sync/CigiWire.h>
#include <aerovista/sync/HostSync.h>
#include <aerovista/sync/IgSync.h>
#include <aerovista/sync/SyncConfig.h>
#include <aerovista/sync/SyncInterface.h>
#include <aerovista/sync/SynchronSystem.h>

#include "CigiBaseEntityPositionCtrl.h"
#include "CigiEntityPositionCtrlV4.h"
#include "CigiHostSession.h"
#include "CigiIGCtrlV4.h"
#include "CigiIGSession.h"
#include "CigiSOFV4.h"
#include "CigiSymbolTextDefV4.h"

#include "CigiIGMsgV4.h"
#include "CigiWeatherCtrlV4.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Common.h"

using aerovista::sync::HostConfig;
using aerovista::sync::HostStatus;
using aerovista::sync::HostSync;
using aerovista::sync::IgConfig;
using aerovista::sync::IgStatus;
using aerovista::sync::IgSync;
using aerovista::sync::OffsetDeg;
using aerovista::sync::SynchronSystem;
using aerovista::sync::SyncInterface;
using aerovista::sync::SyncSystemConfig;
using aerovista::sync::TcpSocket;
namespace cigi_wire = aerovista::sync::cigi_wire;

// 协议分层（测试约定）：
// - 握手：CIGI HELLO（SOF+IGMsg，无 TCP ACK）+ UDP_SYNC / ACK —— §1 / PLT-hello-ch。
// - 数据面（帧节拍 / 眼点 / SOF）：CIGI V4 CCL —— IGCtrl (+ 可选 EntityPositionCtrl) / SOF。
//   数据面契约走 HostSync/IgSync 可观察收发（[wire-contract]）；CCL 首包约束仍为 session 负向单测。

namespace
{
    // 默认端口见 doc/design/多通道同步/多通道同步模块设计.md
    HostConfig makeHostLocal()
    {
        return HostConfig{8000, 8100};
    }

    IgConfig makeIgLocal(int udpRecvPort = 8001)
    {
        return IgConfig{udpRecvPort, {"127.0.0.1", 8100, 8000}};
    }

    // UDP 可丢：actual 落在 [expected-slack, expected]
    bool approxAtMost(std::uint32_t actual, int expected, int slack)
    {
        const auto exp = static_cast<std::uint32_t>(expected);
        const auto minOk = exp > static_cast<std::uint32_t>(slack) ? exp - static_cast<std::uint32_t>(slack) : 0u;
        return actual >= minOk && actual <= exp;
    }

    /// IGCtrl 由 outMsgWithIgCtrlUdp() 自动前置（帧号/自计时时间戳）；hostSendFrame 只发无眼点帧。
    void hostSendFrame(HostSync& host, double /*simTimeMs*/)
    {
        host.outMsgWithIgCtrlUdp();
        host.flushUdp();
    }

    void hostSendEyePose(HostSync& host, const cigi_wire::EyePose& eye)
    {
        auto& omsg = host.outMsgWithIgCtrlUdp();
        cigi_wire::appendEye(omsg, &eye);
        host.flushUdp();
    }

    // 业务侧扇出一帧 IGCtrl + 眼点（outMsgWithIgCtrlUdp 自动前置 IGCtrl，appendEye 追加 ownship）。
    void hostSendEyeFrame(HostSync& host, const ChannelEye& eye)
    {
        cigi_wire::EyePose wire{};
        wire.x = eye.lla.x;
        wire.y = eye.lla.y;
        wire.z = eye.lla.z;
        wire.yawDeg = eye.eulerYprDeg.x;
        wire.pitchDeg = eye.eulerYprDeg.y;
        wire.rollDeg = eye.eulerYprDeg.z;
        hostSendEyePose(host, wire);
    }

    bool linkHostIg(HostSync& host, IgSync& ig, int base)
    {
        if (!host.initialize(makeTestHostConfig(base)))
            return false;
        const IgConfig igConfig = makeTestIgConfig(base + 1, base);
        if (!ig.initialize(igConfig.udpPortRecv) || !ig.connect(igConfig.target))
            return false;
        host.run();
        return true;
    }

    std::vector<std::vector<unsigned char>> collectFrames(
        const std::function<std::vector<std::vector<unsigned char>>()>& take, std::size_t minCount)
    {
        std::vector<std::vector<unsigned char>> frames;
        for (int i = 0; i < 40 && frames.size() < minCount; ++i)
        {
            auto more = take();
            frames.insert(frames.end(), more.begin(), more.end());
            if (frames.size() < minCount)
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return frames;
    }

    std::vector<unsigned char> concatFrames(const std::vector<unsigned char>& a,
                                            const std::vector<unsigned char>& b)
    {
        std::vector<unsigned char> out = a;
        out.insert(out.end(), b.begin(), b.end());
        return out;
    }

    struct OwnshipEyeCapture
    {
        bool got = false;
        std::uint16_t entityId = 0;
        std::uint16_t parentId = 0;
        CigiBaseEntityPositionCtrl::AttachStateGrp attachState = CigiBaseEntityPositionCtrl::Detach;
        double lat = 0.0;
        double lon = 0.0;
        double alt = 0.0;
        double yawDeg = 0.0;
        double pitchDeg = 0.0;
        double rollDeg = 0.0;
    };

    void captureOwnship(IgSync& ig, OwnshipEyeCapture& cap)
    {
        ig.addCallback<CigiEntityPositionCtrlV4>([&](const CigiEntityPositionCtrlV4& pose) {
            if (pose.GetEntityID() != 0)
                return;
            cap.got = true;
            cap.entityId = pose.GetEntityID();
            cap.parentId = pose.GetParentID();
            cap.attachState = pose.GetAttachState();
            cap.lat = pose.GetLat();
            cap.lon = pose.GetLon();
            cap.alt = pose.GetAlt();
            cap.yawDeg = pose.GetYaw();
            cap.pitchDeg = pose.GetPitch();
            cap.rollDeg = pose.GetRoll();
        });
    }

    void pumpIgCtrlFrames(HostSync& host, IgSync& ig, int frames = 5)
    {
        for (int i = 0; i < frames; ++i)
        {
            hostSendFrame(host, i * 16.667);
            ig.drainIncoming();
            ig.update();
        }
    }

    void pumpOwnshipEyeFrames(HostSync& host, IgSync& ig, const cigi_wire::EyePose& eye, int frames = 5)
    {
        for (int i = 0; i < frames; ++i)
        {
            hostSendEyePose(host, eye);
            ig.drainIncoming();
            ig.update();
        }
    }

    // 独立 Host 端点（测试用）：持 HostSync，RAII 生命周期。
    // 端口语义 = makeTestHostConfig（Common.h），与 makeTestIgConfig 的 target 对齐。
    struct TestHost
    {
        HostSync sync;
        bool init(int base)
        {
            if (!sync.initialize(makeTestHostConfig(base)))
                return false;
            sync.run();
            return true;
        }
    };
} // namespace

SCENARIO("linked IG receives Host ownship eye as Detach LLA EntityID 0",
         "[integration][sync][cigi][wire-contract][lla][CIGI-ownship-lla]")
{
    GIVEN("a Host and an IG that have completed CIGI handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(linkHostIg(host, ig, 19100));

        OwnshipEyeCapture cap;
        captureOwnship(ig, cap);

        WHEN("Host sends IGCtrl with an ownship eye")
        {
            cigi_wire::EyePose eye{};
            eye.x = 39.9;
            eye.y = 116.4;
            eye.z = 500.0;
            eye.yawDeg = 30.0;
            eye.pitchDeg = 10.0;
            pumpOwnshipEyeFrames(host, ig, eye);

            THEN("IG unpacks Detach LLA ownship at EntityID 0 ParentID 0")
            {
                REQUIRE(ig.igCtrlReceivedCount() >= 1);
                REQUIRE(cap.got);
                REQUIRE(cap.entityId == 0);
                REQUIRE(cap.parentId == 0);
                REQUIRE(cap.attachState == CigiBaseEntityPositionCtrl::Detach);
                REQUIRE(cap.lat == Catch::Approx(eye.x));
                REQUIRE(cap.lon == Catch::Approx(eye.y));
                REQUIRE(cap.alt == Catch::Approx(eye.z));
                REQUIRE(cap.yawDeg == Catch::Approx(eye.yawDeg));
                REQUIRE(cap.pitchDeg == Catch::Approx(eye.pitchDeg));
            }
        }
    }
}

SCENARIO("linked IG receives IGCtrl when Host sends a frame without eye",
         "[integration][sync][cigi][wire-contract][CIGI-igctrl-no-eye]")
{
    GIVEN("a Host and an IG that have completed CIGI handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(linkHostIg(host, ig, 19200));

        OwnshipEyeCapture cap;
        captureOwnship(ig, cap);

        WHEN("Host sends IGCtrl with no ownship eye")
        {
            pumpIgCtrlFrames(host, ig);

            THEN("IG received IGCtrl and no ownship EntityPosition")
            {
                REQUIRE(ig.igCtrlReceivedCount() >= 1);
                REQUIRE_FALSE(cap.got);
            }
        }
    }
}

namespace
{
    // 手工构造一条「首包非 IGCtrl/SOF」的最小 CIGI4 消息（绕过 Pack 的版本域歧义）。
    // 布局：PacketSize(2,LE) + PacketID(2,LE) + 版本域(2) + 保留(2)。
    std::vector<unsigned char> makeBadFirstPacketMsg(std::uint16_t packetId)
    {
        std::vector<unsigned char> msg(8, 0);
        msg[0] = 8; // PacketSize=8（LE）
        msg[1] = 0;
        msg[2] = static_cast<unsigned char>(packetId & 0xFF); // PacketID
        msg[3] = static_cast<unsigned char>(packetId >> 8);
        msg[4] = 4; // CIGI 主版本 4（CIGI4 版本域在字节 4）
        return msg;
    }
} // namespace

TEST_CASE("CIGI IG rejects a message whose first packet is not IGCtrl",
          "[unit][cigi][wire-contract][negative][CIGI-first-igctrl]")
{
    // CCL 硬约束（CigiIncomingMsg.cpp CheckFirstPacket）：IG 入站消息首包必须 IGCtrl（0x0000），
    // 否则拒绝。构造首包 PacketID=0x0001（EntityPosition，非 IGCtrl）。
    const auto badMsg = makeBadFirstPacketMsg(0x0001);

    auto ig = std::make_unique<CigiIGSession>(1, 4096, 1, 4096);
    int stat = CIGI_SUCCESS;
    try
    {
        stat = ig->GetIncomingMsgMgr().ProcessIncomingMsg(
            const_cast<unsigned char*>(badMsg.data()), static_cast<int>(badMsg.size()));
    }
    catch (...)
    {
        stat = CIGI_ERROR_MISSING_IG_CONTROL_PACKET; // 抛异常也视为拒绝
    }
    REQUIRE(stat != CIGI_SUCCESS);
}

TEST_CASE("CIGI Host rejects a message whose first packet is not SOF",
          "[unit][cigi][wire-contract][negative][CIGI-first-sof]")
{
    // CCL 硬约束（CigiIncomingMsg.cpp CheckFirstPacket）：Host 入站消息首包必须 SOF（0xffff），
    // 否则拒绝。构造首包 PacketID=0x0001（IGCtrl，非 SOF）。
    const auto badMsg = makeBadFirstPacketMsg(0x0001);

    auto host = std::make_unique<CigiHostSession>(1, 4096, 1, 4096);
    int stat = CIGI_SUCCESS;
    try
    {
        stat = host->GetIncomingMsgMgr().ProcessIncomingMsg(
            const_cast<unsigned char*>(badMsg.data()), static_cast<int>(badMsg.size()));
    }
    catch (...)
    {
        stat = CIGI_ERROR_MISSING_SOF_PACKET; // 抛异常也视为拒绝
    }
    REQUIRE(stat != CIGI_SUCCESS);
}

// =============================================================================
// 1. 连接面（集成；握手为 CIGI HELLO + UDP_SYNC）
// =============================================================================

SCENARIO("Host initializes with no ready IG", "[integration][sync][initialize][HS-host-init-empty]")
{
    GIVEN("a new HostSync")
    {
        HostSync host;

        WHEN("it is initialized")
        {
            const bool ok = host.initialize(makeHostLocal());

            THEN("initialization succeeds and no IG is ready yet")
            {
                REQUIRE(ok);
                REQUIRE_FALSE(host.hasReadyIg());
                REQUIRE(host.readyIgCount() == 0);
            }
        }
    }
}

SCENARIO("IG initializes disconnected from any Host", "[integration][sync][initialize][HS-ig-init-disconnected]")
{
    GIVEN("a new IgSync")
    {
        IgSync ig;

        WHEN("it is initialized")
        {
            const bool ok = ig.initialize(makeIgLocal().udpPortRecv);

            THEN("initialization succeeds and it is not connected to a Host yet")
            {
                REQUIRE(ok);
                REQUIRE_FALSE(ig.tcpConnected());
                REQUIRE_FALSE(ig.udpSynced());
            }
        }
    }
}

SCENARIO("IG connect fails when Host is not running", "[integration][sync][connect][failure][HS-connect-fail-no-host]")
{
    GIVEN("an IG initialized without a running Host")
    {
        IgSync ig;
        REQUIRE(ig.initialize(makeIgLocal().udpPortRecv));

        WHEN("the IG connects to a Host endpoint")
        {
            const bool connected = ig.connect(makeIgLocal().target);

            THEN("connect fails and neither plane reports connected")
            {
                REQUIRE_FALSE(connected);
                REQUIRE_FALSE(ig.tcpConnected());
                REQUIRE_FALSE(ig.udpSynced());
            }
        }
    }
}

SCENARIO("IG connects successfully when Host is already waiting", "[integration][sync][connect][HS-connect-ok]")
{
    GIVEN("a Host that has been initialized and is waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE_FALSE(host.hasReadyIg());

        AND_GIVEN("an IG that has been initialized")
        {
            IgSync ig;
            REQUIRE(ig.initialize(makeIgLocal().udpPortRecv));

            WHEN("the IG connects to the Host endpoint")
            {
                const bool connected = ig.connect(makeIgLocal().target);

                THEN("both planes are synced and Host has one ready IG")
                {
                    REQUIRE(connected);
                    REQUIRE(ig.tcpConnected());
                    REQUIRE(ig.udpSynced());
                    REQUIRE(host.hasReadyIg());
                    REQUIRE(host.readyIgCount() == 1);
                }
            }
        }
    }
}

SCENARIO("IG disconnects when Host goes offline", "[integration][sync][connect][disconnect][HS-disconnect-host-offline]")
{
    GIVEN("a connected Host and IG")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal().udpPortRecv));
        REQUIRE(ig.connect(makeIgLocal().target));
        REQUIRE(ig.tcpConnected());
        REQUIRE(ig.udpSynced());

        WHEN("the Host goes offline")
        {
            host.shutdown();

            THEN("IG reports disconnected on both planes")
            {
                REQUIRE_FALSE(ig.tcpConnected());
                REQUIRE_FALSE(ig.udpSynced());
            }
        }
    }
}

SCENARIO("IG connect fails when UDP peer ports are wrong but TCP port is valid",
         "[integration][sync][connect][failure][HS-connect-fail-udp-port]")
{
    GIVEN("a Host waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.initialize(makeHostLocal()));

        AND_GIVEN("an IG initialized with correct local ports")
        {
            IgSync ig;
            REQUIRE(ig.initialize(makeIgLocal().udpPortRecv));

            WHEN("the IG connects using a Host target with wrong UDP ports")
            {
                // Connect 使用 HostTarget::udpPortRecv 作为 Host UDP 收端口。
                IgConfig badUdpConfig{8001, {"127.0.0.1", 8100, 9999}};
                const bool connected = ig.connect(badUdpConfig.target);

                THEN("overall connect fails and neither plane is ready")
                {
                    REQUIRE_FALSE(connected);
                    REQUIRE_FALSE(ig.tcpConnected());
                    REQUIRE_FALSE(ig.udpSynced());
                    REQUIRE_FALSE(host.hasReadyIg());
                }
            }
        }
    }
}

SCENARIO("Host accepts multiple co-located IG connections", "[integration][sync][connect][multi-ig][HS-multi-ig-ready]")
{
    GIVEN("a Host waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.initialize(makeHostLocal()));

        WHEN("two co-located IGs initialize on distinct UDP recv ports and connect")
        {
            IgSync ig1;
            IgSync ig2;
            REQUIRE(ig1.initialize(makeIgLocal(8001).udpPortRecv, 0));
            REQUIRE(ig2.initialize(makeIgLocal(8003).udpPortRecv, 1));

            REQUIRE(ig1.connect(makeIgLocal(8001).target));
            REQUIRE(ig2.connect(makeIgLocal(8003).target));

            THEN("both IGs are synced and Host reports two ready IGs")
            {
                REQUIRE(ig1.tcpConnected());
                REQUIRE(ig1.udpSynced());
                REQUIRE(ig2.tcpConnected());
                REQUIRE(ig2.udpSynced());
                REQUIRE(host.readyIgCount() == 2);
            }
        }
    }
}

SCENARIO("Host snapshot records the channelId from IG HELLO",
         "[acceptance][bdd][platform][PLT-hello-ch]")
{
    GIVEN("a Host waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.initialize(makeHostLocal()));

        AND_GIVEN("an IG initialized as channel 2")
        {
            IgSync ig;
            REQUIRE(ig.initialize(makeIgLocal().udpPortRecv, 2));

            WHEN("the IG connects")
            {
                REQUIRE(ig.connect(makeIgLocal().target));

                THEN("igSnapshot carries channelId 2")
                {
                    REQUIRE(host.readyIgCount() == 1);
                    const auto peers = host.igSnapshot();
                    REQUIRE(peers.size() == 1);
                    REQUIRE(peers.front().channelId == 2);
                }
            }
        }
    }
}

SCENARIO("Host rejects a later IG HELLO that reuses channelId",
         "[acceptance][bdd][platform][PLT-hello-ch]")
{
    GIVEN("a Host with one ready IG on channel 1")
    {
        HostSync host;
        IgSync first;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(first.initialize(makeIgLocal(8001).udpPortRecv, 1));
        REQUIRE(first.connect(makeIgLocal(8001).target));
        REQUIRE(host.readyIgCount() == 1);

        WHEN("a second IG connects with the same channelId")
        {
            IgSync second;
            REQUIRE(second.initialize(makeIgLocal(8003).udpPortRecv, 1));
            const bool connected = second.connect(makeIgLocal(8003).target);

            THEN("the later HELLO is refused and the first peer stays")
            {
                REQUIRE_FALSE(connected);
                REQUIRE_FALSE(second.tcpConnected());
                REQUIRE_FALSE(second.udpSynced());
                REQUIRE(first.tcpConnected());
                REQUIRE(host.readyIgCount() == 1);
                const auto peers = host.igSnapshot();
                REQUIRE(peers.size() == 1);
                REQUIRE(peers.front().channelId == 1);
            }
        }
    }
}

SCENARIO("relayed platform TCP command keeps the original payload",
         "[acceptance][bdd][platform][PLT-tcp-pass]")
{
    GIVEN("a platform Host linked to a virtual IG, and a viewhost Host linked to a real IG")
    {
        HostSync platform;
        IgSync virtualIg;
        HostSync viewhost;
        IgSync realIg;
        REQUIRE(linkHostIg(platform, virtualIg, 36000));
        REQUIRE(linkHostIg(viewhost, realIg, 36200));

        bool igTimeStampValid = true;
        std::string igText;
        realIg.addCallback<CigiIGCtrlV4>([&](const CigiIGCtrlV4& ctrl) {
            igTimeStampValid = ctrl.GetTimeStampValid();
        });
        realIg.addCallback<CigiSymbolTextDefV4>([&](const CigiSymbolTextDefV4& txt) {
            igText = const_cast<CigiSymbolTextDefV4&>(txt).GetText();
        });

        WHEN("the platform sends a TCP command and the relay forwards the framed bytes")
        {
            {
                auto& tcp = platform.outMsgWithIgCtrlTcp();
                CigiSymbolTextDefV4 cmd("pass");
                tcp << cmd;
                platform.flushTcp();
            }

            const auto frames = collectFrames([&] { return virtualIg.takeIncomingTcp(); }, 1);
            REQUIRE_FALSE(frames.empty());
            viewhost.sendTcpMessage(frames.back());

            for (int i = 0; i < 40 && igText.empty(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                realIg.drainIncoming(false);
            }

            THEN("the real IG sees the platform payload and TimeStampValid, not a rewritten header")
            {
                REQUIRE(igText == "pass");
                REQUIRE(igTimeStampValid == false);
            }
        }
    }
}

SCENARIO("relay splits two Host TCP messages glued in one sendAll",
         "[acceptance][bdd][platform][PLT-frame-msg]")
{
    GIVEN("a platform Host linked to a virtual IG, and a viewhost Host linked to a real IG")
    {
        HostSync platform;
        IgSync virtualIg;
        HostSync viewhost;
        IgSync realIg;
        REQUIRE(linkHostIg(platform, virtualIg, 36400));
        REQUIRE(linkHostIg(viewhost, realIg, 36600));

        std::vector<std::uint32_t> igFrames;
        realIg.addCallback<CigiIGCtrlV4>([&](const CigiIGCtrlV4& ctrl) {
            igFrames.push_back(ctrl.GetFrameCntr());
        });

        WHEN("platform sends two complete IGCtrl messages in one TCP write and the relay forwards each framed vector")
        {
            std::vector<unsigned char> first;
            std::vector<unsigned char> second;
            REQUIRE(cigi_wire::packIgCtrl(10, first));
            REQUIRE(cigi_wire::packIgCtrl(11, second));
            platform.sendTcpMessage(concatFrames(first, second));

            const auto frames = collectFrames([&] { return virtualIg.takeIncomingTcp(); }, 2);
            REQUIRE(frames.size() >= 2);
            viewhost.sendTcpMessage(frames[0]);
            viewhost.sendTcpMessage(frames[1]);

            for (int i = 0; i < 40 && igFrames.size() < 2; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                realIg.drainIncoming(false);
            }

            THEN("the real IG receives two separate TCP messages")
            {
                REQUIRE(igFrames.size() == 2);
                REQUIRE(igFrames[0] == 10);
                REQUIRE(igFrames[1] == 11);
            }
        }
    }
}

SCENARIO("relay splits two IG TCP messages glued in one sendAll",
         "[acceptance][bdd][platform][PLT-frame-msg]")
{
    GIVEN("a platform Host linked to a virtual IG, and a viewhost Host linked to a real IG")
    {
        HostSync platform;
        IgSync virtualIg;
        HostSync viewhost;
        IgSync realIg;
        REQUIRE(linkHostIg(platform, virtualIg, 36800));
        REQUIRE(linkHostIg(viewhost, realIg, 37000));

        std::vector<std::uint32_t> platformSof;
        platform.addCallback<CigiSOFV4>([&](const CigiSOFV4& sof) {
            platformSof.push_back(sof.GetFrameCntr());
        });

        WHEN("the real IG sends two complete SOF messages in one TCP write and the relay forwards each framed vector")
        {
            std::vector<unsigned char> first;
            std::vector<unsigned char> second;
            REQUIRE(cigi_wire::packSof(20, first));
            REQUIRE(cigi_wire::packSof(21, second));
            realIg.sendTcpMessage(concatFrames(first, second));

            const auto frames = collectFrames([&] { return viewhost.takeIncomingTcp(); }, 2);
            REQUIRE(frames.size() >= 2);
            virtualIg.sendTcpMessage(frames[0]);
            virtualIg.sendTcpMessage(frames[1]);

            for (int i = 0; i < 40 && platformSof.size() < 2; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                platform.drainIncoming();
            }

            THEN("the platform receives two separate TCP messages")
            {
                REQUIRE(platformSof.size() == 2);
                REQUIRE(platformSof[0] == 20);
                REQUIRE(platformSof[1] == 21);
            }
        }
    }
}

SCENARIO("viewhost TCP weather is a separate message from the relayed platform command",
         "[acceptance][bdd][platform][PLT-splice-cmd]")
{
    GIVEN("a platform Host linked to a virtual IG, and a viewhost Host linked to a real IG")
    {
        HostSync platform;
        IgSync virtualIg;
        HostSync viewhost;
        IgSync realIg;
        REQUIRE(linkHostIg(platform, virtualIg, 37200));
        REQUIRE(linkHostIg(viewhost, realIg, 37400));

        std::vector<std::uint32_t> igFrames;
        std::vector<bool> igTimeStampValid;
        bool gotWeather = false;
        realIg.addCallback<CigiIGCtrlV4>([&](const CigiIGCtrlV4& ctrl) {
            igFrames.push_back(ctrl.GetFrameCntr());
            igTimeStampValid.push_back(ctrl.GetTimeStampValid());
        });
        realIg.addCallback<CigiWeatherCtrlV4>([&](const CigiWeatherCtrlV4&) { gotWeather = true; });

        WHEN("the relay forwards a platform TCP command and viewhost then flushes its own weather TCP")
        {
            {
                auto& tcp = platform.outMsgWithIgCtrlTcp();
                CigiSymbolTextDefV4 cmd("pass");
                tcp << cmd;
                platform.flushTcp();
            }
            const auto frames = collectFrames([&] { return virtualIg.takeIncomingTcp(); }, 1);
            REQUIRE_FALSE(frames.empty());
            viewhost.sendTcpMessage(frames.back());

            {
                auto& tcp = viewhost.outMsgWithIgCtrlTcp();
                CigiWeatherCtrlV4 weather;
                weather.SetSeverity(1);
                tcp << weather;
                viewhost.flushTcp();
            }

            for (int i = 0; i < 40 && (igFrames.size() < 2 || !gotWeather); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                realIg.drainIncoming(false);
            }

            THEN("the IG sees an independent command-plane weather message")
            {
                REQUIRE(igFrames.size() == 2);
                REQUIRE(igTimeStampValid.size() == 2);
                REQUIRE(igTimeStampValid[0] == false);
                REQUIRE(gotWeather);
                REQUIRE(igTimeStampValid[1] == false);
            }
        }
    }
}

SCENARIO("relayed IG TCP report keeps the original payload",
         "[acceptance][bdd][platform][PLT-tcp-return]")
{
    GIVEN("a platform Host linked to a virtual IG, and a viewhost Host linked to a real IG")
    {
        HostSync platform;
        IgSync virtualIg;
        HostSync viewhost;
        IgSync realIg;
        REQUIRE(linkHostIg(platform, virtualIg, 37600));
        REQUIRE(linkHostIg(viewhost, realIg, 37800));

        std::uint16_t platformMsgId = 0;
        std::string platformMsg;
        platform.addCallback<CigiIGMsgV4>([&](const CigiIGMsgV4& msg) {
            platformMsgId = msg.GetMsgID();
            platformMsg = const_cast<CigiIGMsgV4&>(msg).GetMsg();
        });

        WHEN("the real IG reports on TCP and the relay forwards the framed bytes")
        {
            {
                auto& tcp = realIg.outMsgWithSofTcp();
                CigiIGMsgV4 report;
                report.SetMsgID(0x3001);
                report.SetMsg("up");
                tcp << report;
                realIg.flushTcp();
            }

            const auto frames = collectFrames([&] { return viewhost.takeIncomingTcp(); }, 1);
            REQUIRE_FALSE(frames.empty());
            virtualIg.sendTcpMessage(frames.back());

            for (int i = 0; i < 40 && platformMsg.empty(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                platform.drainIncoming();
            }

            THEN("the platform sees the IG message, not a virtual-IG rewrite")
            {
                REQUIRE(platformMsgId == 0x3001);
                REQUIRE(platformMsg == "up");
            }
        }
    }
}

SCENARIO("relayed platform UDP IGCtrl keeps the original header",
         "[acceptance][bdd][platform][PLT-filter-igctrl]")
{
    GIVEN("a platform Host linked to a virtual IG, and a viewhost Host linked to a real IG")
    {
        HostSync platform;
        IgSync virtualIg;
        HostSync viewhost;
        IgSync realIg;
        REQUIRE(linkHostIg(platform, virtualIg, 38000));
        REQUIRE(linkHostIg(viewhost, realIg, 38200));

        std::uint32_t igFrameCntr = 0xffffffffu;
        std::uint32_t igTimeStamp = 0;
        bool igTimeStampValid = false;
        realIg.addCallback<CigiIGCtrlV4>([&](const CigiIGCtrlV4& ctrl) {
            igFrameCntr = ctrl.GetFrameCntr();
            igTimeStamp = ctrl.GetTimeStamp();
            igTimeStampValid = ctrl.GetTimeStampValid();
        });

        WHEN("the platform sends UDP IGCtrl after two earlier frames and the relay forwards the datagram")
        {
            for (int i = 0; i < 3; ++i)
            {
                platform.outMsgWithIgCtrlUdp();
                platform.flushUdp();
            }

            const auto dgrams = collectFrames([&] { return virtualIg.takeIncomingUdp(); }, 3);
            REQUIRE(dgrams.size() >= 3);
            const auto& last = dgrams.back();
            CigiIGCtrlV4 sent;
            REQUIRE(sent.Unpack(const_cast<unsigned char*>(last.data()), false, nullptr) >= 0);
            viewhost.sendUdpMessage(last);

            for (int i = 0; i < 40 && igFrameCntr == 0xffffffffu; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                realIg.drainIncoming(false);
            }

            THEN("the real IG sees the platform data-plane FrameCntr and TimeStamp, and viewhost did not add another IGCtrl")
            {
                REQUIRE(igFrameCntr == sent.GetFrameCntr());
                REQUIRE(igTimeStamp == sent.GetTimeStamp());
                REQUIRE(igTimeStampValid);
                REQUIRE(realIg.igCtrlReceivedCount() == 1);
            }
        }
    }
}

SCENARIO("virtual IG packSof after UDP forward is the only SOF the platform sees",
         "[acceptance][bdd][platform][PLT-relay-sof]")
{
    GIVEN("a platform Host linked to a virtual IG, and a viewhost Host linked to a real IG")
    {
        HostSync platform;
        IgSync virtualIg;
        HostSync viewhost;
        IgSync realIg;
        REQUIRE(linkHostIg(platform, virtualIg, 38400));
        REQUIRE(linkHostIg(viewhost, realIg, 38600));

        std::uint32_t platformSof = 0xffffffffu;
        platform.addCallback<CigiSOFV4>([&](const CigiSOFV4& sof) { platformSof = sof.GetFrameCntr(); });

        WHEN("the relay forwards one platform UDP IGCtrl, echos SOF from the virtual IG, and the real IG replies SOF to viewhost")
        {
            for (int i = 0; i < 3; ++i)
            {
                platform.outMsgWithIgCtrlUdp();
                platform.flushUdp();
            }
            const auto dgrams = collectFrames([&] { return virtualIg.takeIncomingUdp(); }, 3);
            REQUIRE(dgrams.size() >= 3);
            const auto& last = dgrams.back();
            CigiIGCtrlV4 sent;
            REQUIRE(sent.Unpack(const_cast<unsigned char*>(last.data()), false, nullptr) >= 0);
            viewhost.sendUdpMessage(last);
            virtualIg.sendSofForIgCtrl(last);

            for (int i = 0; i < 40 && realIg.igCtrlReceivedCount() == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                realIg.drainIncoming(true);
            }
            for (int i = 0; i < 40 && platform.sofReceivedCount() == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                platform.drainIncoming();
            }
            for (int i = 0; i < 40 && viewhost.sofReceivedCount() == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                viewhost.drainIncoming();
            }

            THEN("the platform receives one matching SOF from the virtual IG, not the real IG SOF")
            {
                REQUIRE(platform.sofReceivedCount() == 1);
                REQUIRE(platformSof == sent.GetFrameCntr());
                REQUIRE(viewhost.sofReceivedCount() == 1);
            }
        }
    }
}

SCENARIO("relay forwards multiple platform TCP and UDP messages to the IG",
         "[acceptance][bdd][platform][PLT-multi-down]")
{
    GIVEN("a platform Host linked to a virtual IG, and a viewhost Host linked to a real IG")
    {
        HostSync platform;
        IgSync virtualIg;
        HostSync viewhost;
        IgSync realIg;
        REQUIRE(linkHostIg(platform, virtualIg, 38800));
        REQUIRE(linkHostIg(viewhost, realIg, 39000));

        std::vector<std::string> igTexts;
        std::vector<std::uint32_t> igUdpFrames;
        realIg.addCallback<CigiSymbolTextDefV4>([&](const CigiSymbolTextDefV4& txt) {
            igTexts.push_back(const_cast<CigiSymbolTextDefV4&>(txt).GetText());
        });
        realIg.addCallback<CigiIGCtrlV4>([&](const CigiIGCtrlV4& ctrl) {
            if (ctrl.GetTimeStampValid())
                igUdpFrames.push_back(ctrl.GetFrameCntr());
        });

        WHEN("the platform sends two TCP commands and two UDP IGCtrl and the relay forwards each")
        {
            auto sendTcpText = [&](const char* text) {
                auto& tcp = platform.outMsgWithIgCtrlTcp();
                CigiSymbolTextDefV4 cmd(text);
                tcp << cmd;
                platform.flushTcp();
            };
            sendTcpText("tcp-0");
            sendTcpText("tcp-1");
            platform.outMsgWithIgCtrlUdp();
            platform.flushUdp();
            platform.outMsgWithIgCtrlUdp();
            platform.flushUdp();

            const auto tcpFrames = collectFrames([&] { return virtualIg.takeIncomingTcp(); }, 2);
            REQUIRE(tcpFrames.size() >= 2);
            viewhost.sendTcpMessage(tcpFrames[0]);
            viewhost.sendTcpMessage(tcpFrames[1]);

            const auto udpFrames = collectFrames([&] { return virtualIg.takeIncomingUdp(); }, 2);
            REQUIRE(udpFrames.size() >= 2);
            viewhost.sendUdpMessage(udpFrames[0]);
            viewhost.sendUdpMessage(udpFrames[1]);

            for (int i = 0; i < 40 && (igTexts.size() < 2 || igUdpFrames.size() < 2); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                realIg.drainIncoming(false);
            }

            THEN("the real IG receives every TCP payload and every UDP data-plane IGCtrl in order")
            {
                REQUIRE(igTexts.size() == 2);
                REQUIRE(igTexts[0] == "tcp-0");
                REQUIRE(igTexts[1] == "tcp-1");
                REQUIRE(igUdpFrames.size() == 2);
                REQUIRE(igUdpFrames[0] == 0);
                REQUIRE(igUdpFrames[1] == 1);
            }
        }
    }
}

SCENARIO("relay forwards multiple IG TCP reports; IG UDP SOF stays on viewhost",
         "[acceptance][bdd][platform][PLT-multi-up]")
{
    GIVEN("a platform Host linked to a virtual IG, and a viewhost Host linked to a real IG")
    {
        HostSync platform;
        IgSync virtualIg;
        HostSync viewhost;
        IgSync realIg;
        REQUIRE(linkHostIg(platform, virtualIg, 39200));
        REQUIRE(linkHostIg(viewhost, realIg, 39400));

        std::vector<std::pair<std::uint16_t, std::string>> platformMsgs;
        platform.addCallback<CigiIGMsgV4>([&](const CigiIGMsgV4& msg) {
            platformMsgs.emplace_back(msg.GetMsgID(), const_cast<CigiIGMsgV4&>(msg).GetMsg());
        });

        WHEN("the real IG sends two TCP reports and two UDP SOF, and the relay forwards only TCP")
        {
            auto sendIgTcp = [&](std::uint16_t msgId, const char* text) {
                auto& tcp = realIg.outMsgWithSofTcp();
                CigiIGMsgV4 report;
                report.SetMsgID(msgId);
                report.SetMsg(text);
                tcp << report;
                realIg.flushTcp();
            };
            sendIgTcp(0x3001, "up-0");
            sendIgTcp(0x3002, "up-1");

            const auto tcpFrames = collectFrames([&] { return viewhost.takeIncomingTcp(); }, 2);
            REQUIRE(tcpFrames.size() >= 2);
            virtualIg.sendTcpMessage(tcpFrames[0]);
            virtualIg.sendTcpMessage(tcpFrames[1]);

            for (int i = 0; i < 40 && platformMsgs.size() < 2; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                platform.drainIncoming();
            }
            const auto platformSofAfterTcp = platform.sofReceivedCount();

            realIg.outMsgWithSofUdp();
            realIg.flushUdp();
            realIg.outMsgWithSofUdp();
            realIg.flushUdp();
            for (int i = 0; i < 40 && viewhost.sofReceivedCount() < 2; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                viewhost.drainIncoming();
            }
            platform.drainIncoming();

            THEN("the platform receives both TCP reports and none of the real IG UDP SOF")
            {
                REQUIRE(platformMsgs.size() == 2);
                REQUIRE(platformMsgs[0].first == 0x3001);
                REQUIRE(platformMsgs[0].second == "up-0");
                REQUIRE(platformMsgs[1].first == 0x3002);
                REQUIRE(platformMsgs[1].second == "up-1");
                REQUIRE(viewhost.sofReceivedCount() == 2);
                REQUIRE(platform.sofReceivedCount() == platformSofAfterTcp);
            }
        }
    }
}

TEST_CASE("IG HELLO on TCP starts with CIGI SOF", "[unit][sync][wire-contract][HS-hello-cigi]")
{
    TcpSocket listener;
    REQUIRE(listener.listen(0));
    const int tcpPort = listener.localPort();
    REQUIRE(tcpPort > 0);

    IgSync ig;
    REQUIRE(ig.initialize(19117, 2));

    std::thread connecting([&] { ig.connect({"127.0.0.1", tcpPort, 19118}); });

    TcpSocket accepted;
    for (int i = 0; i < 100 && !listener.accept(accepted); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    REQUIRE(accepted.valid());

    unsigned char header[4]{};
    REQUIRE(accepted.recvAll(header, 4, 1000));
    accepted.close();
    listener.close();
    connecting.join();

    const std::uint16_t packetId = static_cast<std::uint16_t>(header[2] | (header[3] << 8));
    REQUIRE(packetId == CIGI_SOF_PACKET_ID_V4);
}

TEST_CASE("IgSync sendUdpMessage forwards raw datagram without adding SOF",
          "[unit][sync][CIGI-endpoint-udp]")
{
    HostSync host;
    IgSync ig;
    REQUIRE(linkHostIg(host, ig, 39800));

    host.drainIncoming();
    const auto sofBefore = host.sofReceivedCount();
    const auto sentBefore = ig.sofSentCount();

    std::vector<unsigned char> sof;
    REQUIRE(cigi_wire::packSof(42, sof));
    SyncInterface& sync = ig;
    // UDP 可丢：重发直到 Host 解到 SOF；合同不是单报必达。
    for (int i = 0; i < 10 && host.sofReceivedCount() == sofBefore; ++i)
    {
        sync.sendUdpMessage(sof);
        for (int j = 0; j < 8 && host.sofReceivedCount() == sofBefore; ++j)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            host.drainIncoming();
        }
    }

    REQUIRE(ig.sofSentCount() == sentBefore);
    if (host.sofReceivedCount() == sofBefore)
        SKIP("UDP datagram dropped");
    REQUIRE(host.sofReceivedCount() > sofBefore);
}

TEST_CASE("HostSync takeIncomingUdp consumes the UDP queue without drainIncoming",
          "[unit][sync][CIGI-endpoint-udp]")
{
    HostSync host;
    IgSync ig;
    REQUIRE(linkHostIg(host, ig, 39900));

    host.drainIncoming();
    const auto sofBefore = host.sofReceivedCount();

    SyncInterface& sync = host;
    std::vector<unsigned char> taken;
    for (int i = 0; i < 10 && taken.empty(); ++i)
    {
        ig.outMsgWithSofUdp();
        ig.flushUdp();
        const auto frames = collectFrames([&] { return sync.takeIncomingUdp(); }, 1);
        if (!frames.empty())
            taken = frames.back();
    }
    if (taken.empty())
        SKIP("UDP datagram dropped");
    REQUIRE(cigi_wire::isSofPacket(taken.data(), static_cast<int>(taken.size())));

    // 抽空可能迟到的报，避免随后 drainIncoming 把它们计成 SOF。
    for (int i = 0; i < 10; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        (void)sync.takeIncomingUdp();
    }

    host.drainIncoming();
    REQUIRE(host.sofReceivedCount() == sofBefore);
}

// =============================================================================
// 2. 帧节拍：CIGI IGCtrl / SOF / FreeRun（集成；握手为 CIGI HELLO + UDP_SYNC）
// 数据面线格式契约：IGCtrlV4 (+ 可选 EntityPositionCtrlV4) / SOFV4 —— 见 [wire-contract]。
// =============================================================================

SCENARIO("connected Host and IG enter RUNNING and exchange CIGI IGCtrl each update",
         "[integration][sync][status][cigi][CIGI-running-igctrl]")
{
    GIVEN("a Host and an IG that have completed CIGI handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal().udpPortRecv));
        REQUIRE(ig.connect(makeIgLocal().target));

        WHEN("Host runs and sends 10 CIGI IGCtrl frames while IG updates")
        {
            host.run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                hostSendFrame(host, i * (1000.0 / 60.0));
                ig.drainIncoming();
                ig.update();
            }

            THEN("both are RUNNING and Host sent one IGCtrl per Update")
            {
                REQUIRE(host.status() == HostStatus::RUNNING);
                REQUIRE(ig.status() == IgStatus::RUNNING);
                REQUIRE(host.igCtrlSentCount() == kFrames);
                REQUIRE(approxAtMost(ig.igCtrlReceivedCount(), kFrames, 3));
            }
        }
    }
}

SCENARIO("Host records a matched IGCtrl-SOF RTT sample",
         "[integration][sync][meas][MEAS-sof-rtt]")
{
    GIVEN("a Host and an IG that have completed CIGI handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal().udpPortRecv));
        REQUIRE(ig.connect(makeIgLocal().target));

        WHEN("Host sends IGCtrl frames and IG replies SOF")
        {
            host.run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                hostSendFrame(host, i * (1000.0 / 60.0));
                ig.drainIncoming(/*sendSof=*/true);
                ig.update();
            }

            THEN("the ready IG has a positive matched RTT sample")
            {
                host.drainIncoming();
                const auto peers = host.igSnapshot();
                REQUIRE_FALSE(peers.empty());
                const auto last = host.sofRttLast(peers.front().id);
                REQUIRE(last.has_value());
                REQUIRE(*last > std::chrono::microseconds{0});
                REQUIRE(peers.front().lastRtt == last);
                REQUIRE(peers.front().sofAge.has_value());
                REQUIRE(*peers.front().sofAge >= std::chrono::microseconds{0});
            }
        }
    }
}

SCENARIO("IG replies with one CIGI SOF per received IGCtrl", "[integration][sync][status][sof][cigi][CIGI-sof-echo]")
{
    GIVEN("a Host and an IG that have completed CIGI handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal().udpPortRecv));
        REQUIRE(ig.connect(makeIgLocal().target));

        WHEN("Host sends 10 CIGI IGCtrl and IG updates each frame (reply SOF)")
        {
            host.run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                hostSendFrame(host, i * (1000.0 / 60.0));
                ig.drainIncoming(/*sendSof=*/true);
                ig.update();
            }

            THEN("SOF sent equals IGCtrl received; Host SOF count cannot exceed what IG sent")
            {
                host.drainIncoming();
                REQUIRE(ig.igCtrlReceivedCount() >= 1);
                REQUIRE(ig.sofSentCount() == ig.igCtrlReceivedCount());
                REQUIRE(host.sofReceivedCount() <= ig.sofSentCount());
            }
        }
    }
}

SCENARIO("Host keeps sending CIGI IGCtrl when IG never replies SOF",
         "[integration][sync][status][freerun][cigi][CIGI-freerun]")
{
    GIVEN("a connected Host and IG (send is never gated by SOF)")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal().udpPortRecv));
        REQUIRE(ig.connect(makeIgLocal().target));

        WHEN("Host sends 10 CIGI IGCtrl while IG receives but never replies SOF")
        {
            host.run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                hostSendFrame(host, i * (1000.0 / 60.0));
                ig.drainIncoming(/*sendSof=*/false);
                ig.update();
            }

            THEN("Host sent all IGCtrl without depending on SOF")
            {
                host.drainIncoming();
                REQUIRE(host.igCtrlSentCount() == kFrames);
                REQUIRE(host.sofReceivedCount() == 0);
                REQUIRE(ig.igCtrlReceivedCount() <= kFrames);
            }
        }
    }
}

SCENARIO("IG last received CIGI FrameCntr matches Host frame numbers",
         "[integration][sync][status][frame][cigi][CIGI-frame-cntr]")
{
    GIVEN("a Host and an IG that have completed CIGI handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal().udpPortRecv));
        REQUIRE(ig.connect(makeIgLocal().target));

        WHEN("Host sends CIGI IGCtrl FrameCntr 0..N-1 and IG updates each frame")
        {
            host.run();
            constexpr int kFrames = 10;
            std::uint32_t prevReceived = 0;
            int matchedFrames = 0;

            for (int i = 0; i < kFrames; ++i)
            {
                // Host 本轮 CIGI IGCtrl.FrameCntr == i（经 HostSync API 暴露为 lastIgCtrlFrameCntr）
                hostSendFrame(host, i * (1000.0 / 60.0));
                ig.drainIncoming();
                ig.update();

                if (ig.igCtrlReceivedCount() > prevReceived)
                {
                    // 异步 I/O 线程 + 队列下，一次 update 可能批量处理多条积压帧，
                    // lastIgCtrlFrameCntr 是最新帧号——断言其不超前于本帧已发送的帧号（方向不变）。
                    REQUIRE(ig.lastIgCtrlFrameCntr() <= static_cast<std::uint32_t>(i));
                    prevReceived = ig.igCtrlReceivedCount();
                    ++matchedFrames;
                }
            }

            THEN("at least one IGCtrl was received and FrameCntr values matched")
            {
                REQUIRE(matchedFrames >= 1);
                REQUIRE(ig.lastIgCtrlFrameCntr() < static_cast<std::uint32_t>(kFrames));
            }
        }
    }
}

// =============================================================================
// 3. Engine + SynchronSystem 集成（CIGI 帧交换契约；握手为 CIGI HELLO + UDP_SYNC）
// =============================================================================

SCENARIO("IG exchanges CIGI frame control with an independent Host over ticks",
         "[integration][sync][engine][cigi][CIGI-engine-ticks]")
{
    GIVEN("an offscreen IG-only Engine and an independent HostSync")
    {
        constexpr int kBase = 23000;
        TestHost host;
        REQUIRE(host.init(kBase));
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;
        REQUIRE(engine.initSync(makeTestIgConfig(kBase + 1, kBase)));
        REQUIRE(host.sync.readyIgCount() == 1);

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";

        WHEN("the Host sends frame control and the IG ticks 10 times")
        {
            REQUIRE(engine.initGraphics(modelPath));

            constexpr int kTicks = 10;
            for (int i = 0; i < kTicks; ++i)
            {
                hostSendFrame(host.sync, i * 16.667);
                engine.tickSync();
            }

            THEN("Host and IG exchanged CIGI IGCtrl/SOF")
            {
                SynchronSystem& sync = engine.synchronSystem();
                HostSync& hostRef = host.sync;
                IgSync& ig = sync.igSync();

                hostRef.drainIncoming();
                REQUIRE(hostRef.igCtrlSentCount() == kTicks);
                REQUIRE(approxAtMost(ig.igCtrlReceivedCount(), kTicks, 3));
                REQUIRE(ig.sofSentCount() == ig.igCtrlReceivedCount());
                REQUIRE(hostRef.sofReceivedCount() <= ig.sofSentCount());
                REQUIRE(approxAtMost(hostRef.sofReceivedCount(), kTicks, 3));
            }
        }
    }
}

SCENARIO("three IG Engines exchange CIGI frame control across one independent Host",
         "[integration][sync][engine][multi-ig][cigi][CIGI-multi-ig]")
{
    GIVEN("one independent HostSync and three IG-only Engines A/B/C on distinct UDP ports")
    {
        constexpr int kBase = 24000;
        TestHost host;
        REQUIRE(host.init(kBase));
        Engine engineA;
        Engine engineB;
        Engine engineC;
        engineA.extent = engineB.extent = engineC.extent = {1920, 1080};
        engineA.showWindow = engineB.showWindow = engineC.showWindow = false;

        REQUIRE(engineA.initSync(makeTestIgConfig(kBase + 1, kBase), makeTestSyncSystem(0)));
        REQUIRE(engineB.initSync(makeTestIgConfig(kBase + 3, kBase), makeTestSyncSystem(1)));
        REQUIRE(engineC.initSync(makeTestIgConfig(kBase + 5, kBase), makeTestSyncSystem(2)));
        REQUIRE(host.sync.readyIgCount() == 3);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));

        WHEN("the Host leads 10 ticks to all three IGs")
        {
            constexpr int kTicks = 10;
            constexpr int kIgCount = 3;
            for (int i = 0; i < kTicks; ++i)
            {
                hostSendFrame(host.sync, i * 16.667);
                engineA.tickSync();
                engineB.tickSync();
                engineC.tickSync();
            }

            THEN("each IG got about N CIGI IGCtrl and Host got about N times 3 SOF")
            {
                HostSync& hostRef = host.sync;
                IgSync& igA = engineA.synchronSystem().igSync();
                IgSync& igB = engineB.synchronSystem().igSync();
                IgSync& igC = engineC.synchronSystem().igSync();

                hostRef.drainIncoming();
                REQUIRE(hostRef.igCtrlSentCount() == kTicks);
                REQUIRE(approxAtMost(igA.igCtrlReceivedCount(), kTicks, 3));
                REQUIRE(approxAtMost(igB.igCtrlReceivedCount(), kTicks, 3));
                REQUIRE(approxAtMost(igC.igCtrlReceivedCount(), kTicks, 3));
                REQUIRE(igA.sofSentCount() == igA.igCtrlReceivedCount());
                REQUIRE(igB.sofSentCount() == igB.igCtrlReceivedCount());
                REQUIRE(igC.sofSentCount() == igC.igCtrlReceivedCount());
                REQUIRE(approxAtMost(hostRef.sofReceivedCount(), kTicks * kIgCount, 9));
            }
        }
    }
}

// =============================================================================
// 4. Host 控制 IG 相机位姿
// 约定：已连接时最终位姿 = Host 眼点 ⊕ 本地 offsetDeg。
// 分层：应用契约用注入测门控/合成/无新包；E2E 钉真报文（注入是测试手法，不写进故事标题）。
// =============================================================================

namespace
{
    // R = Rz*Rx*Ry，通过依次作用轴四元数（VSG 四元数乘法是 reverse-Hamilton）。
    vsg::dvec3 rotateByEulerYprDeg(const vsg::dvec3& eulerYprDeg, const vsg::dvec3& v)
    {
        const vsg::dvec3 afterRoll =
            vsg::dquat(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0)) * v;
        const vsg::dvec3 afterPitch =
            vsg::dquat(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) * afterRoll;
        return vsg::dquat(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0)) * afterPitch;
    }

    vsg::dquat quatFromEulerYprDeg(const vsg::dvec3& eulerYprDeg)
    {
        // VSG(a*b)=Hamilton(b*a) ⇒ 写 Ry*Rx*Rz 得到 Hamilton 的 Rz*Rx*Ry。
        return vsg::dquat(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0)) *
               vsg::dquat(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) *
               vsg::dquat(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0));
    }

    void requireLookAtMatchesPose(Engine& engine, const vsg::dvec3& position, const vsg::dvec3& eulerYprDeg)
    {
        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);

        const vsg::dvec3 expectedForward = rotateByEulerYprDeg(eulerYprDeg, vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dvec3 expectedUp = rotateByEulerYprDeg(eulerYprDeg, vsg::dvec3(0.0, 0.0, 1.0));
        const vsg::dvec3 expectedCenter = position + expectedForward;

        REQUIRE(vsg::length(lookAt->eye - position) < 1e-9);
        REQUIRE(vsg::length(lookAt->center - expectedCenter) < 1e-9);
        REQUIRE(vsg::length(vsg::normalize(lookAt->up) - vsg::normalize(expectedUp)) < 1e-9);
    }

    // Rotate ENU direction by orthonormalized LocalToWorld columns (lla设计 §3.3).
    vsg::dvec3 rotateEnuToEcef(const vsg::dmat4& localToWorld, const vsg::dvec3& enuDir)
    {
        const vsg::dvec3 east = vsg::normalize(vsg::dvec3(localToWorld(0, 0), localToWorld(0, 1), localToWorld(0, 2)));
        const vsg::dvec3 north = vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
        const vsg::dvec3 upAxis = vsg::normalize(vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
        return enuDir.x * east + enuDir.y * north + enuDir.z * upAxis;
    }

    void requireLookAtMatchesLlaPose(Engine& engine, const vsg::EllipsoidModel& ellipsoid, const vsg::dvec3& lla,
                                     const vsg::dvec3& eulerYprDeg, double eyeEps = 1e-6, double dirEps = 1e-9)
    {
        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);

        constexpr double kLookDistance = 1.0;
        const vsg::dvec3 forwardEnu = rotateByEulerYprDeg(eulerYprDeg, vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dvec3 upEnu = rotateByEulerYprDeg(eulerYprDeg, vsg::dvec3(0.0, 0.0, 1.0));
        const vsg::dmat4 localToWorld = ellipsoid.computeLocalToWorldTransform(lla);

        const vsg::dvec3 expectedEye = ellipsoid.convertLatLongAltitudeToECEF(lla);
        const vsg::dvec3 expectedForward = vsg::normalize(rotateEnuToEcef(localToWorld, forwardEnu));
        const vsg::dvec3 expectedUp = vsg::normalize(rotateEnuToEcef(localToWorld, upEnu));
        const vsg::dvec3 expectedCenter = expectedEye + expectedForward * kLookDistance;

        REQUIRE(vsg::length(lookAt->eye - expectedEye) < eyeEps);
        REQUIRE(vsg::length(lookAt->center - expectedCenter) < eyeEps);
        REQUIRE(vsg::length(vsg::normalize(lookAt->up) - expectedUp) < dirEps);
        REQUIRE(vsg::length(vsg::normalize(lookAt->center - lookAt->eye) - expectedForward) < dirEps);
    }

    // §3.3 写路径的逆（lla设计 §3.5）：ECEF LookAt → LLA + 当地 ENU YPR。
    bool sampleLookAtToLlaYpr(const vsg::LookAt& lookAt, const vsg::EllipsoidModel& ellipsoid,
                              vsg::dvec3& llaOut, vsg::dvec3& eulerYprDegOut)
    {
        llaOut = ellipsoid.convertECEFToLatLongAltitude(lookAt.eye);
        const vsg::dvec3 forwardEcef = vsg::normalize(lookAt.center - lookAt.eye);
        if (vsg::length(forwardEcef) < 1e-12)
            return false;

        // 与 Engine/SynchronSystem 一致：ENU 经 LocalToWorld 列（§3.3 写的逆）。
        const vsg::dmat4 localToWorld = ellipsoid.computeLocalToWorldTransform(llaOut);
        const vsg::dvec3 east = vsg::normalize(vsg::dvec3(localToWorld(0, 0), localToWorld(0, 1), localToWorld(0, 2)));
        const vsg::dvec3 north = vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
        const vsg::dvec3 upAxis = vsg::normalize(vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
        const auto toEnu = [&](const vsg::dvec3& ecefDir) {
            return vsg::normalize(
                vsg::dvec3(vsg::dot(ecefDir, east), vsg::dot(ecefDir, north), vsg::dot(ecefDir, upAxis)));
        };

        const vsg::dvec3 forward = toEnu(forwardEcef);
        const vsg::dvec3 up = toEnu(vsg::normalize(lookAt.up));

        constexpr double kPi = 3.14159265358979323846;
        const auto rad2deg = [](double r) { return r * (180.0 / kPi); };
        const auto clampd = [](double v, double lo, double hi) {
            return v < lo ? lo : (v > hi ? hi : v);
        };

        const double yawRad = std::atan2(-forward.x, forward.y);
        const double pitchRad = std::asin(clampd(forward.z, -1.0, 1.0));
        const vsg::dvec3 afterPitchUp =
            vsg::dquat(pitchRad, vsg::dvec3(1.0, 0.0, 0.0)) * vsg::dvec3(0.0, 0.0, 1.0);
        const vsg::dvec3 afterPitchRight =
            vsg::dquat(pitchRad, vsg::dvec3(1.0, 0.0, 0.0)) * vsg::dvec3(1.0, 0.0, 0.0);
        const vsg::dvec3 expectedUp =
            vsg::normalize(vsg::dquat(yawRad, vsg::dvec3(0.0, 0.0, 1.0)) * afterPitchUp);
        const vsg::dvec3 expectedRight =
            vsg::normalize(vsg::dquat(yawRad, vsg::dvec3(0.0, 0.0, 1.0)) * afterPitchRight);
        const double rollRad = std::atan2(vsg::dot(up, expectedRight), vsg::dot(up, expectedUp));

        eulerYprDegOut = vsg::dvec3(rad2deg(yawRad), rad2deg(pitchRad), rad2deg(rollRad));
        return true;
    }

    void requireLlaYprNear(const vsg::dvec3& actualLla, const vsg::dvec3& actualYpr,
                           const vsg::dvec3& expectedLla, const vsg::dvec3& expectedYpr,
                           double llaEps = 1e-6, double yprEps = 1e-4)
    {
        REQUIRE(std::abs(actualLla.x - expectedLla.x) < llaEps);
        REQUIRE(std::abs(actualLla.y - expectedLla.y) < llaEps);
        REQUIRE(std::abs(actualLla.z - expectedLla.z) < llaEps);
        REQUIRE(std::abs(actualYpr.x - expectedYpr.x) < yprEps);
        REQUIRE(std::abs(actualYpr.y - expectedYpr.y) < yprEps);
        REQUIRE(std::abs(actualYpr.z - expectedYpr.z) < yprEps);
    }

    void requirePoseNear(const ChannelEye& actual, const ChannelEye& expected, double eps = 1e-6)
    {
        REQUIRE(vsg::length(actual.lla - expected.lla) < eps);
        REQUIRE(vsg::length(actual.eulerYprDeg - expected.eulerYprDeg) < eps);
    }

    // Host 眼点用例使用独立端口，避免与 §1–3 默认 8000/8001 并行冲突。
    IgConfig makeIgLocalEye(int udpRecvPort, int base = 18000)
    {
        return IgConfig{udpRecvPort, {"127.0.0.1", base + 100, base}};
    }

    IgConfig makeIgOnlyRole(int igUdpRecv, int base = 18000)
    {
        return makeIgLocalEye(igUdpRecv, base);
    }
} // namespace

// -----------------------------------------------------------------------------
// 4.1 位姿 API 标尺（单元，非验收）
// -----------------------------------------------------------------------------

TEST_CASE("setCameraPose writes LookAt from position and euler YPR", "[unit][camera][CAM-lookat-ypr]")
{
    Engine engine;
    engine.extent = {1920, 1080};
    engine.showWindow = false;

    const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
    REQUIRE(engine.init(modelPath));

    auto camera = engine.mainCamera();
    REQUIRE(camera);
    REQUIRE(camera->viewMatrix.cast<vsg::LookAt>());

    const vsg::dvec3 position{10.0, -20.0, 5.0};
    const vsg::dvec3 eulerYprDeg{90.0, 0.0, 0.0}; // yaw 90° about Z
    REQUIRE(engine.setCameraPose(position, eulerYprDeg));
    requireLookAtMatchesPose(engine, position, eulerYprDeg);
}

// lla位姿传输设计.md §3.3 / §4.1 / §7：有 EllipsoidModel 时 LLA+当地 YPR → ECEF LookAt。
TEST_CASE("setCameraPoseLla writes ECEF LookAt from LLA and local ENU YPR", "[unit][camera][lla][CAM-lookat-lla]")
{
    Engine engine;
    engine.extent = {1920, 1080};
    engine.showWindow = false;

    // 模型内嵌 EllipsoidModel（无需 injectEllipsoidIfMissing）。
    const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
    engine.config.injectEllipsoidIfMissing = true;
    REQUIRE(engine.init(modelPath));

    auto camera = engine.mainCamera();
    REQUIRE(camera);
    auto ellipsoidPerspective = camera->projectionMatrix.cast<vsg::EllipsoidPerspective>();
    REQUIRE(ellipsoidPerspective);
    REQUIRE(ellipsoidPerspective->ellipsoidModel);

    // 设计 §6 的中纬默认，取非零 yaw/pitch 以覆盖 ENU→ECEF。
    const vsg::dvec3 lla{39.9, 116.4, 500.0};
    const vsg::dvec3 eulerYprDeg{45.0, 10.0, 0.0};
    REQUIRE(engine.setCameraPoseLla(lla, eulerYprDeg));
    requireLookAtMatchesLlaPose(engine, *ellipsoidPerspective->ellipsoidModel, lla, eulerYprDeg);
}

// lla位姿传输设计.md §3.5 / §7：LLA 本机往返（单机、无网络）。
TEST_CASE("setCameraPoseLla round-trips LLA and local YPR on one engine", "[unit][camera][lla][roundtrip][CAM-lla-roundtrip]")
{
    Engine engine;
    engine.extent = {1920, 1080};
    engine.showWindow = false;

    const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
    engine.config.injectEllipsoidIfMissing = true;
    REQUIRE(engine.init(modelPath));

    auto ellipsoidPerspective = engine.mainCamera()->projectionMatrix.cast<vsg::EllipsoidPerspective>();
    REQUIRE(ellipsoidPerspective);
    REQUIRE(ellipsoidPerspective->ellipsoidModel);
    const auto& ellipsoid = *ellipsoidPerspective->ellipsoidModel;

    const vsg::dvec3 lla{39.9, 116.4, 500.0};
    // Roll=0：仅 forward 的 yaw/pitch 提取对 Rz*Rx*Ry 精确；非零 roll 由 LookAt 向量测试覆盖。
    const vsg::dvec3 eulerYprDeg{45.0, 10.0, 0.0};
    REQUIRE(engine.setCameraPoseLla(lla, eulerYprDeg));

    auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
    REQUIRE(lookAt);

    vsg::dvec3 sampledLla{};
    vsg::dvec3 sampledYpr{};
    REQUIRE(sampleLookAtToLlaYpr(*lookAt, ellipsoid, sampledLla, sampledYpr));
    requireLlaYprNear(sampledLla, sampledYpr, lla, eulerYprDeg);
}

// -----------------------------------------------------------------------------
// 4.2 未连接也会应用 / 已连接覆盖（验收行为；注入仅作测试手段）
// -----------------------------------------------------------------------------

SCENARIO("unlinked IG still applies queued Host eye to the camera",
         "[acceptance][bdd][sync][hostctrl][gate][LLA-unlinked-apply]")
{
    GIVEN("an Engine with graphics whose IG is not linked to a Host")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        engine.config.injectEllipsoidIfMissing = true;
        REQUIRE(engine.init(modelPath));

        const IgConfig igCfg = makeIgLocalEye(18001, 18000);
        // Host 未启动 → Connect 失败，但仍完成本地 Init。
        REQUIRE(engine.synchronSystem().initialize(igCfg, SyncSystemConfig{/*requireConnectedIg=*/false}));
        REQUIRE_FALSE(engine.synchronSystem().igLinked());

        const ChannelEye localPose{{39.9, 116.4, 400.0}, {10.0, 0.0, 0.0}};
        const ChannelEye hostPose{{39.9, 116.4, 500.0}, {45.0, 0.0, 0.0}};
        REQUIRE(engine.setCameraPoseLla(localPose.lla, localPose.eulerYprDeg));

        WHEN("a Host eye is queued and sync update runs")
        {
            // 未连接也会应用：收包即合成，update 写相机。
            engine.cameraDriver().compose(hostPose);
            engine.stepSync();

            THEN("camera matches the Host eye")
            {
                requireLookAtMatchesLlaPose(engine, *engine.ellipsoidModel(), hostPose.lla, hostPose.eulerYprDeg);
            }
        }
    }
}

SCENARIO("linked IG applies Host eye to the camera", "[acceptance][bdd][sync][hostctrl][gate][LLA-linked-apply]")
{
    GIVEN("an IG Engine linked to an independent Host")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        TestHost host;
        REQUIRE(host.init(18000));
        REQUIRE(engine.init(modelPath, makeIgOnlyRole(18001, 18000)));
        REQUIRE(engine.synchronSystem().igLinked());

        const ChannelEye localPose{{39.9, 116.4, 400.0}, {10.0, 0.0, 0.0}};
        const ChannelEye hostPose{{39.9, 116.4, 500.0}, {45.0, 0.0, 0.0}};
        REQUIRE(engine.setCameraPoseLla(localPose.lla, localPose.eulerYprDeg));

        WHEN("a Host eye becomes available and sync update runs")
        {
            engine.cameraDriver().setOffsetDeg({});
            engine.cameraDriver().compose(hostPose);
            engine.stepSync();

            THEN("camera matches the Host eye")
            {
                requireLookAtMatchesLlaPose(engine, *engine.ellipsoidModel(), hostPose.lla, hostPose.eulerYprDeg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.3 位姿合成（offset 的椭球版本见下方椭球 E2E；同步层只 LLA）
// -----------------------------------------------------------------------------

SCENARIO("update re-applies last Host eye when no new eye arrives",
         "[acceptance][bdd][sync][hostctrl][LLA-reuse-last]")
{
    GIVEN("a linked Engine after one Host eye was applied")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        TestHost host;
        REQUIRE(host.init(18300));
        REQUIRE(engine.init(modelPath, makeIgOnlyRole(18001, 18300)));
        engine.cameraDriver().setOffsetDeg({});

        const ChannelEye hostPose{{39.9, 116.4, 500.0}, {15.0, 0.0, 0.0}};
        engine.cameraDriver().compose(hostPose);
        engine.stepSync();
        requireLookAtMatchesLlaPose(engine, *engine.ellipsoidModel(), hostPose.lla, hostPose.eulerYprDeg);

        WHEN("local pose is changed and update runs without a new Host eye")
        {
            REQUIRE(engine.setCameraPoseLla(vsg::dvec3{39.9, 116.4, 400.0}, vsg::dvec3{0.0, 0.0, 0.0}));
            engine.stepSync();

            THEN("camera returns to the cached Host eye")
            {
                requireLookAtMatchesLlaPose(engine, *engine.ellipsoidModel(), hostPose.lla, hostPose.eulerYprDeg);
            }
        }
    }
}

SCENARIO("after disconnect, camera keeps the last Host eye pose",
         "[acceptance][bdd][sync][hostctrl][disconnect][LLA-keep-after-disconnect]")
{
    GIVEN("a linked Engine that applied a Host eye then lost the IG link")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        TestHost host;
        REQUIRE(host.init(18500));
        REQUIRE(engine.init(modelPath, makeIgOnlyRole(18001, 18500)));
        engine.cameraDriver().setOffsetDeg({});

        const ChannelEye hostPose{{39.9, 116.4, 500.0}, {25.0, 0.0, 0.0}};
        engine.cameraDriver().compose(hostPose);
        engine.stepSync();
        REQUIRE(engine.synchronSystem().igLinked());

        engine.synchronSystem().igSync().shutdown();
        REQUIRE_FALSE(engine.synchronSystem().igLinked());

        WHEN("local pose is changed and update runs while disconnected")
        {
            REQUIRE(engine.setCameraPoseLla(vsg::dvec3{39.9, 116.4, 400.0}, vsg::dvec3{90.0, 0.0, 0.0}));
            engine.stepSync();

            THEN("camera is restored to the last Host eye")
            {
                requireLookAtMatchesLlaPose(engine, *engine.ellipsoidModel(), hostPose.lla, hostPose.eulerYprDeg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.5 权威窗同样回灌（无旁路）
// -----------------------------------------------------------------------------

// =============================================================================
// 5. LLA / 椭球 Host↔IG 部署一致性（lla位姿传输设计.md §2.4 / §2.5 / §4.5 / §7）
// 冒烟：对齐半径；错配：模式拒收；半径不一致：跟拍 ECEF 超差（已知错配，禁止默默绿过）。
// =============================================================================

namespace
{
    // 刚性阵列 E2E 的通道配置 JSON：纯 IG（不含 hostConfig）；
    // requireConnectedIg 按通道指定（A 连 host 强制，B/C 宽松）。
    // TempConfigFile 来自公共头 Common.h。
    std::string makeChannelConfigBody(int kBase, int channelId, int udpRecv, double yawOffset,
                                      bool requireConnectedIg, bool injectEllipsoidIfMissing,
                                      const std::string& model)
    {
        std::string body = std::string(R"({
              "syncSystem": {
              "channelId": )") +
                           std::to_string(channelId) + R"(,
              "offsetDeg": { "yaw": )" +
                           std::to_string(yawOffset) + R"(, "pitch": 0.0, "roll": 0.0 },
              "requireConnectedIg": )" +
                           (requireConnectedIg ? std::string("true") : std::string("false")) + R"(
              },
              "injectEllipsoidIfMissing": )" +
                           (injectEllipsoidIfMissing ? std::string("true") : std::string("false")) + R"(,
              "igConfig": { "udpPortRecv": )" +
                           std::to_string(udpRecv) +
                           R"(, "targetAddr": "127.0.0.1", "targetTcpPort": )" + std::to_string(kBase + 100) +
                           R"(, "targetUdpPortRecv": )" + std::to_string(kBase) + R"( },
              "model": ")" +
                           model + R"(",
              "window": { "x": 0, "y": 0, "width": 640, "height": 480 }
            })";
        return body;
    }

    vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidOf(Engine& engine)
    {
        auto camera = engine.mainCamera();
        if (!camera || !camera->projectionMatrix)
            return {};
        auto perspective = camera->projectionMatrix.cast<vsg::EllipsoidPerspective>();
        if (!perspective)
            return {};
        return perspective->ellipsoidModel;
    }

    bool radiiEqual(const vsg::EllipsoidModel& a, const vsg::EllipsoidModel& b, double eps = 1e-6)
    {
        return std::abs(a.radiusEquator() - b.radiusEquator()) <= eps &&
               std::abs(a.radiusPolar() - b.radiusPolar()) <= eps;
    }

    // 三通道刚性阵列 E2E 的公共装载：独立 HostSync（hostConfig 文件）+ A（IG，offset 0，graphics）
    // + B/C（IG-only，yaw ±60）。全部经配置文件驱动。B/C 走 sync + scene mode only。
    struct RigidArrayHarness
    {
        HostSync host;
        Engine a;
        Engine b;
        Engine c;

        RigidArrayHarness(int kBase, bool injectEllipsoidIfMissing, const std::string& model)
        {
            // 独立 Host 端点（hostConfig 文件，viewhost 形态）。
            const TempConfigFile hostFile(
                std::string(R"({ "hostConfig": { "udpPortRecv": )") + std::to_string(kBase) +
                R"(, "tcpPort": )" + std::to_string(kBase + 100) + R"( } })");
            HostConfig hostCfg;
            std::string hostErr;
            REQUIRE(loadHostConfig(hostFile.path(), hostCfg, &hostErr));
            REQUIRE(host.initialize(hostCfg));
            host.run();

            const TempConfigFile igAFile(
                makeChannelConfigBody(kBase, 0, kBase + 1, 0.0, /*requireConnectedIg=*/true, injectEllipsoidIfMissing, model));
            const TempConfigFile igBFile(
                makeChannelConfigBody(kBase, 1, kBase + 3, -60.0, /*requireConnectedIg=*/false, injectEllipsoidIfMissing, model));
            const TempConfigFile igCFile(
                makeChannelConfigBody(kBase, 2, kBase + 5, 60.0, /*requireConnectedIg=*/false, injectEllipsoidIfMissing, model));

            a.extent = {1920, 1080};
            a.showWindow = b.showWindow = c.showWindow = false;

            REQUIRE(a.loadConfig(igAFile.path()));
            REQUIRE(b.loadConfig(igBFile.path()));
            REQUIRE(c.loadConfig(igCFile.path()));
            REQUIRE(a.init());
            // B/C：sync + scene mode only（单进程避免第三个 Vulkan Device）。
            REQUIRE(b.initSync(b.config.igConfig, b.config.syncSystem));
            REQUIRE(b.initSceneMode(vsg::Path(RESOURCE_DIR) / b.config.model));
            b.cameraDriver().setOffsetDeg(b.config.syncSystem.offsetDeg);
            REQUIRE(c.initSync(c.config.igConfig, c.config.syncSystem));
            REQUIRE(c.initSceneMode(vsg::Path(RESOURCE_DIR) / c.config.model));
            c.cameraDriver().setOffsetDeg(c.config.syncSystem.offsetDeg);

            // 握手是异步网络时序：Windows 下偶发 UDP_SYNC 丢包/调度延迟会让 connect 失败。
            // B/C 的 requireConnectedIg=false 会静默继续，此时对未同步的 IG 重连并等待全部 ready。
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (host.readyIgCount() == 3)
                    return;
                for (Engine* ig : {&b, &c})
                {
                    if (ig->synchronSystem().hasIg() && !ig->synchronSystem().igSync().udpSynced())
                        ig->synchronSystem().igSync().connect(ig->config.igConfig->target);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            REQUIRE(host.readyIgCount() == 3);
        }

        void tick(const ChannelEye& eye, const int frames = 2)
        {
            for (int i = 0; i < frames; ++i)
            {
                hostSendEyeFrame(host, eye);
                a.tickSync();
                b.tickSync();
                c.tickSync();
            }
        }
    };

    // ECEF 刚性断言：B/C up（ECEF）与 Host up 平行、forward == R_host·Rz(δ) 经 ENU→ECEF 映射。
    void requireEcefRigidArray(RigidArrayHarness& h, const vsg::dvec3& lla, const vsg::dvec3& yprHost)
    {
        auto emA = ellipsoidOf(h.a);
        REQUIRE(emA);
        const vsg::dmat4 localToWorld = emA->computeLocalToWorldTransform(lla);

        // A（offset 0）相机 up（ECEF）由真实 LookAt 读取，代表刚性阵列的 up。
        auto lookAtA = h.a.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAtA);
        const vsg::dvec3 hostUpEcef = vsg::normalize(lookAtA->up);

        // R_host 在 ENU 中构建（lla §3.2 同一约定）；δ 为通道 yaw offset。
        const vsg::dquat rHost = quatFromEulerYprDeg(yprHost);
        const vsg::dvec3 upEnuExpected = vsg::normalize(rHost * vsg::dvec3(0.0, 0.0, 1.0));
        const vsg::dvec3 upEcefExpected = vsg::normalize(rotateEnuToEcef(localToWorld, upEnuExpected));

        const auto check = [&](const std::optional<ChannelEye>& applied, const OffsetDeg& offset, Engine& ig) {
            REQUIRE(applied.has_value());
            // B/C 为 sync-only（无相机），从 _lastApplied 的 ENU YPR 经各自椭球转到 ECEF。
            auto em = ig.ellipsoidModel();
            REQUIRE(em);
            const vsg::dmat4 l2w = em->computeLocalToWorldTransform(applied->lla);
            const vsg::dvec3 upEcef =
                vsg::normalize(rotateEnuToEcef(l2w, rotateByEulerYprDeg(applied->eulerYprDeg, vsg::dvec3(0.0, 0.0, 1.0))));
            const vsg::dvec3 forwardEcef =
                vsg::normalize(rotateEnuToEcef(l2w, rotateByEulerYprDeg(applied->eulerYprDeg, vsg::dvec3(0.0, 1.0, 0.0))));

            const vsg::dquat rzDelta = vsg::dquat(vsg::radians(offset.yaw), vsg::dvec3(0.0, 0.0, 1.0));
            const vsg::dvec3 forwardEnuExpected = vsg::normalize(rHost * (rzDelta * vsg::dvec3(0.0, 1.0, 0.0)));
            const vsg::dvec3 forwardEcefExpected = vsg::normalize(rotateEnuToEcef(l2w, forwardEnuExpected));

            // CIGI YPR 走 float 量化 + ENU→ECEF 换算，方向容差放 1e-4。
            constexpr double kDirEps = 1e-4;
            REQUIRE(vsg::length(upEcef - hostUpEcef) < kDirEps);               // 与 Host up 平行（刚性阵列贴边）
            REQUIRE(vsg::length(upEcef - upEcefExpected) < kDirEps);           // up == R_host·Up（ECEF）
            REQUIRE(vsg::length(forwardEcef - forwardEcefExpected) < kDirEps); // forward == R_host·Rz(δ)·ŷ（ECEF）
        };
        check(h.b.cameraDriver().lastAppliedEye(), h.b.config.syncSystem.offsetDeg, h.b);
        check(h.c.cameraDriver().lastAppliedEye(), h.c.config.syncSystem.offsetDeg, h.c);
    }

    vsg::dvec3 lookAtEye(Engine& engine)
    {
        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);
        return lookAt->eye;
    }
} // namespace

SCENARIO("Host LLA eye is followed by IG LookAt ECEF on aligned ellipsoids",
         "[acceptance][bdd][sync][lla][follow][LLA-follow-aligned]")
{
    GIVEN("an independent Host, IG Engine A and IG-only Engine B both on readymap with zero offset")
    {
        constexpr int kBase = 19500;
        TestHost host;
        REQUIRE(host.init(kBase));
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engineA.initSync(makeIgOnlyRole(kBase + 1, kBase), makeTestSyncSystem(0)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase), makeTestSyncSystem(1)));
        REQUIRE(host.sync.readyIgCount() == 2);
        engineA.config.injectEllipsoidIfMissing = true;
        engineB.config.injectEllipsoidIfMissing = true;
        REQUIRE(engineA.initGraphics(modelPath));
        REQUIRE(engineB.initSceneMode(modelPath)); // sync-only IG：单进程避免第二个 Vulkan Device

        auto emA = ellipsoidOf(engineA);
        REQUIRE(emA);
        REQUIRE(engineB.ellipsoidModel());
        auto emB = engineB.ellipsoidModel();
        REQUIRE(emB);
        REQUIRE(radiiEqual(*emA, *emB));

        engineA.cameraDriver().setOffsetDeg({});
        engineB.cameraDriver().setOffsetDeg({});

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 ypr{30.0, 5.0, 0.0};
        const vsg::dvec3 expectedEcef = emA->convertLatLongAltitudeToECEF(lla);
        const ChannelEye intent{lla, ypr};

        WHEN("Host publishes LLA authority eye over live CIGI and both tick")
        {
            for (int i = 0; i < 3; ++i)
            {
                hostSendEyeFrame(host.sync, intent);
                engineA.tickSync();
                engineB.tickSync();
            }

            THEN("B applies Host LLA; A LookAt.eye matches ECEF (lla Host-IG follow)")
            {
                constexpr double kEcefEps = 1e-2; // meter-scale ECEF tolerance (design §4.4 band)
                auto applied = engineB.cameraDriver().lastAppliedEye();
                REQUIRE(applied.has_value());
                REQUIRE(vsg::length(applied->lla - lla) < 1e-6);
                REQUIRE(vsg::length(lookAtEye(engineA) - expectedEcef) < kEcefEps);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// lla设计 §3.4 / §7：椭球 offsetDeg — 刚性阵列旋转复合 R_ig=R_host·Rz(δ)（绕 Host ENU up）
// -----------------------------------------------------------------------------

SCENARIO("ellipsoid zero offset keeps Host LLA eye unchanged",
         "[acceptance][bdd][sync][lla][offset][LLA-zero-offset]")
{
    GIVEN("an IG Engine on readymap linked to an independent Host with channel offset all zero")
    {
        constexpr int kBase = 20000;
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        engine.config.injectEllipsoidIfMissing = true;
        TestHost host;
        REQUIRE(host.init(kBase));
        REQUIRE(engine.init(modelPath, makeIgOnlyRole(kBase + 1, kBase)));
        auto em = ellipsoidOf(engine);
        REQUIRE(em);

        engine.cameraDriver().setOffsetDeg({0.0, 0.0, 0.0});

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 ypr{25.0, 8.0, -3.0};
        const ChannelEye hostPose{lla, ypr};

        WHEN("a Host LLA eye becomes available and sync update runs")
        {
            engine.cameraDriver().compose(hostPose);
            engine.stepSync();

            THEN("LookAt matches Host LLA + ENU YPR (no channel yaw)")
            {
                requireLookAtMatchesLlaPose(engine, *em, lla, ypr, 1e-2, 1e-6);
            }
        }
    }
}

SCENARIO("ellipsoid IG applies Host LLA eye plus yaw-only ENU offset",
         "[acceptance][bdd][sync][lla][offset][LLA-yaw-offset]")
{
    GIVEN("an IG Engine on readymap linked to an independent Host with yaw offset -60 deg")
    {
        constexpr int kBase = 20020;
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        engine.config.injectEllipsoidIfMissing = true;
        TestHost host;
        REQUIRE(host.init(kBase));
        REQUIRE(engine.init(modelPath, makeIgOnlyRole(kBase + 1, kBase)));
        auto em = ellipsoidOf(engine);
        REQUIRE(em);

        OffsetDeg offset{-60.0, 0.0, 0.0};
        engine.cameraDriver().setOffsetDeg(offset);

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const ChannelEye hostPose{lla, {30.0, 5.0, 1.0}};
        const ChannelEye expected = engine.cameraDriver().compose(hostPose);

        WHEN("a Host LLA eye becomes available and sync update runs")
        {
            engine.cameraDriver().compose(hostPose);
            engine.stepSync();

            THEN("LookAt uses Host LLA composed with ENU yaw offset (rigid-array rotation)")
            {
                REQUIRE(vsg::length(expected.lla - lla) < 1e-12);
                requireLookAtMatchesLlaPose(engine, *em, expected.lla, expected.eulerYprDeg, 1e-2, 1e-6);
            }
        }
    }
}

SCENARIO("ellipsoid yaw-only offset keeps channel up parallel to Host up (R_ig=R_host*Rz(delta))",
         "[acceptance][bdd][sync][lla][offset][LLA-up-parallel]")
{
    GIVEN("an IG Engine on readymap linked to an independent Host; Host LLA eye has pitch/roll; channel yaw-only")
    {
        constexpr int kBase = 20040;
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        engine.config.injectEllipsoidIfMissing = true;
        TestHost host;
        REQUIRE(host.init(kBase));
        REQUIRE(engine.init(modelPath, makeIgOnlyRole(kBase + 1, kBase)));
        auto em = ellipsoidOf(engine);
        REQUIRE(em);

        constexpr double kDeltaYawDeg = 18.05;
        OffsetDeg offset{kDeltaYawDeg, 0.0, 0.0};
        engine.cameraDriver().setOffsetDeg(offset);

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        // 非零 roll 覆盖刚性阵列不变量：up 与 Host up 保持平行。
        const ChannelEye hostPose{lla, {20.0, 15.0, -8.0}};

        WHEN("the Host LLA eye is applied with that yaw-only channel offset")
        {
            engine.cameraDriver().compose(hostPose);
            engine.stepSync();

            THEN("LookAt matches Host LLA composed with R_ig=R_host*Rz(delta) for yaw-only offset")
            {
                const ChannelEye expected = engine.cameraDriver().compose(hostPose);
                requireLookAtMatchesLlaPose(engine, *em, expected.lla, expected.eulerYprDeg, 1e-2, 1e-6);
            }
        }
    }
}

SCENARIO("remote IG follows Host LLA with channel yaw offset over CIGI",
         "[acceptance][bdd][sync][lla][offset][e2e][cigi][LLA-remote-yaw]")
{
    GIVEN("an independent Host, IG A (offset 0) and IG-only B (yaw +60) both on readymap")
    {
        constexpr int kBase = 20060;
        TestHost host;
        REQUIRE(host.init(kBase));
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engineA.initSync(makeIgOnlyRole(kBase + 1, kBase), makeTestSyncSystem(0)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase), makeTestSyncSystem(1)));
        REQUIRE(host.sync.readyIgCount() == 2);
        engineA.config.injectEllipsoidIfMissing = true;
        engineB.config.injectEllipsoidIfMissing = true;
        REQUIRE(engineA.initGraphics(modelPath));
        REQUIRE(engineB.initSceneMode(modelPath));

        REQUIRE(engineB.ellipsoidModel());
        REQUIRE(radiiEqual(*ellipsoidOf(engineA), *engineB.ellipsoidModel()));

        OffsetDeg offsetB{60.0, 0.0, 0.0};
        engineA.cameraDriver().setOffsetDeg({});
        engineB.cameraDriver().setOffsetDeg(offsetB);

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 yprHost{30.0, 5.0, 0.0};
        const ChannelEye intent{lla, yprHost};
        const ChannelEye expectedB = engineB.cameraDriver().compose(intent);

        WHEN("Host publishes LLA authority eye over live CIGI and both tick")
        {
            for (int i = 0; i < 3; ++i)
            {
                hostSendEyeFrame(host.sync, intent);
                engineA.tickSync();
                engineB.tickSync();
            }

            THEN("B applied pose matches Host LLA with ENU YPR plus B yaw offset")
            {
                auto applied = engineB.cameraDriver().lastAppliedEye();
                REQUIRE(applied.has_value());
                requirePoseNear(*applied, expectedB, 1e-3);
            }
        }
    }
}

// lla设计 §3.4 / 多通道同步设计 §4.5：ECEF/椭球下，真 CIGI 报文 + Host 非零 roll 时，
// 各通道 up 轴与 Host up 平行（刚性阵列一起 roll、frustum 贴边），且 forward 满足
// R_ig=R_host·Rz(δ)。本地刚性用例的 ECEF 对应物；无自带椭球时用 injectEllipsoidIfMissing 注入 WGS-84
// （lla位姿传输设计.md §2）。
SCENARIO("three ellipsoid channels keep up axes parallel to Host when it rolls over live CIGI",
         "[acceptance][bdd][sync][lla][offset][e2e][multi-ig][cigi][rigid][LLA-three-up-parallel]")
{
    GIVEN("an independent Host and IG-only A/B/C with injected WGS-84 ellipsoid, yaw offsets -60/+60")
    {
        RigidArrayHarness h(20120, true, "models/lz.vsgt");

        REQUIRE(h.a.ellipsoidModel());
        REQUIRE(h.b.ellipsoidModel());
        REQUIRE(h.c.ellipsoidModel());
        auto emA = ellipsoidOf(h.a);
        REQUIRE(emA);
        REQUIRE(radiiEqual(*emA, *h.b.ellipsoidModel()));
        REQUIRE(radiiEqual(*emA, *h.c.ellipsoidModel()));

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        // Host 带非零 roll：回归点——刚性阵列必须整体滚转，up 轴保持平行。
        const vsg::dvec3 yprHost{30.0, 12.0, -18.0};
        const ChannelEye llaIntent{lla, yprHost};

        WHEN("Host publishes the rolled LLA intent and all channels tick (shared CIGI Detach+LLA eye)")
        {
            h.tick(llaIntent, 3);

            THEN("B/C up axes stay parallel to Host up and forwards equal R_host*Rz(delta) in ECEF")
            {
                requireEcefRigidArray(h, lla, yprHost);
            }
        }
    }
}

SCENARIO("aligned Host and IG ellipsoid smoke: both have EllipsoidModel with matching radii",
         "[acceptance][bdd][sync][lla][smoke][LLA-radii-match]")
{
    GIVEN("an independent Host, IG Engine A and IG-only Engine B both load readymap (built-in EllipsoidModel)")
    {
        constexpr int kBase = 19200;
        TestHost host;
        REQUIRE(host.init(kBase));
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engineA.initSync(makeIgOnlyRole(kBase + 1, kBase), makeTestSyncSystem(0)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase), makeTestSyncSystem(1)));
        REQUIRE(host.sync.readyIgCount() == 2);
        engineA.config.injectEllipsoidIfMissing = true;
        engineB.config.injectEllipsoidIfMissing = true;
        REQUIRE(engineA.initGraphics(modelPath));
        REQUIRE(engineB.initSceneMode(modelPath));

        WHEN("both engines have finished scene assembly")
        {
            auto emA = ellipsoidOf(engineA);
            auto emB = engineB.ellipsoidModel();

            THEN("both scenes are ellipsoid and radii match (aligned deployment smoke)")
            {
                REQUIRE(emA);
                REQUIRE(emB);
                REQUIRE(engineB.ellipsoidModel());
                REQUIRE(radiiEqual(*emA, *emB));
            }
        }
    }
}

SCENARIO("Host readymap vs IG inject-WGS84 radius mismatch makes ECEF follow disagree",
         "[acceptance][bdd][sync][lla][radius-mismatch][LLA-radii-mismatch]")
{
    GIVEN("an independent HostSync and IG on lz with injected WGS-84 ellipsoid")
    {
        constexpr int kBase = 19400;
        // 独立 Host 端点（hostConfig 文件，viewhost 形态）。
        const TempConfigFile hostFile(
            std::string(R"({ "hostConfig": { "udpPortRecv": )") + std::to_string(kBase) +
            R"(, "tcpPort": )" + std::to_string(kBase + 100) + R"( } })");
        HostConfig hostCfg;
        std::string hostErr;
        REQUIRE(loadHostConfig(hostFile.path(), hostCfg, &hostErr));
        HostSync host;
        REQUIRE(host.initialize(hostCfg));
        host.run();

        // engine A（IG，readymap 自带椭球）。
        const TempConfigFile igAFile(
            std::string(R"({
              "syncSystem": {
              "channelId": 0,
              "offsetDeg": { "yaw": 0.0, "pitch": 0.0, "roll": 0.0 },
              "requireConnectedIg": true
              },
              "injectEllipsoidIfMissing": true,
              "igConfig": { "udpPortRecv": )") +
            std::to_string(kBase + 1) +
            R"(, "targetAddr": "127.0.0.1", "targetTcpPort": )" + std::to_string(kBase + 100) +
            R"(, "targetUdpPortRecv": )" + std::to_string(kBase) + R"( },
              "model": "models/readymap.vsgt",
              "window": { "x": 0, "y": 0, "width": 640, "height": 480 }
            })");

        const TempConfigFile igFile(
            std::string(R"({
              "syncSystem": {
              "channelId": 1,
              "offsetDeg": { "yaw": 0.0, "pitch": 0.0, "roll": 0.0 },
              "requireConnectedIg": false
              },
              "injectEllipsoidIfMissing": true,
              "igConfig": { "udpPortRecv": )") +
            std::to_string(kBase + 3) +
            R"(, "targetAddr": "127.0.0.1", "targetTcpPort": )" + std::to_string(kBase + 100) +
            R"(, "targetUdpPortRecv": )" + std::to_string(kBase) + R"( },
              "model": "models/lz.vsgt",
              "window": { "x": 0, "y": 0, "width": 640, "height": 480 }
            })");

        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;
        REQUIRE(engineA.loadConfig(igAFile.path()));
        REQUIRE(engineB.loadConfig(igFile.path()));
        REQUIRE(engineA.init());
        // B：sync + scene mode only（部分机器单 Vulkan Device 限制）。
        REQUIRE(engineB.initSync(engineB.config.igConfig, engineB.config.syncSystem));
        REQUIRE(engineB.initSceneMode(vsg::Path(RESOURCE_DIR) / engineB.config.model));
        engineB.cameraDriver().setOffsetDeg(engineB.config.syncSystem.offsetDeg);

        auto emA = ellipsoidOf(engineA);
        auto emB = engineB.ellipsoidModel();
        REQUIRE(emA);
        REQUIRE(emB);
        // 已知不匹配：readymap 半径 ≠ 默认 WGS-84 注入（lla §2.4）。
        REQUIRE_FALSE(radiiEqual(*emA, *emB));

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 ypr{0.0, 0.0, 0.0};
        const vsg::dvec3 hostEcef = emA->convertLatLongAltitudeToECEF(lla);
        const vsg::dvec3 igEcefSameLla = emB->convertLatLongAltitudeToECEF(lla);
        REQUIRE(vsg::length(hostEcef - igEcefSameLla) > 0.5);

        // requireConnectedIg=false 时握手失败会静默继续，而 `IgSync::connect` 内部重试
        // （TCP×16、握手×8）在 Windows 下仍可能因 UDP_SYNC_ACK 偶发丢包耗尽（多通道同步模块设计.md §9 P1）。
        // 测试侧持续重连直到 A 与 B 全部 ready，用反复 connect 兜住单次重试耗尽的问题。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (host.readyIgCount() == 2)
                break;
            if (engineB.synchronSystem().hasIg() && !engineB.synchronSystem().igSync().udpSynced())
                engineB.synchronSystem().igSync().connect(engineB.config.igConfig->target);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(host.readyIgCount() == 2);

        const ChannelEye llaEye{lla, ypr};

        WHEN("Host publishes that LLA eye and B follows over CIGI")
        {
            for (int i = 0; i < 3; ++i)
            {
                hostSendEyeFrame(host, llaEye);
                engineA.tickSync();
                engineB.tickSync();
            }

            THEN("B applied LLA converts to ECEF that disagrees with Host beyond meter-scale")
            {
                auto applied = engineB.cameraDriver().lastAppliedEye();
                REQUIRE(applied.has_value());
                const vsg::dvec3 igEcef = emB->convertLatLongAltitudeToECEF(applied->lla);
                REQUIRE(vsg::length(igEcef - hostEcef) > 0.5);
            }
        }
    }
}

// =============================================================================
// 6. LLA 验收补齐：范围校验 / 权威 offset / 缓存复位（lla设计 §7）
// =============================================================================

SCENARIO("Host still sends IGCtrl when ownship LLA is out of range",
         "[integration][sync][cigi][wire-contract][lla][range][CIGI-lla-oor]")
{
    GIVEN("a Host and an IG that have completed CIGI handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(linkHostIg(host, ig, 19300));

        OwnshipEyeCapture cap;
        captureOwnship(ig, cap);

        WHEN("Host sends ownship eyes with latitude and pitch out of range")
        {
            const auto rejectedBefore = cigi_wire::eyePoseRejectedByRange();
            cigi_wire::EyePose badLat{};
            badLat.x = 91.0;
            badLat.y = 10.0;
            badLat.z = 100.0;
            pumpOwnshipEyeFrames(host, ig, badLat);
            const auto receivedAfterLat = ig.igCtrlReceivedCount();
            const bool eyeAfterLat = cap.got;
            const auto rejectedAfterLat = cigi_wire::eyePoseRejectedByRange();

            cap.got = false;
            cigi_wire::EyePose badPitch{};
            badPitch.x = 39.9;
            badPitch.y = 116.4;
            badPitch.z = 500.0;
            badPitch.pitchDeg = 95.0;
            pumpOwnshipEyeFrames(host, ig, badPitch);

            THEN("IG received IGCtrl, no ownship eye, and the range counter advanced")
            {
                REQUIRE(receivedAfterLat >= 1);
                REQUIRE_FALSE(eyeAfterLat);
                REQUIRE(rejectedAfterLat > rejectedBefore);
                REQUIRE(ig.igCtrlReceivedCount() >= receivedAfterLat);
                REQUIRE_FALSE(cap.got);
                REQUIRE(cigi_wire::eyePoseRejectedByRange() > rejectedAfterLat);
            }
        }
    }
}

SCENARIO("linked IG receives Host longitude wrapped into (-180,180]",
         "[integration][sync][cigi][wire-contract][lla][range][CIGI-lon-wrap]")
{
    GIVEN("a Host and an IG that have completed CIGI handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(linkHostIg(host, ig, 19400));

        OwnshipEyeCapture cap;
        captureOwnship(ig, cap);

        WHEN("Host sends ownship eyes with longitudes that wrap across ±180")
        {
            auto sendLon = [&](double lonIn) {
                cap.got = false;
                cigi_wire::EyePose eye{};
                eye.x = 10.0;
                eye.y = lonIn;
                eye.z = 50.0;
                pumpOwnshipEyeFrames(host, ig, eye);
                REQUIRE(cap.got);
                return cap.lon;
            };

            const double lonFrom190 = sendLon(190.0);
            const double lonFromNeg190 = sendLon(-190.0);
            const double lonFrom180 = sendLon(180.0);

            THEN("IG sees longitude in (-180,180]")
            {
                REQUIRE(lonFrom190 == Catch::Approx(-170.0));
                REQUIRE(lonFromNeg190 == Catch::Approx(170.0));
                REQUIRE(lonFrom180 == Catch::Approx(180.0));
            }
        }
    }
}

SCENARIO("initGraphics clears SynchronSystem eye caches without network shutdown",
         "[acceptance][bdd][sync][lla][cache-reset][LLA-cache-reset]")
{
    GIVEN("a linked IG Engine with an applied Host eye")
    {
        constexpr int kBase = 19700;
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        TestHost host;
        REQUIRE(host.init(kBase));
        REQUIRE(engine.init(modelPath, makeIgOnlyRole(kBase + 1, kBase)));
        REQUIRE(engine.synchronSystem().igLinked());

        const ChannelEye hostPose{{39.9, 116.4, 500.0}, {12.0, 0.0, 0.0}};
        engine.cameraDriver().setOffsetDeg({});
        engine.cameraDriver().compose(hostPose);
        engine.stepSync();

        REQUIRE(engine.cameraDriver().lastAppliedEye().has_value());
        REQUIRE(engine.synchronSystem().hasIg());

        WHEN("initGraphics rebuilds the scene without SynchronSystem::shutdown")
        {
            REQUIRE(engine.initGraphics(modelPath));

            THEN("eye caches are empty while IG link remains")
            {
                REQUIRE_FALSE(engine.cameraDriver().lastAppliedEye().has_value());
                REQUIRE(engine.synchronSystem().hasIg());
                REQUIRE(engine.synchronSystem().igLinked());
            }
        }
    }
}

// =============================================================================
// viewhost E2E：loadHostConfig → HostSync 与带 IG 的 Engine 真实 TCP/UDP/CIGI 收发
// （sync模块化设计.md §4.1）。
// =============================================================================

SCENARIO("viewhost loads hostConfig and exchanges CIGI with an IG engine",
         "[acceptance][bdd][sync][viewhost][cigi][CIGI-viewhost-exchange]")
{
    GIVEN("a viewhost HostSync loaded from a host-only config file, and an IG engine targeting it")
    {
        constexpr int kBase = 21000;

        // viewhost 配置：与 makeTestIgConfig 的 target（tcp=base+100, udpRecv=base）对齐。
        const TempConfigFile viewhostFile(
            std::string(R"({ "hostConfig": { "udpPortRecv": )") + std::to_string(kBase) +
            R"(, "tcpPort": )" + std::to_string(kBase + 100) + R"( } })");

        HostConfig host;
        std::string error;
        REQUIRE(loadHostConfig(viewhostFile.path(), host, &error));

        // viewhost 纯 Host：直接持 HostSync（直发，不经 SynchronSystem 门面）。
        auto viewhost = std::make_unique<HostSync>();
        REQUIRE(viewhost->initialize(host));
        viewhost->run();

        // 带 IG 的 engine（纯 IG，连 viewhost）。
        Engine engineIg;
        engineIg.extent = {640, 480};
        engineIg.showWindow = false;
        REQUIRE(engineIg.initSync(makeTestIgConfig(kBase + 3, kBase)));

        WHEN("both link over TCP and UDP handshake")
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
            while (viewhost->readyIgCount() < 1 &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            THEN("viewhost sees one ready IG")
            {
                REQUIRE(viewhost->readyIgCount() == 1);
                REQUIRE(engineIg.synchronSystem().igLinked());
            }

            THEN("CIGI IGCtrl flows Host→IG and SOF flows IG→Host")
            {
                viewhost->drainIncoming();
                const std::uint32_t sentBefore = viewhost->igCtrlSentCount();
                const std::uint32_t recvBefore = engineIg.synchronSystem().igSync().igCtrlReceivedCount();
                const std::uint32_t sofBefore = viewhost->sofReceivedCount();

                constexpr int kTicks = 5;
                for (int i = 0; i < kTicks; ++i)
                {
                    hostSendFrame(*viewhost, i * 16.667); // 无渲染节拍：业务侧扇出
                    engineIg.tickSync();                  // IG 收包 + 回 SOF
                }

                viewhost->drainIncoming();
                REQUIRE(viewhost->igCtrlSentCount() > sentBefore);
                REQUIRE(engineIg.synchronSystem().igSync().igCtrlReceivedCount() > recvBefore);
                REQUIRE(viewhost->sofReceivedCount() > sofBefore);
            }
        }
    }
}

// =============================================================================
// 独立 IG 配置 E2E：host 与 IG 双侧都走 sync 库独立配置文件
// （loadHostConfig / loadIgConfig → 各自 SynchronSystem），装配参数程序化注入。
// 验证外部 engine 脱离引擎整体配置使用 sync 的完整路径（sync模块化设计.md §4.1/§4.2）。
// =============================================================================

SCENARIO("host and IG both load standalone sync configs and exchange CIGI",
         "[acceptance][bdd][sync][standalone][cigi][CIGI-standalone]")
{
    GIVEN("host HostSync from hostConfig file, and IG SynchronSystem from igConfig file")
    {
        constexpr int kBase = 22000;

        // host 独立配置（udpRecv=base, tcp=base+100）。
        const TempConfigFile hostFile(
            std::string(R"({ "hostConfig": { "udpPortRecv": )") + std::to_string(kBase) +
            R"(, "tcpPort": )" + std::to_string(kBase + 100) + R"( } })");
        // IG 独立配置（本地 udpRecv=base+3，target 指向 host 的 tcp=base+100 / udpRecv=base）。
        const TempConfigFile igFile(
            std::string(R"({ "igConfig": { "udpPortRecv": )") + std::to_string(kBase + 3) +
            R"(, "targetAddr": "127.0.0.1", "targetTcpPort": )" + std::to_string(kBase + 100) +
            R"(, "targetUdpPortRecv": )" + std::to_string(kBase) + R"( } })");

        HostConfig host;
        std::string hostError;
        REQUIRE(loadHostConfig(hostFile.path(), host, &hostError));
        IgConfig ig;
        std::string igError;
        REQUIRE(loadIgConfig(igFile.path(), ig, &igError));

        // 两侧：host 用 HostSync 直发；IG 用 SynchronSystem（IG 决策器）。
        auto hostSync = std::make_unique<HostSync>();
        REQUIRE(hostSync->initialize(host));
        hostSync->run();

        auto igSync = SynchronSystem::create();
        // 装配参数程序化注入（外部 engine 不经 syncSystem 配置文件时的路径）。
        SyncSystemConfig igSystem;
        igSystem.channelId = 2;
        igSystem.offsetDeg = OffsetDeg{5.0, 0.0, 0.0};
        REQUIRE(igSync->initialize(ig, igSystem));

        WHEN("IG connects to host and both link")
        {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
            while (hostSync->readyIgCount() < 1 &&
                   std::chrono::steady_clock::now() < deadline)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            THEN("host sees the standalone IG and IG is linked")
            {
                REQUIRE(hostSync->readyIgCount() == 1);
                REQUIRE(igSync->igLinked());
            }

            THEN("CIGI IGCtrl and SOF flow both ways over TCP/UDP")
            {
                hostSync->drainIncoming();
                const std::uint32_t sentBefore = hostSync->igCtrlSentCount();
                const std::uint32_t recvBefore = igSync->igSync().igCtrlReceivedCount();
                const std::uint32_t sofBefore = hostSync->sofReceivedCount();

                constexpr int kTicks = 5;
                for (int i = 0; i < kTicks; ++i)
                {
                    hostSendFrame(*hostSync, i * 16.667); // 业务侧扇出 IGCtrl
                    igSync->preFrame();                   // IG 收 IGCtrl + 回 SOF
                }

                hostSync->drainIncoming();
                REQUIRE(hostSync->igCtrlSentCount() > sentBefore);
                REQUIRE(igSync->igSync().igCtrlReceivedCount() > recvBefore);
                REQUIRE(hostSync->sofReceivedCount() > sofBefore);
            }
        }
    }
}
