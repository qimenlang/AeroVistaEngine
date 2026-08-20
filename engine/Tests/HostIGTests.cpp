#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine.h"
#include <aerovista/sync/CigiWire.h>
#include <aerovista/sync/HostSync.h>
#include <aerovista/sync/IgSync.h>
#include <aerovista/sync/SyncConfig.h>
#include <aerovista/sync/SynchronSystem.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "Common.h"

using aerovista::sync::FrameGate;
using aerovista::sync::HostConfig;
using aerovista::sync::HostEyeCoordFrame;
using aerovista::sync::HostEyePose;
using aerovista::sync::HostStatus;
using aerovista::sync::HostSync;
using aerovista::sync::IgConfig;
using aerovista::sync::IgStatus;
using aerovista::sync::IgSync;
using aerovista::sync::OffsetDeg;
using aerovista::sync::SendPace;
using aerovista::sync::SynchronSystem;
using aerovista::sync::SyncPaceConfig;
namespace cigi_wire = aerovista::sync::cigi_wire;

// 协议分层（测试约定）：
// - 握手 / 动态端口：仍为自建 sync_proto WireMsg（HELLO / UDP_SYNC）——§1 用例覆盖，本文件不改其方向。
// - 数据面（帧节拍 / 眼点 / SOF）：CIGI V4 CCL —— IGCtrl (+ 可选 EntityPositionCtrl) / SOF。
//   §2 / §3 / §4.6 行为断言仍走 HostSync/IgSync/Engine API；线格式契约见 [unit][cigi][wire-contract]。

namespace
{
    // 默认端口见 doc/design/多通道同步模块设计.md
    HostConfig makeHostLocal()
    {
        return HostConfig{8001, 8000, 8100};
    }

    IgConfig makeIgLocal(int udpRecvPort = 8001)
    {
        return IgConfig{8000, udpRecvPort, "127.0.0.1", 8100, 8000};
    }

    // UDP 可丢：actual 落在 [expected-slack, expected]
    bool approxAtMost(std::uint32_t actual, int expected, int slack)
    {
        const auto exp = static_cast<std::uint32_t>(expected);
        const auto minOk = exp > static_cast<std::uint32_t>(slack) ? exp - static_cast<std::uint32_t>(slack) : 0u;
        return actual >= minOk && actual <= exp;
    }

    /// 钉住数据面线契约（帧号 + 可选眼点 + SOF 回显）；经 cigi_wire 堆上 Session，避免栈溢出。
    void requireCigiDataPlaneWireContract()
    {
        constexpr std::uint32_t kFrame = 7;
        constexpr double kSimTimeMs = 123.45; // → TimeStamp 12345（10 µs 步进）
        const cigi_wire::EyePose eye{11.0, 22.0, 33.0, 40.0, 0.0, 0.0};

        std::vector<unsigned char> withEye;
        REQUIRE(cigi_wire::packHostFrame(kFrame, kSimTimeMs, &eye, withEye));
        REQUIRE_FALSE(cigi_wire::isAvsyMagic(withEye.data(), static_cast<int>(withEye.size())));

        cigi_wire::HostFrame frame{};
        REQUIRE(cigi_wire::unpackHostFrame(withEye.data(), static_cast<int>(withEye.size()), frame));
        REQUIRE(frame.frameCntr == kFrame);
        REQUIRE(frame.timeStampValid);
        REQUIRE(frame.timeStamp == cigi_wire::simTimeMsToTimeStamp(kSimTimeMs));
        REQUIRE(frame.eye.has_value());
        REQUIRE(frame.eye->x == Catch::Approx(eye.x));
        REQUIRE(frame.eye->y == Catch::Approx(eye.y));
        REQUIRE(frame.eye->z == Catch::Approx(eye.z));
        REQUIRE(frame.eye->yawDeg == Catch::Approx(eye.yawDeg));
        REQUIRE(frame.eye->frame == cigi_wire::EyeFrame::WORLD_LOCAL);
        REQUIRE(frame.eye->entityId == 0);
        REQUIRE(frame.eye->parentId == 1);

        std::vector<unsigned char> noEye;
        REQUIRE(cigi_wire::packHostFrame(kFrame + 1, kSimTimeMs, nullptr, noEye));
        cigi_wire::HostFrame frameNoEye{};
        REQUIRE(cigi_wire::unpackHostFrame(noEye.data(), static_cast<int>(noEye.size()), frameNoEye));
        REQUIRE(frameNoEye.frameCntr == kFrame + 1);
        REQUIRE_FALSE(frameNoEye.eye.has_value());

        std::vector<unsigned char> sofBuf;
        REQUIRE(cigi_wire::packSof(kFrame, sofBuf));
        std::uint32_t sofFrame = 0;
        REQUIRE(cigi_wire::unpackSof(sofBuf.data(), static_cast<int>(sofBuf.size()), sofFrame));
        REQUIRE(sofFrame == kFrame);
    }

    const char* kMainJson =
        R"({"syncSystem":{"channelId":0,"offsetDeg":{"yaw":0.0,"pitch":0.0,"roll":0.0},"hostEyeStalePolicy":"ReuseLast","requireConnectedIg":true},"igConfig":{"udpPortSend":8000,"udpPortRecv":8001,"targetAddr":"127.0.0.1","targetTcpPort":8100,"targetUdpPortRecv":8000},"hostConfig":{"udpPortSend":8001,"udpPortRecv":8000,"tcpPort":8100},"model":"models/lz.vsgt","window":{"x":640,"y":0,"width":640,"height":1080}})";
} // namespace

TEST_CASE("CIGI V4 data-plane wire contract: IGCtrl, optional EntityPosition, SOF",
          "[unit][cigi][wire-contract]")
{
    requireCigiDataPlaneWireContract();
}

// lla位姿传输设计.md §5 / §7 线契约：Attach+XYZ 与 Detach+LLA、EntityID/ParentID、帧间切换。
TEST_CASE("CIGI EntityPosition Attach+XYZ maps to WorldLocal with EntityID 0 ParentID 1",
          "[unit][cigi][wire-contract][lla]")
{
    cigi_wire::EyePose eye{};
    eye.frame = cigi_wire::EyeFrame::WORLD_LOCAL;
    eye.x = 11.0;
    eye.y = 22.0;
    eye.z = 33.0;
    eye.yawDeg = 40.0;
    eye.pitchDeg = 5.0;
    eye.rollDeg = -2.0;

    std::vector<unsigned char> buf;
    REQUIRE(cigi_wire::packHostFrame(1, 0.0, &eye, buf));

    cigi_wire::HostFrame frame{};
    REQUIRE(cigi_wire::unpackHostFrame(buf.data(), static_cast<int>(buf.size()), frame));
    REQUIRE(frame.eye.has_value());
    REQUIRE(frame.eye->frame == cigi_wire::EyeFrame::WORLD_LOCAL);
    REQUIRE(frame.eye->entityId == 0);
    REQUIRE(frame.eye->parentId == 1);
    REQUIRE(frame.eye->x == Catch::Approx(eye.x));
    REQUIRE(frame.eye->y == Catch::Approx(eye.y));
    REQUIRE(frame.eye->z == Catch::Approx(eye.z));
    REQUIRE(frame.eye->yawDeg == Catch::Approx(eye.yawDeg));
    REQUIRE(frame.eye->pitchDeg == Catch::Approx(eye.pitchDeg));
    REQUIRE(frame.eye->rollDeg == Catch::Approx(eye.rollDeg));
}

TEST_CASE("CIGI EntityPosition Detach+LLA maps to Lla with EntityID 0 ParentID 0",
          "[unit][cigi][wire-contract][lla]")
{
    cigi_wire::EyePose eye{};
    eye.frame = cigi_wire::EyeFrame::LLA;
    eye.x = 39.9;  // lat
    eye.y = 116.4; // lon
    eye.z = 500.0; // alt m
    eye.yawDeg = 30.0;
    eye.pitchDeg = 10.0;
    eye.rollDeg = 0.0;

    std::vector<unsigned char> buf;
    REQUIRE(cigi_wire::packHostFrame(2, 0.0, &eye, buf));

    cigi_wire::HostFrame frame{};
    REQUIRE(cigi_wire::unpackHostFrame(buf.data(), static_cast<int>(buf.size()), frame));
    REQUIRE(frame.eye.has_value());
    REQUIRE(frame.eye->frame == cigi_wire::EyeFrame::LLA);
    REQUIRE(frame.eye->entityId == 0);
    REQUIRE(frame.eye->parentId == 0);
    REQUIRE(frame.eye->x == Catch::Approx(eye.x));
    REQUIRE(frame.eye->y == Catch::Approx(eye.y));
    REQUIRE(frame.eye->z == Catch::Approx(eye.z));
    REQUIRE(frame.eye->yawDeg == Catch::Approx(eye.yawDeg));
    REQUIRE(frame.eye->pitchDeg == Catch::Approx(eye.pitchDeg));
}

TEST_CASE("CIGI EntityPosition Attach then Detach on EntityID 0 is a legal frame switch",
          "[unit][cigi][wire-contract][lla]")
{
    cigi_wire::EyePose attachEye{};
    attachEye.frame = cigi_wire::EyeFrame::WORLD_LOCAL;
    attachEye.x = 1.0;
    attachEye.y = 2.0;
    attachEye.z = 3.0;

    cigi_wire::EyePose detachEye{};
    detachEye.frame = cigi_wire::EyeFrame::LLA;
    detachEye.x = 39.9;
    detachEye.y = 116.4;
    detachEye.z = 500.0;
    detachEye.yawDeg = 12.0;

    std::vector<unsigned char> attachBuf;
    std::vector<unsigned char> detachBuf;
    REQUIRE(cigi_wire::packHostFrame(10, 0.0, &attachEye, attachBuf));
    REQUIRE(cigi_wire::packHostFrame(11, 0.0, &detachEye, detachBuf));

    cigi_wire::HostFrame attachFrame{};
    cigi_wire::HostFrame detachFrame{};
    REQUIRE(cigi_wire::unpackHostFrame(attachBuf.data(), static_cast<int>(attachBuf.size()), attachFrame));
    REQUIRE(cigi_wire::unpackHostFrame(detachBuf.data(), static_cast<int>(detachBuf.size()), detachFrame));

    REQUIRE(attachFrame.eye.has_value());
    REQUIRE(detachFrame.eye.has_value());
    REQUIRE(attachFrame.eye->frame == cigi_wire::EyeFrame::WORLD_LOCAL);
    REQUIRE(attachFrame.eye->entityId == 0);
    REQUIRE(attachFrame.eye->parentId == 1);
    REQUIRE(detachFrame.eye->frame == cigi_wire::EyeFrame::LLA);
    REQUIRE(detachFrame.eye->entityId == 0);
    REQUIRE(detachFrame.eye->parentId == 0);
    REQUIRE(attachFrame.frameCntr == 10);
    REQUIRE(detachFrame.frameCntr == 11);
}

namespace
{
    cigi_wire::EyePose hostEyePoseToWire(const HostEyePose& host)
    {
        cigi_wire::EyePose wire{};
        wire.x = host.position.x;
        wire.y = host.position.y;
        wire.z = host.position.z;
        wire.yawDeg = host.eulerYprDeg.x;
        wire.pitchDeg = host.eulerYprDeg.y;
        wire.rollDeg = host.eulerYprDeg.z;
        wire.frame = (host.frame == HostEyeCoordFrame::LLA) ? cigi_wire::EyeFrame::LLA
                                                            : cigi_wire::EyeFrame::WORLD_LOCAL;
        return wire;
    }
} // namespace

// lla位姿传输设计.md §5 / §7：Host 按 HostEyePose 位置类型选 Attach/Detach；线上无私有 frame 字段。
TEST_CASE("HostEyePose WorldLocal selects Attach on wire; frame recovered only from AttachState",
          "[unit][cigi][wire-contract][lla][host-eye]")
{
    HostEyePose host{};
    host.position = {10.0, 20.0, 30.0};
    host.eulerYprDeg = {5.0, 0.0, 0.0};
    host.frame = HostEyeCoordFrame::WORLD_LOCAL;

    const cigi_wire::EyePose wireIn = hostEyePoseToWire(host);
    std::vector<unsigned char> buf;
    REQUIRE(cigi_wire::packHostFrame(3, 0.0, &wireIn, buf));

    // 非握手/应用私有头——纯 CIGI 数据报（lla §5 无私有 frame 字段）。
    REQUIRE_FALSE(cigi_wire::isAvsyMagic(buf.data(), static_cast<int>(buf.size())));
    REQUIRE(buf.size() >= 8);

    cigi_wire::HostFrame frame{};
    REQUIRE(cigi_wire::unpackHostFrame(buf.data(), static_cast<int>(buf.size()), frame));
    REQUIRE(frame.eye.has_value());
    // 解包出的 EyeFrame 来自 AttachState（+ ParentID），不是私有 UDP 标志。
    REQUIRE(frame.eye->frame == cigi_wire::EyeFrame::WORLD_LOCAL);
    REQUIRE(frame.eye->entityId == 0);
    REQUIRE(frame.eye->parentId == 1);
    REQUIRE(frame.eye->x == Catch::Approx(host.position.x));
}

TEST_CASE("HostEyePose Lla selects Detach on wire; frame recovered only from AttachState",
          "[unit][cigi][wire-contract][lla][host-eye]")
{
    HostEyePose host{};
    host.position = {39.9, 116.4, 500.0};
    host.eulerYprDeg = {45.0, 10.0, 0.0};
    host.frame = HostEyeCoordFrame::LLA;

    const cigi_wire::EyePose wireIn = hostEyePoseToWire(host);
    std::vector<unsigned char> buf;
    REQUIRE(cigi_wire::packHostFrame(4, 0.0, &wireIn, buf));

    REQUIRE_FALSE(cigi_wire::isAvsyMagic(buf.data(), static_cast<int>(buf.size())));

    cigi_wire::HostFrame frame{};
    REQUIRE(cigi_wire::unpackHostFrame(buf.data(), static_cast<int>(buf.size()), frame));
    REQUIRE(frame.eye.has_value());
    REQUIRE(frame.eye->frame == cigi_wire::EyeFrame::LLA);
    REQUIRE(frame.eye->entityId == 0);
    REQUIRE(frame.eye->parentId == 0);
    REQUIRE(frame.eye->x == Catch::Approx(host.position.x));
    REQUIRE(frame.eye->y == Catch::Approx(host.position.y));
    REQUIRE(frame.eye->z == Catch::Approx(host.position.z));
}

TEST_CASE("wire EyeFrame is not carried as a private payload field besides AttachState",
          "[unit][cigi][wire-contract][lla][host-eye]")
{
    // 数字载荷相同；只有 HostEyePose.frame（→ AttachState）改变线上语义。
    HostEyePose asLocal{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0}, HostEyeCoordFrame::WORLD_LOCAL};
    HostEyePose asLla{{1.0, 2.0, 3.0}, {0.0, 0.0, 0.0}, HostEyeCoordFrame::LLA};

    std::vector<unsigned char> localBuf;
    std::vector<unsigned char> llaBuf;
    const auto localWire = hostEyePoseToWire(asLocal);
    auto llaWire = hostEyePoseToWire(asLla);
    // 有效中纬 LLA，使 Detach 边界检查通过（lat/lon/alt 取 1°/2°/3m 即可）。
    REQUIRE(cigi_wire::packHostFrame(5, 0.0, &localWire, localBuf));
    REQUIRE(cigi_wire::packHostFrame(6, 0.0, &llaWire, llaBuf));

    REQUIRE_FALSE(cigi_wire::isAvsyMagic(localBuf.data(), static_cast<int>(localBuf.size())));
    REQUIRE_FALSE(cigi_wire::isAvsyMagic(llaBuf.data(), static_cast<int>(llaBuf.size())));

    cigi_wire::HostFrame localFrame{};
    cigi_wire::HostFrame llaFrame{};
    REQUIRE(cigi_wire::unpackHostFrame(localBuf.data(), static_cast<int>(localBuf.size()), localFrame));
    REQUIRE(cigi_wire::unpackHostFrame(llaBuf.data(), static_cast<int>(llaBuf.size()), llaFrame));

    REQUIRE(localFrame.eye->frame == cigi_wire::EyeFrame::WORLD_LOCAL);
    REQUIRE(localFrame.eye->parentId == 1);
    REQUIRE(llaFrame.eye->frame == cigi_wire::EyeFrame::LLA);
    REQUIRE(llaFrame.eye->parentId == 0);
    // 判别是 Attach/ParentID，不是数据报里的额外应用级 frame 字节。
    REQUIRE(localFrame.eye->parentId != llaFrame.eye->parentId);
}

// =============================================================================
// 1. 连接面（集成；握手仍为自建 sync_proto，非 CIGI）
// =============================================================================

SCENARIO("Host initializes with no ready IG", "[integration][sync][initialize]")
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

SCENARIO("IG initializes disconnected from any Host", "[integration][sync][initialize]")
{
    GIVEN("a new IgSync")
    {
        IgSync ig;

        WHEN("it is initialized")
        {
            const bool ok = ig.initialize(makeIgLocal());

            THEN("initialization succeeds and it is not connected to a Host yet")
            {
                REQUIRE(ok);
                REQUIRE_FALSE(ig.tcpConnected());
                REQUIRE_FALSE(ig.udpSynced());
            }
        }
    }
}

SCENARIO("IG connect fails when Host is not running", "[integration][sync][connect][failure]")
{
    GIVEN("an IG initialized without a running Host")
    {
        IgSync ig;
        REQUIRE(ig.initialize(makeIgLocal()));

        WHEN("the IG connects to a Host endpoint")
        {
            const bool connected = ig.connect(makeIgLocal());

            THEN("connect fails and neither plane reports connected")
            {
                REQUIRE_FALSE(connected);
                REQUIRE_FALSE(ig.tcpConnected());
                REQUIRE_FALSE(ig.udpSynced());
            }
        }
    }
}

SCENARIO("IG connects successfully when Host is already waiting", "[integration][sync][connect]")
{
    GIVEN("a Host that has been initialized and is waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE_FALSE(host.hasReadyIg());

        AND_GIVEN("an IG that has been initialized")
        {
            IgSync ig;
            REQUIRE(ig.initialize(makeIgLocal()));

            WHEN("the IG connects to the Host endpoint")
            {
                const bool connected = ig.connect(makeIgLocal());

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

SCENARIO("IG disconnects when Host goes offline", "[integration][sync][connect][disconnect]")
{
    GIVEN("a connected Host and IG")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeIgLocal()));
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
         "[integration][sync][connect][failure]")
{
    GIVEN("a Host waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.initialize(makeHostLocal()));

        AND_GIVEN("an IG initialized with correct local ports")
        {
            IgSync ig;
            REQUIRE(ig.initialize(makeIgLocal()));

            WHEN("the IG connects using a Host target with wrong UDP ports")
            {
                // Connect 使用 targetUdpPortRecv 作为 Host UDP 收端口。
                IgConfig badUdpConfig{8000, 8001, "127.0.0.1", 8100, 9999};
                const bool connected = ig.connect(badUdpConfig);

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

SCENARIO("Host accepts multiple co-located IG connections", "[integration][sync][connect][multi-ig]")
{
    GIVEN("a Host waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.initialize(makeHostLocal()));

        WHEN("two co-located IGs initialize on distinct UDP recv ports and connect")
        {
            IgSync ig1;
            IgSync ig2;
            REQUIRE(ig1.initialize(makeIgLocal(8001)));
            REQUIRE(ig2.initialize(makeIgLocal(8003)));

            REQUIRE(ig1.connect(makeIgLocal(8001)));
            REQUIRE(ig2.connect(makeIgLocal(8003)));

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

// =============================================================================
// 2. 帧节拍：CIGI IGCtrl / SOF / FreeRun（集成；握手仍为 sync_proto）
// 数据面线格式契约：IGCtrlV4 (+ 可选 EntityPositionCtrlV4) / SOFV4 —— 见 [wire-contract]。
// =============================================================================

SCENARIO("connected Host and IG enter RUNNING and exchange CIGI IGCtrl each update",
         "[integration][sync][status][cigi]")
{
    GIVEN("a Host and an IG that have completed sync_proto handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeIgLocal()));

        WHEN("Host runs and sends 10 CIGI IGCtrl frames while IG updates")
        {
            host.run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                host.update(/*simTimeMs=*/i * (1000.0 / 60.0));
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

SCENARIO("IG replies with one CIGI SOF per received IGCtrl", "[integration][sync][status][sof][cigi]")
{
    GIVEN("a Host and an IG that have completed sync_proto handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeIgLocal()));

        WHEN("Host sends 10 CIGI IGCtrl and IG updates each frame (reply SOF)")
        {
            host.run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                host.update(/*simTimeMs=*/i * (1000.0 / 60.0));
                ig.update(/*sendSof=*/true);
            }

            THEN("SOF sent equals IGCtrl received; Host SOF count cannot exceed what IG sent")
            {
                REQUIRE(ig.igCtrlReceivedCount() >= 1);
                REQUIRE(ig.sofSentCount() == ig.igCtrlReceivedCount());
                REQUIRE(host.sofReceivedCount() <= ig.sofSentCount());
            }
        }
    }
}

SCENARIO("Host keeps sending CIGI IGCtrl when IG never replies SOF",
         "[integration][sync][status][freerun][cigi]")
{
    GIVEN("a connected Host and IG with FreeRun send pace (CIGI SOF does not gate)")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeIgLocal()));

        SyncPaceConfig pace{};
        pace.igCtrlSendPace = SendPace::FREE_RUN;
        pace.frameGate = FrameGate::FREE_RUN;
        host.setPaceConfig(pace);

        WHEN("Host sends 10 CIGI IGCtrl while IG receives but never replies SOF")
        {
            host.run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                host.update(/*simTimeMs=*/i * (1000.0 / 60.0));
                ig.update(/*sendSof=*/false);
            }

            THEN("Host sent all IGCtrl without depending on SOF")
            {
                REQUIRE(host.igCtrlSentCount() == kFrames);
                REQUIRE(host.sofReceivedCount() == 0);
                REQUIRE(ig.igCtrlReceivedCount() <= kFrames);
            }
        }
    }
}

SCENARIO("IG last received CIGI FrameCntr matches Host frame numbers",
         "[integration][sync][status][frame][cigi]")
{
    GIVEN("a Host and an IG that have completed sync_proto handshake")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeIgLocal()));

        WHEN("Host sends CIGI IGCtrl FrameCntr 0..N-1 and IG updates each frame")
        {
            host.run();
            constexpr int kFrames = 10;
            std::uint32_t prevReceived = 0;
            int matchedFrames = 0;

            for (int i = 0; i < kFrames; ++i)
            {
                // Host 本轮 CIGI IGCtrl.FrameCntr == i（经 HostSync API 暴露为 lastIgCtrlFrameCntr）
                host.update(/*simTimeMs=*/i * (1000.0 / 60.0));
                ig.update();

                if (ig.igCtrlReceivedCount() > prevReceived)
                {
                    REQUIRE(ig.lastIgCtrlFrameCntr() == static_cast<std::uint32_t>(i));
                    prevReceived = ig.igCtrlReceivedCount();
                    ++matchedFrames;
                }
            }

            THEN("at least one IGCtrl was received and FrameCntr values matched")
            {
                REQUIRE(matchedFrames >= 1);
                REQUIRE(ig.igCtrlReceivedCount() == static_cast<std::uint32_t>(matchedFrames));
                REQUIRE(ig.lastIgCtrlFrameCntr() < static_cast<std::uint32_t>(kFrames));
            }
        }
    }
}

// =============================================================================
// 3. Engine + SynchronSystem 集成（CIGI 帧交换契约；握手仍为 sync_proto）
// =============================================================================

SCENARIO("single Engine with Host and IG exchanges CIGI frame control over ticks",
         "[integration][sync][engine][cigi]")
{
    GIVEN("an offscreen Engine whose SynchronSystem owns both Host and IG")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        SyncRoleConfig syncRole{};
        syncRole.enableHost = true;
        syncRole.enableIg = true;
        syncRole.hostConfig = makeHostLocal();
        syncRole.igConfig = makeIgLocal();

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";

        WHEN("the Engine is initialized and tickOnFrame runs 10 times")
        {
            REQUIRE(engine.init(modelPath, syncRole));

            constexpr int kTicks = 10;
            for (int i = 0; i < kTicks; ++i)
                REQUIRE(engine.tickOnFrame());

            THEN("Host and IG exchanged CIGI IGCtrl/SOF via the Engine loop")
            {
                SynchronSystem& sync = engine.synchronSystem();
                HostSync& host = engine.hostSync();
                IgSync& ig = sync.igSync();

                REQUIRE(host.igCtrlSentCount() == kTicks);
                REQUIRE(approxAtMost(ig.igCtrlReceivedCount(), kTicks, 3));
                REQUIRE(ig.sofSentCount() == ig.igCtrlReceivedCount());
                REQUIRE(host.sofReceivedCount() <= ig.sofSentCount());
                REQUIRE(approxAtMost(host.sofReceivedCount(), kTicks, 3));
            }
        }
    }
}

SCENARIO("three Engines exchange CIGI frame control across one Host and three IGs",
         "[integration][sync][engine][multi-ig][cigi]")
{
    GIVEN("Engine A with Host+IG, and Engines B and C with IG only on distinct UDP ports")
    {
        Engine engineA;
        Engine engineB;
        Engine engineC;
        engineA.extent = engineB.extent = engineC.extent = {1920, 1080};
        engineA.showWindow = engineB.showWindow = engineC.showWindow = false;

        SyncRoleConfig roleA{};
        roleA.enableHost = true;
        roleA.enableIg = true;
        roleA.hostConfig = makeHostLocal();
        roleA.igConfig = makeIgLocal(8001);

        SyncRoleConfig roleB{};
        roleB.enableHost = false;
        roleB.enableIg = true;
        roleB.igConfig = makeIgLocal(8003);

        SyncRoleConfig roleC{};
        roleC.enableHost = false;
        roleC.enableIg = true;
        roleC.igConfig = makeIgLocal(8005);

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";

        WHEN("all three engines sync-connect, A loads graphics, then A leads 10 ticks")
        {
            REQUIRE(engineA.initSync(roleA));
            REQUIRE(engineB.initSync(roleB));
            REQUIRE(engineC.initSync(roleC));
            REQUIRE(engineA.hostSync().readyIgCount() == 3);
            REQUIRE(engineA.initGraphics(modelPath));

            constexpr int kTicks = 10;
            constexpr int kIgCount = 3;
            for (int i = 0; i < kTicks; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
                engineC.tickSync();
            }

            THEN("each IG got about N CIGI IGCtrl and A's Host got about N times 3 SOF")
            {
                HostSync& host = engineA.hostSync();
                IgSync& igA = engineA.synchronSystem().igSync();
                IgSync& igB = engineB.synchronSystem().igSync();
                IgSync& igC = engineC.synchronSystem().igSync();

                REQUIRE(host.igCtrlSentCount() == kTicks);
                REQUIRE(approxAtMost(igA.igCtrlReceivedCount(), kTicks, 3));
                REQUIRE(approxAtMost(igB.igCtrlReceivedCount(), kTicks, 3));
                REQUIRE(approxAtMost(igC.igCtrlReceivedCount(), kTicks, 3));
                REQUIRE(igA.sofSentCount() == igA.igCtrlReceivedCount());
                REQUIRE(igB.sofSentCount() == igB.igCtrlReceivedCount());
                REQUIRE(igC.sofSentCount() == igC.igCtrlReceivedCount());
                REQUIRE(approxAtMost(host.sofReceivedCount(), kTicks * kIgCount, 9));
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
    // HostEyePose 边界类型 DVec3 ↔ 测试内部 vsg::dvec3（sync 公开头零 vsg，测试工程内自转）。
    inline vsg::dvec3 toVsg(const aerovista::sync::DVec3& v)
    {
        return {v.x, v.y, v.z};
    }
    inline aerovista::sync::DVec3 toDVec3(const vsg::dvec3& v)
    {
        return {v.x, v.y, v.z};
    }

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

    void requireLookAtMatchesPose(Engine& engine, const aerovista::sync::DVec3& position,
                                  const aerovista::sync::DVec3& eulerYprDeg)
    {
        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);

        const vsg::dvec3 pos = toVsg(position);
        const vsg::dvec3 euler = toVsg(eulerYprDeg);
        const vsg::dvec3 expectedForward = rotateByEulerYprDeg(euler, vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dvec3 expectedUp = rotateByEulerYprDeg(euler, vsg::dvec3(0.0, 0.0, 1.0));
        const vsg::dvec3 expectedCenter = pos + expectedForward;

        REQUIRE(vsg::length(lookAt->eye - pos) < 1e-9);
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

    void requireLookAtMatchesLlaPose(Engine& engine, const vsg::EllipsoidModel& ellipsoid,
                                     const aerovista::sync::DVec3& lla, const aerovista::sync::DVec3& eulerYprDeg,
                                     double eyeEps = 1e-6, double dirEps = 1e-9)
    {
        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);

        const vsg::dvec3 llaV = toVsg(lla);
        const vsg::dvec3 euler = toVsg(eulerYprDeg);
        constexpr double kLookDistance = 1.0;
        const vsg::dvec3 forwardEnu = rotateByEulerYprDeg(euler, vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dvec3 upEnu = rotateByEulerYprDeg(euler, vsg::dvec3(0.0, 0.0, 1.0));
        const vsg::dmat4 localToWorld = ellipsoid.computeLocalToWorldTransform(llaV);

        const vsg::dvec3 expectedEye = ellipsoid.convertLatLongAltitudeToECEF(llaV);
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

    void requirePoseNear(const HostEyePose& actual, const HostEyePose& expected, double eps = 1e-6)
    {
        REQUIRE(vsg::length(toVsg(actual.position) - toVsg(expected.position)) < eps);
        REQUIRE(vsg::length(toVsg(actual.eulerYprDeg) - toVsg(expected.eulerYprDeg)) < eps);
    }

    HostEyePose hostEyePlusOffset(const HostEyePose& host, const OffsetDeg& offset)
    {
        // 与 SynchronSystem::compose 语义相同：刚性阵列旋转合成
        // R_ig = R_host · R_offset（lla设计 §3.4），不是分量式 YPR 相加。
        return SynchronSystem::compose(host, offset);
    }

    // Host 眼点用例使用独立端口，避免与 §1–3 默认 8000/8001 并行冲突。
    HostConfig makeHostLocalEye(int base = 18000)
    {
        return HostConfig{base + 1, base, base + 100};
    }

    IgConfig makeIgLocalEye(int udpRecvPort, int base = 18000)
    {
        return IgConfig{base, udpRecvPort, "127.0.0.1", base + 100, base};
    }

    SyncRoleConfig makeHostIgRole(int igUdpRecv, int base = 18000)
    {
        SyncRoleConfig role{};
        role.enableHost = true;
        role.enableIg = true;
        role.hostConfig = makeHostLocalEye(base);
        role.igConfig = makeIgLocalEye(igUdpRecv, base);
        return role;
    }

    SyncRoleConfig makeIgOnlyRole(int igUdpRecv, int base = 18000)
    {
        SyncRoleConfig role{};
        role.enableHost = false;
        role.enableIg = true;
        role.igConfig = makeIgLocalEye(igUdpRecv, base);
        return role;
    }
} // namespace

// -----------------------------------------------------------------------------
// 4.1 位姿 API 标尺（单元，非验收）
// -----------------------------------------------------------------------------

TEST_CASE("setCameraPose writes LookAt from position and euler YPR", "[unit][camera]")
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
    requireLookAtMatchesPose(engine, toDVec3(position), toDVec3(eulerYprDeg));
}

// lla位姿传输设计.md §3.3 / §4.1 / §7：有 EllipsoidModel 时 LLA+当地 YPR → ECEF LookAt。
TEST_CASE("setCameraPoseLla writes ECEF LookAt from LLA and local ENU YPR", "[unit][camera][lla]")
{
    Engine engine;
    engine.extent = {1920, 1080};
    engine.showWindow = false;

    // 模型内嵌 EllipsoidModel（此 API 标尺无需 coordFrame 注入）。
    const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
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
    requireLookAtMatchesLlaPose(engine, *ellipsoidPerspective->ellipsoidModel, toDVec3(lla), toDVec3(eulerYprDeg));
}

// lla位姿传输设计.md §3.5 / §7：LLA 本机往返（单机、无网络）。
TEST_CASE("setCameraPoseLla round-trips LLA and local YPR on one engine", "[unit][camera][lla][roundtrip]")
{
    Engine engine;
    engine.extent = {1920, 1080};
    engine.showWindow = false;

    const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
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
// 4.2 门控：未连接不覆盖 / 已连接覆盖（验收行为；注入仅作测试手段）
// -----------------------------------------------------------------------------

SCENARIO("unlinked IG does not apply Host eye to the camera",
         "[acceptance][bdd][sync][hostctrl][gate]")
{
    GIVEN("an Engine with graphics whose IG is not linked to a Host")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath));

        SyncRoleConfig role{};
        role.enableHost = false;
        role.enableIg = true;
        role.igConfig = makeIgLocalEye(18001, 18000);
        // Host 未启动 → Connect 失败，但仍完成本地 Init。
        REQUIRE(engine.synchronSystem().initialize(role, SyncSystemConfig{/*requireConnectedIg=*/false}));
        REQUIRE_FALSE(engine.synchronSystem().igLinked());

        const HostEyePose localPose{{1.0, 2.0, 3.0}, {10.0, 0.0, 0.0}};
        const HostEyePose hostPose{{100.0, 200.0, 50.0}, {45.0, 0.0, 0.0}};
        REQUIRE(engine.setCameraPose(toVsg(localPose.position), toVsg(localPose.eulerYprDeg)));

        WHEN("a Host eye becomes available and sync update runs")
        {
            // 测试手法：queue 注入，绕过真报文，只钉门控行为。
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.stepSync();

            THEN("camera stays at the local pose")
            {
                requireLookAtMatchesPose(engine, localPose.position, localPose.eulerYprDeg);
            }
        }
    }
}

SCENARIO("linked IG applies Host eye to the camera", "[acceptance][bdd][sync][hostctrl][gate]")
{
    GIVEN("an Engine whose SynchronSystem is Host+IG and linked")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(18001, 18000)));
        REQUIRE(engine.synchronSystem().igLinked());

        const HostEyePose localPose{{1.0, 2.0, 3.0}, {10.0, 0.0, 0.0}};
        const HostEyePose hostPose{{100.0, 200.0, 50.0}, {45.0, 0.0, 0.0}};
        REQUIRE(engine.setCameraPose(toVsg(localPose.position), toVsg(localPose.eulerYprDeg)));

        WHEN("a Host eye becomes available and sync update runs")
        {
            engine.synchronSystem().setOffsetDeg({});
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.stepSync();

            THEN("camera matches the Host eye")
            {
                requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYprDeg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.3 位姿合成：Host 眼点 ⊕ offsetDeg（旋转复合，刚性阵列，见 lla设计 §3.4）
// -----------------------------------------------------------------------------

SCENARIO("linked IG with zero offset keeps Host eye unchanged",
         "[acceptance][bdd][sync][hostctrl][offset]")
{
    GIVEN("a linked Host+IG Engine with channel offset all zero")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(18001, 18100)));
        engine.synchronSystem().setOffsetDeg({0.0, 0.0, 0.0});

        const HostEyePose hostPose{{10.0, -5.0, 2.0}, {0.0, 0.0, 0.0}};

        WHEN("a Host eye becomes available and sync update runs")
        {
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.stepSync();

            THEN("final pose equals Host eye")
            {
                requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYprDeg);
            }
        }
    }
}

SCENARIO("linked IG applies Host eye plus channel offset",
         "[acceptance][bdd][sync][hostctrl][offset]")
{
    GIVEN("a linked Host+IG Engine with yaw offset -60 deg")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(18001, 18200)));

        OffsetDeg offset{-60.0, 0.0, 0.0};
        engine.synchronSystem().setOffsetDeg(offset);

        const HostEyePose hostPose{{10.0, -5.0, 2.0}, {30.0, 5.0, 1.0}};
        const HostEyePose expected = hostEyePlusOffset(hostPose, offset);

        WHEN("a Host eye becomes available and sync update runs")
        {
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.stepSync();

            THEN("camera matches Host position composed with channel offset (rigid-array rotation)")
            {
                requireLookAtMatchesPose(engine, expected.position, expected.eulerYprDeg);
            }
        }
    }
}

// lla位姿传输设计.md §3.4 / §7：offsetDeg 仅 yaw；多通道刚性阵列偏移 R_ig=R_host*Rz(δ)，
// yaw 偏移绕 Host 自身 up 轴，roll 下各通道 up 轴保持平行（frustum 贴边）。
SCENARIO("yaw-only offset with Host pitch/roll keeps channel up parallel to Host up",
         "[acceptance][bdd][sync][hostctrl][offset]")
{
    GIVEN("a linked Host+IG Engine, Host eye with pitch/roll, channel offset yaw-only")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(18001, 18250)));

        constexpr double kDeltaYawDeg = 18.05;
        OffsetDeg offset{kDeltaYawDeg, 0.0, 0.0};
        engine.synchronSystem().setOffsetDeg(offset);

        // 非零 roll 覆盖刚性阵列不变量（up 保持平行）。
        const HostEyePose hostPose{{5.0, -3.0, 2.0}, {20.0, 15.0, -8.0}};

        WHEN("the Host eye is applied with that yaw-only channel offset")
        {
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.stepSync();

            THEN("LookAt forward/up equal R_host*Rz(delta) (up stays parallel to Host up)")
            {
                auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
                REQUIRE(lookAt);

                // R_ig = R_host·Rz(δ) ⟹ forward_ig = R_host·(Rz(δ)·ŷ), up_ig = R_host·ẑ = up_host.
                const vsg::dquat rHost = quatFromEulerYprDeg(toVsg(hostPose.eulerYprDeg));
                const vsg::dquat rzDelta =
                    vsg::dquat(vsg::radians(kDeltaYawDeg), vsg::dvec3(0.0, 0.0, 1.0));
                const vsg::dvec3 expectedForward =
                    vsg::normalize(rHost * (rzDelta * vsg::dvec3(0.0, 1.0, 0.0)));
                const vsg::dvec3 expectedUp = vsg::normalize(rHost * vsg::dvec3(0.0, 0.0, 1.0));

                const vsg::dvec3 actualForward = vsg::normalize(lookAt->center - lookAt->eye);
                const vsg::dvec3 actualUp = vsg::normalize(lookAt->up);

                REQUIRE(vsg::length(actualForward - expectedForward) < 1e-9);
                REQUIRE(vsg::length(actualUp - expectedUp) < 1e-9);
                REQUIRE(vsg::length(lookAt->eye - toVsg(hostPose.position)) < 1e-9);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.4 无新包策略与断线
// -----------------------------------------------------------------------------

SCENARIO("ReuseLast re-applies cached Host eye when no new eye arrives",
         "[acceptance][bdd][sync][hostctrl][stale]")
{
    GIVEN("a linked Engine with ReuseLast after one Host eye was applied")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(18001, 18300)));
        engine.synchronSystem().setHostEyeStalePolicy(HostEyeStalePolicy::REUSE_LAST);
        engine.synchronSystem().setOffsetDeg({});

        const HostEyePose hostPose{{8.0, 9.0, 10.0}, {15.0, 0.0, 0.0}};
        engine.synchronSystem().queueHostEyePose(hostPose);
        engine.stepSync();
        requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYprDeg);

        WHEN("local pose is changed and update runs without a new Host eye")
        {
            REQUIRE(engine.setCameraPose(vsg::dvec3{0.0, 0.0, 0.0}, vsg::dvec3{0.0, 0.0, 0.0}));
            engine.stepSync();

            THEN("camera returns to the cached Host eye")
            {
                requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYprDeg);
            }
        }
    }
}

SCENARIO("Freeze leaves camera unchanged when no new Host eye arrives",
         "[acceptance][bdd][sync][hostctrl][stale]")
{
    GIVEN("a linked Engine with Freeze after one Host eye was applied")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(18001, 18400)));
        engine.synchronSystem().setHostEyeStalePolicy(HostEyeStalePolicy::FREEZE);
        engine.synchronSystem().setOffsetDeg({});

        const HostEyePose hostPose{{8.0, 9.0, 10.0}, {15.0, 0.0, 0.0}};
        engine.synchronSystem().queueHostEyePose(hostPose);
        engine.stepSync();
        requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYprDeg);

        const HostEyePose localPose{{0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}};

        WHEN("local pose is changed and update runs without a new Host eye")
        {
            REQUIRE(engine.setCameraPose(toVsg(localPose.position), toVsg(localPose.eulerYprDeg)));
            engine.stepSync();

            THEN("camera stays at the local pose")
            {
                requireLookAtMatchesPose(engine, localPose.position, localPose.eulerYprDeg);
            }
        }
    }
}

SCENARIO("after disconnect, camera keeps the last Host eye pose",
         "[acceptance][bdd][sync][hostctrl][disconnect]")
{
    GIVEN("a linked Engine that applied a Host eye then lost the IG link")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(18001, 18500)));
        engine.synchronSystem().setOffsetDeg({});

        const HostEyePose hostPose{{20.0, 30.0, 40.0}, {25.0, 0.0, 0.0}};
        engine.synchronSystem().queueHostEyePose(hostPose);
        engine.stepSync();
        REQUIRE(engine.synchronSystem().igLinked());

        engine.synchronSystem().igSync().shutdown();
        REQUIRE_FALSE(engine.synchronSystem().igLinked());

        WHEN("local pose is changed and update runs while disconnected")
        {
            REQUIRE(engine.setCameraPose(vsg::dvec3{0.0, 0.0, 0.0}, vsg::dvec3{90.0, 0.0, 0.0}));
            engine.stepSync();

            THEN("camera is restored to the last Host eye")
            {
                requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYprDeg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.5 权威窗同样回灌（无旁路）
// -----------------------------------------------------------------------------

SCENARIO("Host-local IG channel also applies Host eye to its camera",
         "[acceptance][bdd][sync][hostctrl][authority]")
{
    GIVEN("an Engine with Host+IG linked (authority window)")
    {
        Engine engineA;
        engineA.extent = {1920, 1080};
        engineA.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engineA.init(modelPath, makeHostIgRole(18001, 18600)));
        REQUIRE(engineA.hasHost());
        REQUIRE(engineA.synchronSystem().igLinked());

        const HostEyePose localPose{{1.0, 1.0, 1.0}, {5.0, 0.0, 0.0}};
        const HostEyePose hostPose{{50.0, 60.0, 70.0}, {12.0, 0.0, 0.0}};
        REQUIRE(engineA.setCameraPose(toVsg(localPose.position), toVsg(localPose.eulerYprDeg)));

        WHEN("a Host eye becomes available and sync update runs")
        {
            engineA.synchronSystem().setOffsetDeg({});
            engineA.synchronSystem().queueHostEyePose(hostPose);
            engineA.stepSync();

            THEN("camera matches Host eye with no authority bypass")
            {
                requireLookAtMatchesPose(engineA, hostPose.position, hostPose.eulerYprDeg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.6 端到端验收：真连接 + CIGI 数据面真报文（IGCtrl + EntityPosition）+ 多通道 offset / 防回声
// 握手仍为 sync_proto；眼点在 EntityPositionCtrl（Attach XYZ，本地世界系临时约定）。
// -----------------------------------------------------------------------------

SCENARIO("remote IG applies Host eye from live CIGI packets with channel offset",
         "[acceptance][bdd][sync][hostctrl][e2e][cigi]")
{
    GIVEN("Engine A as Host+IG with graphics and Engine B as IG-only sync")
    {
        constexpr int kBase = 19000;
        Engine engineA;
        Engine engineB;
        engineA.extent = {1920, 1080};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(modelPath));

        OffsetDeg offsetB{60.0, 0.0, 0.0};
        engineA.synchronSystem().setOffsetDeg({});
        engineB.synchronSystem().setOffsetDeg(offsetB);

        const HostEyePose intentPose{{11.0, 22.0, 33.0}, {40.0, 0.0, 0.0}};
        const HostEyePose expectedB = hostEyePlusOffset(intentPose, offsetB);

        WHEN("A publishes authority pose and both engines tick (CIGI IGCtrl+EntityPosition fan-out)")
        {
            REQUIRE(engineA.setCameraPose(toVsg(intentPose.position), toVsg(intentPose.eulerYprDeg)));
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();

            THEN("B applied pose matches Host intent plus B offset")
            {
                auto applied = engineB.synchronSystem().lastAppliedHostEye();
                REQUIRE(applied.has_value());
                requirePoseNear(*applied, expectedB);
            }
        }
    }
}

SCENARIO("Host fans out the new authority pose instead of an echoed old pose",
         "[acceptance][bdd][sync][hostctrl][e2e][anti-echo][cigi]")
{
    GIVEN("linked A(Host+IG graphics) and B(IG sync-only) with prior Host eye Pose_old")
    {
        constexpr int kBase = 19100;
        Engine engineA;
        Engine engineB;
        engineA.extent = {1920, 1080};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.initGraphics(modelPath));

        engineA.synchronSystem().setOffsetDeg({});
        engineB.synchronSystem().setOffsetDeg({});

        const HostEyePose poseOld{{1.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
        const HostEyePose poseNew{{9.0, 8.0, 7.0}, {35.0, 0.0, 0.0}};

        // 建立 Pose_old 为当前权威（应用层注入；E2E 断言走后续真 tick 的 CIGI 扇出）。
        engineA.synchronSystem().queueHostEyePose(poseOld);
        engineA.stepSync();
        engineB.synchronSystem().queueHostEyePose(poseOld);
        engineB.stepSync();

        WHEN("A camera becomes Pose_new then a full tick fans out to B via CIGI")
        {
            REQUIRE(engineA.setCameraPose(toVsg(poseNew.position), toVsg(poseNew.eulerYprDeg)));
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();

            THEN("B follows Pose_new")
            {
                auto applied = engineB.synchronSystem().lastAppliedHostEye();
                REQUIRE(applied.has_value());
                requirePoseNear(*applied, poseNew);
            }

            AND_THEN("Host last sent authority eye is Pose_new when observable")
            {
                auto sent = engineA.lastSentHostEye();
                REQUIRE(sent.has_value());
                requirePoseNear(*sent, poseNew);
            }
        }
    }
}

SCENARIO("three channels share Host eye and differ only by channel offset",
         "[acceptance][bdd][sync][hostctrl][e2e][multi-ig][cigi]")
{
    GIVEN("A Host+IG with graphics, B and C IG-only with yaw offsets -60 / +60")
    {
        constexpr int kBase = 19200;
        Engine engineA;
        Engine engineB;
        Engine engineC;
        engineA.extent = {1920, 1080};
        engineA.showWindow = engineB.showWindow = engineC.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineC.initSync(makeIgOnlyRole(kBase + 5, kBase)));
        REQUIRE(engineA.hostSync().readyIgCount() == 3);
        REQUIRE(engineA.initGraphics(modelPath));

        OffsetDeg offsetA{0.0, 0.0, 0.0};
        OffsetDeg offsetB{-60.0, 0.0, 0.0};
        OffsetDeg offsetC{60.0, 0.0, 0.0};
        engineA.synchronSystem().setOffsetDeg(offsetA);
        engineB.synchronSystem().setOffsetDeg(offsetB);
        engineC.synchronSystem().setOffsetDeg(offsetC);

        const HostEyePose intent{{5.0, 6.0, 7.0}, {0.0, 0.0, 0.0}};
        const HostEyePose expectA = hostEyePlusOffset(intent, offsetA);
        const HostEyePose expectB = hostEyePlusOffset(intent, offsetB);
        const HostEyePose expectC = hostEyePlusOffset(intent, offsetC);

        WHEN("A publishes intent and all channels tick (shared CIGI eye, local offsetDeg)")
        {
            REQUIRE(engineA.setCameraPose(toVsg(intent.position), toVsg(intent.eulerYprDeg)));
            for (int i = 0; i < 2; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
                engineC.tickSync();
            }

            THEN("each channel pose matches Host intent plus its own offset")
            {
                requireLookAtMatchesPose(engineA, expectA.position, expectA.eulerYprDeg);
                auto appliedB = engineB.synchronSystem().lastAppliedHostEye();
                auto appliedC = engineC.synchronSystem().lastAppliedHostEye();
                REQUIRE(appliedB.has_value());
                REQUIRE(appliedC.has_value());
                requirePoseNear(*appliedB, expectB);
                requirePoseNear(*appliedC, expectC);
            }
        }
    }
}

// =============================================================================
// 5. LLA / 椭球 Host↔IG 部署一致性（lla位姿传输设计.md §2.4 / §2.5 / §4.5 / §7）
// 冒烟：对齐半径；错配：模式拒收；半径不一致：跟拍 ECEF 超差（已知错配，禁止默默绿过）。
// =============================================================================

namespace
{
    // 刚性阵列 E2E 的通道配置 JSON。coordFrame 显式声明坐标系（lla设计 §2.2）；
    // Host+IG 通道（isHost）带 hostConfig；纯 IG 只带 igConfig（自包含远端目标，§3.1）。
    // TempConfigFile 来自公共头 Common.h。
    std::string makeChannelConfigBody(int kBase, int channelId, int udpRecv, double yawOffset, bool isHost,
                                      const std::string& coordFrame, const std::string& model)
    {
        std::string body = std::string(R"({
              "syncSystem": {
              "channelId": )") +
                           std::to_string(channelId) + R"(,
              "offsetDeg": { "yaw": )" +
                           std::to_string(yawOffset) + R"(, "pitch": 0.0, "roll": 0.0 },
              "requireConnectedIg": )" +
                           (isHost ? std::string("true") : std::string("false")) + R"(
              },
              "coordFrame": ")" +
                           coordFrame + R"(",
              "igConfig": { "udpPortSend": )" +
                           std::to_string(kBase) +
                           R"(, "udpPortRecv": )" + std::to_string(udpRecv) +
                           R"(, "targetAddr": "127.0.0.1", "targetTcpPort": )" + std::to_string(kBase + 100) +
                           R"(, "targetUdpPortRecv": )" + std::to_string(kBase) + R"( },)";
        if (isHost)
        {
            body += R"(
              "hostConfig": { "udpPortSend": )" +
                    std::to_string(kBase + 1) +
                    R"(, "udpPortRecv": )" + std::to_string(kBase) + R"(, "tcpPort": )" +
                    std::to_string(kBase + 100) + R"( },)";
        }
        body += R"(
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

    // 三通道刚性阵列 E2E 的公共装载：A（Host+IG，offset 0）+ B/C（IG-only，yaw ±60），
    // 全部经配置文件驱动，coordFrame 显式声明坐标系。B/C 走 sync + scene mode only。
    struct RigidArrayHarness
    {
        Engine a;
        Engine b;
        Engine c;

        RigidArrayHarness(int kBase, const std::string& coordFrame, const std::string& model)
        {
            const TempConfigFile hostFile(
                makeChannelConfigBody(kBase, 0, kBase + 1, 0.0, /*isHost=*/true, coordFrame, model));
            const TempConfigFile igBFile(
                makeChannelConfigBody(kBase, 1, kBase + 3, -60.0, /*isHost=*/false, coordFrame, model));
            const TempConfigFile igCFile(
                makeChannelConfigBody(kBase, 2, kBase + 5, 60.0, /*isHost=*/false, coordFrame, model));

            a.extent = {1920, 1080};
            a.showWindow = b.showWindow = c.showWindow = false;

            REQUIRE(a.loadConfig(hostFile.path()));
            REQUIRE(b.loadConfig(igBFile.path()));
            REQUIRE(c.loadConfig(igCFile.path()));
            REQUIRE(a.init());
            // B/C：sync + scene mode only（单进程避免第三个 Vulkan Device）。
            REQUIRE(b.initSync(b.config.toSyncRole(), b.config.syncSystem.requireConnectedIg));
            REQUIRE(b.initSceneMode(vsg::Path(RESOURCE_DIR) / b.config.model));
            b.synchronSystem().setOffsetDeg(b.config.syncSystem.offsetDeg);
            REQUIRE(c.initSync(c.config.toSyncRole(), c.config.syncSystem.requireConnectedIg));
            REQUIRE(c.initSceneMode(vsg::Path(RESOURCE_DIR) / c.config.model));
            c.synchronSystem().setOffsetDeg(c.config.syncSystem.offsetDeg);

            // 握手是异步网络时序：Windows 下偶发 UDP_SYNC 丢包/调度延迟会让 connect 失败。
            // B/C 的 requireConnectedIg=false 会静默继续，此时对未同步的 IG 重连并等待全部 ready。
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
            while (std::chrono::steady_clock::now() < deadline)
            {
                if (a.hostSync().readyIgCount() == 3)
                    return;
                for (Engine* ig : {&b, &c})
                {
                    if (ig->synchronSystem().hasIg() && !ig->synchronSystem().igSync().udpSynced())
                        ig->synchronSystem().igSync().connect(ig->config.igConfig);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            REQUIRE(a.hostSync().readyIgCount() == 3);
        }

        void tick(const int frames = 2)
        {
            for (int i = 0; i < frames; ++i)
            {
                REQUIRE(a.tickOnFrame());
                b.tickSync();
                c.tickSync();
            }
        }
    };

    // 本地刚性断言：B/C up 轴与 Host up 平行、forward == R_host·Rz(δ)（1e-4，覆盖 CIGI float 量化）。
    void requireLocalRigidArray(RigidArrayHarness& h, const HostEyePose& intent)
    {
        auto lookAtA = h.a.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAtA);
        const vsg::dvec3 hostUp = vsg::normalize(lookAtA->up);

        const vsg::dquat rHost = quatFromEulerYprDeg(toVsg(intent.eulerYprDeg));
        const vsg::dvec3 expectedHostUp = vsg::normalize(rHost * vsg::dvec3(0.0, 0.0, 1.0));

        const auto check = [&](const std::optional<HostEyePose>& applied, const OffsetDeg& offset) {
            REQUIRE(applied.has_value());
            // B/C 为 sync-only（无相机），从 _lastApplied 的 YPR 计算相机基。
            const vsg::dvec3 up =
                vsg::normalize(rotateByEulerYprDeg(toVsg(applied->eulerYprDeg), vsg::dvec3(0.0, 0.0, 1.0)));
            const vsg::dvec3 forward =
                vsg::normalize(rotateByEulerYprDeg(toVsg(applied->eulerYprDeg), vsg::dvec3(0.0, 1.0, 0.0)));

            const vsg::dquat rzDelta = vsg::dquat(vsg::radians(offset.yaw), vsg::dvec3(0.0, 0.0, 1.0));
            const vsg::dvec3 expectedForward = vsg::normalize(rHost * (rzDelta * vsg::dvec3(0.0, 1.0, 0.0)));

            constexpr double kDirEps = 1e-4;
            REQUIRE(vsg::length(up - hostUp) < kDirEps);               // 与 Host up 平行（刚性阵列贴边）
            REQUIRE(vsg::length(up - expectedHostUp) < kDirEps);       // up == R_host·ẑ
            REQUIRE(vsg::length(forward - expectedForward) < kDirEps); // forward == R_host·Rz(δ)·ŷ
        };
        check(h.b.synchronSystem().lastAppliedHostEye(), h.b.config.syncSystem.offsetDeg);
        check(h.c.synchronSystem().lastAppliedHostEye(), h.c.config.syncSystem.offsetDeg);
    }

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

        const auto check = [&](const std::optional<HostEyePose>& applied, const OffsetDeg& offset, Engine& ig) {
            REQUIRE(applied.has_value());
            REQUIRE(applied->frame == HostEyeCoordFrame::LLA);
            // B/C 为 sync-only（无相机），从 _lastApplied 的 ENU YPR 经各自椭球转到 ECEF。
            auto em = ig.ellipsoidModel();
            REQUIRE(em);
            const vsg::dmat4 l2w = em->computeLocalToWorldTransform(toVsg(applied->position));
            const vsg::dvec3 upEcef =
                vsg::normalize(rotateEnuToEcef(l2w, rotateByEulerYprDeg(toVsg(applied->eulerYprDeg), vsg::dvec3(0.0, 0.0, 1.0))));
            const vsg::dvec3 forwardEcef =
                vsg::normalize(rotateEnuToEcef(l2w, rotateByEulerYprDeg(toVsg(applied->eulerYprDeg), vsg::dvec3(0.0, 1.0, 0.0))));

            const vsg::dquat rzDelta = vsg::dquat(vsg::radians(offset.yaw), vsg::dvec3(0.0, 0.0, 1.0));
            const vsg::dvec3 forwardEnuExpected = vsg::normalize(rHost * (rzDelta * vsg::dvec3(0.0, 1.0, 0.0)));
            const vsg::dvec3 forwardEcefExpected = vsg::normalize(rotateEnuToEcef(l2w, forwardEnuExpected));

            // CIGI YPR 走 float 量化 + ENU→ECEF 换算，方向容差放 1e-4。
            constexpr double kDirEps = 1e-4;
            REQUIRE(vsg::length(upEcef - hostUpEcef) < kDirEps);               // 与 Host up 平行（刚性阵列贴边）
            REQUIRE(vsg::length(upEcef - upEcefExpected) < kDirEps);           // up == R_host·Up（ECEF）
            REQUIRE(vsg::length(forwardEcef - forwardEcefExpected) < kDirEps); // forward == R_host·Rz(δ)·ŷ（ECEF）
        };
        check(h.b.synchronSystem().lastAppliedHostEye(), h.b.config.syncSystem.offsetDeg, h.b);
        check(h.c.synchronSystem().lastAppliedHostEye(), h.c.config.syncSystem.offsetDeg, h.c);
    }

    vsg::dvec3 lookAtEye(Engine& engine)
    {
        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);
        return lookAt->eye;
    }
} // namespace

// lla设计 §3.4 / 多通道同步设计 §4.5：本地笛卡尔（coordFrame: "Local"）下，真 CIGI 报文
// + Host 非零 roll 时，各通道 up 轴与 Host up 平行（刚性阵列一起 roll、frustum 贴边），
// 且 forward 满足 R_ig=R_host·Rz(δ)。线上 roll 撕裂 bug 的 E2E 回归。
SCENARIO("three channels keep up axes parallel to Host when it rolls over live CIGI",
         "[acceptance][bdd][sync][hostctrl][e2e][multi-ig][cigi][rigid]")
{
    GIVEN("Host+IG A and IG-only B/C all on coordFrame Local, yaw offsets -60/+60")
    {
        RigidArrayHarness h(19350, "Local", "models/teapot.vsgt");
        REQUIRE_FALSE(h.a.ellipsoidModel());
        REQUIRE_FALSE(h.b.ellipsoidModel());
        REQUIRE_FALSE(h.c.ellipsoidModel());

        // Host 带非零 roll：回归点——刚性阵列必须整体滚转，up 轴保持平行。
        const HostEyePose intent{{5.0, 6.0, 7.0}, {30.0, 12.0, -18.0}};

        WHEN("A publishes the rolled intent and all channels tick (shared CIGI Attach eye, local offsetDeg)")
        {
            REQUIRE(h.a.setCameraPose(toVsg(intent.position), toVsg(intent.eulerYprDeg)));
            h.tick();

            THEN("B/C up axes stay parallel to Host up and forwards equal R_host*Rz(delta)")
            {
                requireLocalRigidArray(h, intent);
            }
        }
    }
}

SCENARIO("Host LLA eye is followed by IG LookAt ECEF on aligned ellipsoids",
         "[acceptance][bdd][sync][lla][follow]")
{
    GIVEN("Host+IG Engine A and IG-only Engine B both on readymap with zero offset")
    {
        constexpr int kBase = 19500;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(modelPath));
        REQUIRE(engineB.initSceneMode(modelPath)); // sync-only IG：单进程避免第二个 Vulkan Device

        auto emA = ellipsoidOf(engineA);
        REQUIRE(emA);
        REQUIRE(engineB.ellipsoidModel());
        auto emB = engineB.ellipsoidModel();
        REQUIRE(emB);
        REQUIRE(radiiEqual(*emA, *emB));

        engineA.synchronSystem().setOffsetDeg({});
        engineB.synchronSystem().setOffsetDeg({});

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 ypr{30.0, 5.0, 0.0};
        const vsg::dvec3 expectedEcef = emA->convertLatLongAltitudeToECEF(lla);

        WHEN("A publishes LLA authority eye over live CIGI and both tick")
        {
            REQUIRE(engineA.setCameraPoseLla(lla, ypr));
            for (int i = 0; i < 3; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }

            THEN("B applies Host LLA; A LookAt.eye matches ECEF (lla Host-IG follow)")
            {
                constexpr double kEcefEps = 1e-2; // meter-scale ECEF tolerance (design §4.4 band)
                auto applied = engineB.synchronSystem().lastAppliedHostEye();
                REQUIRE(applied.has_value());
                REQUIRE(applied->frame == HostEyeCoordFrame::LLA);
                REQUIRE(vsg::length(toVsg(applied->position) - lla) < 1e-6);
                REQUIRE(vsg::length(lookAtEye(engineA) - expectedEcef) < kEcefEps);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// lla设计 §3.4 / §7：椭球 offsetDeg — 刚性阵列旋转复合 R_ig=R_host·Rz(δ)（绕 Host ENU up）
// -----------------------------------------------------------------------------

SCENARIO("ellipsoid zero offset keeps Host LLA eye unchanged",
         "[acceptance][bdd][sync][lla][offset]")
{
    GIVEN("a linked Host+IG Engine on readymap with channel offset all zero")
    {
        constexpr int kBase = 20000;
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(kBase + 1, kBase)));
        auto em = ellipsoidOf(engine);
        REQUIRE(em);

        engine.synchronSystem().setOffsetDeg({0.0, 0.0, 0.0});

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 ypr{25.0, 8.0, -3.0};
        const HostEyePose hostPose{toDVec3(lla), toDVec3(ypr), HostEyeCoordFrame::LLA};

        WHEN("a Host LLA eye becomes available and sync update runs")
        {
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.stepSync();

            THEN("LookAt matches Host LLA + ENU YPR (no channel yaw)")
            {
                requireLookAtMatchesLlaPose(engine, *em, toDVec3(lla), toDVec3(ypr), 1e-2, 1e-6);
            }
        }
    }
}

SCENARIO("ellipsoid IG applies Host LLA eye plus yaw-only ENU offset",
         "[acceptance][bdd][sync][lla][offset]")
{
    GIVEN("a linked Host+IG Engine on readymap with yaw offset -60 deg")
    {
        constexpr int kBase = 20020;
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(kBase + 1, kBase)));
        auto em = ellipsoidOf(engine);
        REQUIRE(em);

        OffsetDeg offset{-60.0, 0.0, 0.0};
        engine.synchronSystem().setOffsetDeg(offset);

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const HostEyePose hostPose{toDVec3(lla), {30.0, 5.0, 1.0}, HostEyeCoordFrame::LLA};
        const HostEyePose expected = hostEyePlusOffset(hostPose, offset);

        WHEN("a Host LLA eye becomes available and sync update runs")
        {
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.stepSync();

            THEN("LookAt uses Host LLA composed with ENU yaw offset (rigid-array rotation)")
            {
                REQUIRE(expected.frame == HostEyeCoordFrame::LLA);
                REQUIRE(vsg::length(toVsg(expected.position) - lla) < 1e-12);
                requireLookAtMatchesLlaPose(engine, *em, expected.position, expected.eulerYprDeg, 1e-2, 1e-6);
            }
        }
    }
}

SCENARIO("ellipsoid yaw-only offset keeps channel up parallel to Host up (R_ig=R_host*Rz(delta))",
         "[acceptance][bdd][sync][lla][offset]")
{
    GIVEN("a linked Host+IG Engine on readymap; Host LLA eye has pitch/roll; channel yaw-only")
    {
        constexpr int kBase = 20040;
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(kBase + 1, kBase)));
        auto em = ellipsoidOf(engine);
        REQUIRE(em);

        constexpr double kDeltaYawDeg = 18.05;
        OffsetDeg offset{kDeltaYawDeg, 0.0, 0.0};
        engine.synchronSystem().setOffsetDeg(offset);

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        // 非零 roll 覆盖刚性阵列不变量：up 与 Host up 保持平行。
        const HostEyePose hostPose{toDVec3(lla), {20.0, 15.0, -8.0}, HostEyeCoordFrame::LLA};

        WHEN("the Host LLA eye is applied with that yaw-only channel offset")
        {
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.stepSync();

            THEN("LookAt matches Host LLA composed with R_ig=R_host*Rz(delta) for yaw-only offset")
            {
                const HostEyePose expected = hostEyePlusOffset(hostPose, offset);
                requireLookAtMatchesLlaPose(engine, *em, expected.position, expected.eulerYprDeg, 1e-2, 1e-6);
            }
        }
    }
}

SCENARIO("remote IG follows Host LLA with channel yaw offset over CIGI",
         "[acceptance][bdd][sync][lla][offset][e2e][cigi]")
{
    GIVEN("Host+IG A (offset 0) and IG-only B (yaw +60) both on readymap")
    {
        constexpr int kBase = 20060;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(modelPath));
        REQUIRE(engineB.initSceneMode(modelPath));

        REQUIRE(engineB.ellipsoidModel());
        REQUIRE(radiiEqual(*ellipsoidOf(engineA), *engineB.ellipsoidModel()));

        OffsetDeg offsetB{60.0, 0.0, 0.0};
        engineA.synchronSystem().setOffsetDeg({});
        engineB.synchronSystem().setOffsetDeg(offsetB);

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 yprHost{30.0, 5.0, 0.0};
        const HostEyePose intent{toDVec3(lla), toDVec3(yprHost), HostEyeCoordFrame::LLA};
        const HostEyePose expectedB = hostEyePlusOffset(intent, offsetB);

        WHEN("A publishes LLA authority eye over live CIGI and both tick")
        {
            REQUIRE(engineA.setCameraPoseLla(lla, yprHost));
            for (int i = 0; i < 3; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }

            THEN("B applied pose matches Host LLA with ENU YPR plus B yaw offset")
            {
                auto applied = engineB.synchronSystem().lastAppliedHostEye();
                REQUIRE(applied.has_value());
                REQUIRE(applied->frame == HostEyeCoordFrame::LLA);
                requirePoseNear(*applied, expectedB, 1e-3);
            }
        }
    }
}

// lla设计 §3.4 / 多通道同步设计 §4.5：ECEF/椭球下，真 CIGI 报文 + Host 非零 roll 时，
// 各通道 up 轴与 Host up 平行（刚性阵列一起 roll、frustum 贴边），且 forward 满足
// R_ig=R_host·Rz(δ)。本地刚性用例的 ECEF 对应物；同时经配置文件 `coordFrame: "Ellipsoid"`
// 驱动坐标系选择（lla设计 §2.2 / §2.3：无椭球模型 + Ellipsoid → 注入 WGS-84）。
SCENARIO("three ellipsoid channels keep up axes parallel to Host when it rolls over live CIGI",
         "[acceptance][bdd][sync][lla][offset][e2e][multi-ig][cigi][rigid]")
{
    GIVEN("Host+IG A and IG-only B/C all on coordFrame Ellipsoid (inject WGS-84), yaw offsets -60/+60")
    {
        RigidArrayHarness h(20120, "Ellipsoid", "models/lz.vsgt");

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

        WHEN("A publishes the rolled LLA intent and all channels tick (shared CIGI Detach+LLA eye)")
        {
            REQUIRE(h.a.setCameraPoseLla(lla, yprHost));
            h.tick(3);

            THEN("B/C up axes stay parallel to Host up and forwards equal R_host*Rz(delta) in ECEF")
            {
                requireEcefRigidArray(h, lla, yprHost);
            }
        }
    }
}

// lla设计 §4.4 / §7：LookAt ECEF 与 `_lastApplied` 重建一致 → 不采样；不走「减 offset 回推」。
SCENARIO("ellipsoid anti-echo skips sampling when LookAt matches lastApplied ECEF rebuild",
         "[acceptance][bdd][sync][lla][anti-echo]")
{
    GIVEN("Host+IG Engine A on readymap with zero offset and an applied LLA authority eye")
    {
        constexpr int kBase = 19800;
        Engine engineA;
        engineA.extent = {640, 480};
        engineA.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engineA.init(modelPath, makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineA.synchronSystem().igLinked());

        auto emA = ellipsoidOf(engineA);
        REQUIRE(emA);

        engineA.synchronSystem().setOffsetDeg({});

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 ypr{30.0, 5.0, 0.0};
        const HostEyePose intent{toDVec3(lla), toDVec3(ypr), HostEyeCoordFrame::LLA};

        REQUIRE(engineA.setCameraPoseLla(lla, ypr));
        for (int i = 0; i < 3; ++i)
            REQUIRE(engineA.tickOnFrame());

        auto applied = engineA.synchronSystem().lastAppliedHostEye();
        REQUIRE(applied.has_value());
        REQUIRE(applied->frame == HostEyeCoordFrame::LLA);

        // Precondition (lla §4.4)：当前 LookAt 的 ECEF eye/forward/up 与 `_lastApplied` 重建一致。
        requireLookAtMatchesLlaPose(engineA, *emA, applied->position, applied->eulerYprDeg, 1e-2, 1e-6);

        auto sentBefore = engineA.lastSentHostEye();
        REQUIRE(sentBefore.has_value());
        REQUIRE(sentBefore->frame == HostEyeCoordFrame::LLA);
        requirePoseNear(*sentBefore, intent, 1e-3);

        WHEN("another tick runs without changing the camera")
        {
            REQUIRE(engineA.tickOnFrame());

            THEN("Host does not sample a new intent; lastSent stays the prior LLA eye")
            {
                auto sentAfter = engineA.lastSentHostEye();
                REQUIRE(sentAfter.has_value());
                REQUIRE(sentAfter->frame == HostEyeCoordFrame::LLA);
                // Idle → resend `_lastSent`, not a LookAt→LLA re-sample (would drift YPR via float).
                requirePoseNear(*sentAfter, *sentBefore, 1e-9);
                requirePoseNear(*sentAfter, intent, 1e-3);
            }
        }
    }
}

SCENARIO("aligned Host and IG ellipsoid smoke: both have EllipsoidModel with matching radii",
         "[acceptance][bdd][sync][lla][smoke]")
{
    GIVEN("Host+IG Engine A and IG-only Engine B both load readymap (built-in EllipsoidModel)")
    {
        constexpr int kBase = 19200;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.hostSync().readyIgCount() == 2);
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

SCENARIO("Host ellipsoid vs IG local rejects mismatched eye and keeps SOF healthy",
         "[acceptance][bdd][sync][lla][mode-mismatch]")
{
    GIVEN("Host+IG Engine A on readymap (ellipsoid) and IG-only Engine B on teapot (local)")
    {
        constexpr int kBase = 19300;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path readymapPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        const vsg::Path teapotPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(readymapPath));
        REQUIRE(engineB.initSceneMode(teapotPath));
        REQUIRE(ellipsoidOf(engineA));
        REQUIRE_FALSE(engineB.ellipsoidModel());

        engineA.synchronSystem().setOffsetDeg({});
        engineB.synchronSystem().setOffsetDeg({});

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 ypr{0.0, 0.0, 0.0};
        const std::uint64_t rejectedBefore = engineB.synchronSystem().eyePoseRejectedByFrameMismatch();
        const auto sofBefore = engineB.synchronSystem().igSync().sofSentCount();

        WHEN("A publishes an ellipsoid LLA eye over live CIGI and both tick")
        {
            // lla §4.5 / §7：首拒收打 [ERROR] 一次；持续不符不刷屏。
            std::stringstream errCapture;
            auto* prevCerr = std::cerr.rdbuf(errCapture.rdbuf());

            REQUIRE(engineA.setCameraPoseLla(lla, ypr));
            for (int i = 0; i < 3; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }

            std::cerr.rdbuf(prevCerr);
            const std::string errLog = errCapture.str();

            THEN("B rejects without polluting camera; first [ERROR]; SOF/ready healthy")
            {
                REQUIRE(engineB.synchronSystem().eyePoseRejectedByFrameMismatch() > rejectedBefore);
                REQUIRE(engineA.hostSync().readyIgCount() == 2);
                REQUIRE(engineB.synchronSystem().igSync().sofSentCount() > sofBefore);

                auto emA = ellipsoidOf(engineA);
                REQUIRE(emA);
                REQUIRE_FALSE(engineB.synchronSystem().lastAppliedHostEye().has_value());

                const auto firstError = errLog.find("[ERROR]");
                REQUIRE(firstError != std::string::npos);
                REQUIRE(errLog.find("[ERROR]", firstError + 7) == std::string::npos);
            }
        }
    }
}

SCENARIO("Host readymap vs IG inject-WGS84 radius mismatch makes ECEF follow disagree",
         "[acceptance][bdd][sync][lla][radius-mismatch]")
{
    GIVEN("Host on readymap ellipsoid and IG on lz with coordFrame Ellipsoid (inject WGS-84)")
    {
        constexpr int kBase = 19400;
        const TempConfigFile hostFile(
            std::string(R"({
              "syncSystem": {
              "channelId": 0,
              "offsetDeg": { "yaw": 0.0, "pitch": 0.0, "roll": 0.0 },
              "requireConnectedIg": true
              },
              "igConfig": { "udpPortSend": )") +
            std::to_string(kBase) + R"(, "udpPortRecv": )" + std::to_string(kBase + 1) +
            R"(, "targetAddr": "127.0.0.1", "targetTcpPort": )" + std::to_string(kBase + 100) +
            R"(, "targetUdpPortRecv": )" + std::to_string(kBase) + R"( },
              "hostConfig": { "udpPortSend": )" +
            std::to_string(kBase + 1) + R"(, "udpPortRecv": )" + std::to_string(kBase) +
            R"(, "tcpPort": )" + std::to_string(kBase + 100) + R"( },
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
              "coordFrame": "Ellipsoid",
              "igConfig": { "udpPortSend": )") +
            std::to_string(kBase) + R"(, "udpPortRecv": )" + std::to_string(kBase + 3) +
            R"(, "targetAddr": "127.0.0.1", "targetTcpPort": )" + std::to_string(kBase + 100) +
            R"(, "targetUdpPortRecv": )" + std::to_string(kBase) + R"( },
              "model": "models/lz.vsgt",
              "window": { "x": 0, "y": 0, "width": 640, "height": 480 }
            })");

        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;
        REQUIRE(engineA.loadConfig(hostFile.path()));
        REQUIRE(engineB.loadConfig(igFile.path()));
        REQUIRE(engineA.init());
        // B：sync + scene mode only（部分机器单 Vulkan Device 限制）。
        REQUIRE(engineB.initSync(engineB.config.toSyncRole(), engineB.config.syncSystem.requireConnectedIg));
        REQUIRE(engineB.initSceneMode(vsg::Path(RESOURCE_DIR) / engineB.config.model));
        engineB.synchronSystem().setOffsetDeg(engineB.config.syncSystem.offsetDeg);

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
        // （TCP×16、握手×8）在 Windows 下仍可能因 UDP_SYNC_ACK 偶发丢包耗尽（多通道同步模块设计.md P1）。
        // 测试侧持续重连直到 B 与 A 全部 ready，用反复 connect 兜住单次重试耗尽的问题。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (engineA.hostSync().readyIgCount() == 2)
                break;
            if (engineB.synchronSystem().hasIg() && !engineB.synchronSystem().igSync().udpSynced())
                engineB.synchronSystem().igSync().connect(engineB.config.igConfig);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(engineA.hostSync().readyIgCount() == 2);

        WHEN("A publishes that LLA eye and B follows over CIGI")
        {
            REQUIRE(engineA.setCameraPoseLla(lla, ypr));
            for (int i = 0; i < 3; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }

            THEN("B applied LLA converts to ECEF that disagrees with Host beyond meter-scale")
            {
                auto applied = engineB.synchronSystem().lastAppliedHostEye();
                REQUIRE(applied.has_value());
                REQUIRE(applied->frame == HostEyeCoordFrame::LLA);
                const vsg::dvec3 igEcef = emB->convertLatLongAltitudeToECEF(toVsg(applied->position));
                REQUIRE(vsg::length(igEcef - hostEcef) > 0.5);
            }
        }
    }
}

// =============================================================================
// 6. LLA 验收补齐：模式隔离 / 范围校验 / 权威 offset / 缓存复位（lla设计 §7）
// =============================================================================

SCENARIO("Host local vs IG ellipsoid rejects mismatched Attach eye and keeps SOF healthy",
         "[acceptance][bdd][sync][lla][mode-mismatch]")
{
    GIVEN("Host+IG Engine A on teapot (local) and IG-only Engine B on readymap (ellipsoid)")
    {
        constexpr int kBase = 19600;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path teapotPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        const vsg::Path readymapPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(teapotPath));
        REQUIRE(engineB.initSceneMode(readymapPath));
        REQUIRE_FALSE(engineA.ellipsoidModel());
        REQUIRE(engineB.ellipsoidModel());

        engineA.synchronSystem().setOffsetDeg({});
        engineB.synchronSystem().setOffsetDeg({});

        const HostEyePose localEye{{11.0, 22.0, 33.0}, {10.0, 0.0, 0.0}, HostEyeCoordFrame::WORLD_LOCAL};
        const std::uint64_t rejectedBefore = engineB.synchronSystem().eyePoseRejectedByFrameMismatch();
        const auto sofBefore = engineB.synchronSystem().igSync().sofSentCount();

        WHEN("A publishes a local XYZ eye over live CIGI and both tick")
        {
            std::stringstream errCapture;
            auto* prevCerr = std::cerr.rdbuf(errCapture.rdbuf());

            REQUIRE(engineA.setCameraPose(toVsg(localEye.position), toVsg(localEye.eulerYprDeg)));
            for (int i = 0; i < 3; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }

            std::cerr.rdbuf(prevCerr);
            const std::string errLog = errCapture.str();

            THEN("B rejects by frame mismatch; camera unchanged; first [ERROR]; SOF/ready healthy")
            {
                REQUIRE(engineB.synchronSystem().eyePoseRejectedByFrameMismatch() > rejectedBefore);
                REQUIRE(engineA.hostSync().readyIgCount() == 2);
                REQUIRE(engineB.synchronSystem().igSync().sofSentCount() > sofBefore);
                REQUIRE_FALSE(engineB.synchronSystem().lastAppliedHostEye().has_value());

                const auto firstError = errLog.find("[ERROR]");
                REQUIRE(firstError != std::string::npos);
                REQUIRE(errLog.find("[ERROR]", firstError + 7) == std::string::npos);
            }
        }
    }
}

// lla §7 mode-isolation local regression：错模式路径之外，同本地模式跟拍仍成立。
SCENARIO("local XYZ Host→IG follow remains green under mode-isolation regression",
         "[acceptance][bdd][sync][lla][mode-mismatch][local-regression]")
{
    GIVEN("Host+IG A and IG-only B both on teapot (local / Attach)")
    {
        constexpr int kBase = 19650;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(modelPath));
        REQUIRE(engineB.initSceneMode(modelPath));
        REQUIRE_FALSE(engineA.ellipsoidModel());
        REQUIRE_FALSE(engineB.ellipsoidModel());

        engineA.synchronSystem().setOffsetDeg({});
        engineB.synchronSystem().setOffsetDeg({});

        const HostEyePose localEye{{15.0, 25.0, 35.0}, {20.0, 0.0, 0.0}, HostEyeCoordFrame::WORLD_LOCAL};

        WHEN("A publishes a local XYZ eye and both tick")
        {
            REQUIRE(engineA.setCameraPose(toVsg(localEye.position), toVsg(localEye.eulerYprDeg)));
            for (int i = 0; i < 3; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }

            THEN("B applies the Host eye (local regression still green)")
            {
                REQUIRE(engineB.synchronSystem().eyePoseRejectedByFrameMismatch() == 0);
                auto applied = engineB.synchronSystem().lastAppliedHostEye();
                REQUIRE(applied.has_value());
                REQUIRE(applied->frame == HostEyeCoordFrame::WORLD_LOCAL);
                REQUIRE(vsg::length(toVsg(applied->position) - toVsg(localEye.position)) < 1e-4);
            }
        }
    }
}

TEST_CASE("CIGI LLA pack drops out-of-range lat/pitch eye but still packs IGCtrl",
          "[unit][cigi][wire-contract][lla][range]")
{
    const auto rejectedBefore = cigi_wire::eyePoseRejectedByRange();

    cigi_wire::EyePose badLat{};
    badLat.frame = cigi_wire::EyeFrame::LLA;
    badLat.x = 91.0; // lat OOR
    badLat.y = 10.0;
    badLat.z = 100.0;

    std::vector<unsigned char> buf;
    bool packed = false;
    REQUIRE_NOTHROW(packed = cigi_wire::packHostFrame(20, 0.0, &badLat, buf));
    REQUIRE(packed);
    REQUIRE_FALSE(buf.empty());

    cigi_wire::HostFrame frame{};
    REQUIRE(cigi_wire::unpackHostFrame(buf.data(), static_cast<int>(buf.size()), frame));
    REQUIRE(frame.frameCntr == 20);
    REQUIRE_FALSE(frame.eye.has_value());
    REQUIRE(cigi_wire::eyePoseRejectedByRange() > rejectedBefore);

    cigi_wire::EyePose badPitch{};
    badPitch.frame = cigi_wire::EyeFrame::LLA;
    badPitch.x = 39.9;
    badPitch.y = 116.4;
    badPitch.z = 500.0;
    badPitch.pitchDeg = 95.0; // pitch OOR

    const auto rejectedMid = cigi_wire::eyePoseRejectedByRange();
    packed = false;
    REQUIRE_NOTHROW(packed = cigi_wire::packHostFrame(21, 0.0, &badPitch, buf));
    REQUIRE(packed);
    REQUIRE(cigi_wire::unpackHostFrame(buf.data(), static_cast<int>(buf.size()), frame));
    REQUIRE_FALSE(frame.eye.has_value());
    REQUIRE(cigi_wire::eyePoseRejectedByRange() > rejectedMid);
}

TEST_CASE("CIGI LLA pack normalizes longitude into (-180,180]",
          "[unit][cigi][wire-contract][lla][range]")
{
    auto packAndReadLon = [](double lonIn) {
        cigi_wire::EyePose eye{};
        eye.frame = cigi_wire::EyeFrame::LLA;
        eye.x = 10.0;
        eye.y = lonIn;
        eye.z = 50.0;
        std::vector<unsigned char> buf;
        bool packed = false;
        REQUIRE_NOTHROW(packed = cigi_wire::packHostFrame(22, 0.0, &eye, buf));
        REQUIRE(packed);
        cigi_wire::HostFrame frame{};
        REQUIRE(cigi_wire::unpackHostFrame(buf.data(), static_cast<int>(buf.size()), frame));
        REQUIRE(frame.eye.has_value());
        REQUIRE(frame.eye->frame == cigi_wire::EyeFrame::LLA);
        return frame.eye->y;
    };

    REQUIRE(packAndReadLon(190.0) == Catch::Approx(-170.0));
    REQUIRE(packAndReadLon(-190.0) == Catch::Approx(170.0));
    REQUIRE(packAndReadLon(180.0) == Catch::Approx(180.0));
}

SCENARIO("authority Host channel keeps offsetDeg at zero",
         "[acceptance][bdd][sync][lla][authority-offset]")
{
    GIVEN("a Host+IG authority channel config")
    {
        EngineChannelConfig cfg;
        std::string error;
        const TempConfigFile file(kMainJson);
        REQUIRE(loadEngineChannelConfig(file.path(), cfg, &error));

        WHEN("the authority offset is inspected")
        {
            THEN("yaw/pitch/roll are all zero (lla §7 权威 offset)")
            {
                REQUIRE(cfg.syncSystem.offsetDeg.yaw == Catch::Approx(0.0));
                REQUIRE(cfg.syncSystem.offsetDeg.pitch == Catch::Approx(0.0));
                REQUIRE(cfg.syncSystem.offsetDeg.roll == Catch::Approx(0.0));
            }
        }
    }

    GIVEN("an Engine loaded from a Host+IG authority config")
    {
        Engine engine;
        const TempConfigFile file(kMainJson);
        REQUIRE(engine.loadConfig(file.path()));
        engine.showWindow = false;
        REQUIRE(engine.init());

        THEN("SynchronSystem authority offset stays all zero")
        {
            REQUIRE(engine.synchronSystem().offsetDeg().yaw == Catch::Approx(0.0));
            REQUIRE(engine.synchronSystem().offsetDeg().pitch == Catch::Approx(0.0));
            REQUIRE(engine.synchronSystem().offsetDeg().roll == Catch::Approx(0.0));
        }
    }
}

SCENARIO("initGraphics clears SynchronSystem eye caches without network shutdown",
         "[acceptance][bdd][sync][lla][cache-reset]")
{
    GIVEN("a linked Host+IG Engine with an applied Host eye and lastSent populated")
    {
        constexpr int kBase = 19700;
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engine.synchronSystem().igLinked());

        const HostEyePose hostPose{{7.0, 8.0, 9.0}, {12.0, 0.0, 0.0}};
        engine.synchronSystem().setOffsetDeg({});
        // 先采样/发送以填充 `_lastSent`，再应用（仅防回声会跳过发送）。
        REQUIRE(engine.setCameraPose(toVsg(hostPose.position), toVsg(hostPose.eulerYprDeg)));
        REQUIRE(engine.tickOnFrame());
        engine.synchronSystem().queueHostEyePose(hostPose);
        engine.stepSync();

        REQUIRE(engine.synchronSystem().lastAppliedHostEye().has_value());
        REQUIRE(engine.lastSentHostEye().has_value());
        REQUIRE(engine.hasHost());
        REQUIRE(engine.synchronSystem().hasIg());

        WHEN("initGraphics rebuilds the scene without SynchronSystem::shutdown")
        {
            REQUIRE(engine.initGraphics(modelPath));

            THEN("eye caches are empty while Host/IG links remain")
            {
                REQUIRE_FALSE(engine.synchronSystem().lastAppliedHostEye().has_value());
                REQUIRE_FALSE(engine.lastSentHostEye().has_value());
                REQUIRE(engine.hasHost());
                REQUIRE(engine.synchronSystem().hasIg());
                REQUIRE(engine.synchronSystem().igLinked());
            }
        }
    }
}

// lla设计 §4.3 / §7：Local↔Ellipsoid 换轨清空位姿缓存；不得用旧类型 `_lastSent` 重发。
SCENARIO("Local to Ellipsoid initGraphics clears caches and switches scene mode",
         "[acceptance][bdd][sync][lla][scene-switch]")
{
    GIVEN("a linked Host+IG Engine on teapot with a WorldLocal lastSent")
    {
        constexpr int kBase = 19850;
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;

        const vsg::Path teapotPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        const vsg::Path readymapPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engine.init(teapotPath, makeHostIgRole(kBase + 1, kBase)));
        REQUIRE_FALSE(ellipsoidOf(engine));

        const HostEyePose localPose{{3.0, 4.0, 5.0}, {8.0, 0.0, 0.0}, HostEyeCoordFrame::WORLD_LOCAL};
        engine.synchronSystem().setOffsetDeg({});
        // setCameraPose + tick 采样/发送；避免先 queue+update（防回声会抑制发送）。
        REQUIRE(engine.setCameraPose(toVsg(localPose.position), toVsg(localPose.eulerYprDeg)));
        REQUIRE(engine.tickOnFrame());

        auto sentBefore = engine.lastSentHostEye();
        REQUIRE(sentBefore.has_value());
        REQUIRE(sentBefore->frame == HostEyeCoordFrame::WORLD_LOCAL);

        WHEN("initGraphics reloads readymap (Local → Ellipsoid) without sync shutdown")
        {
            REQUIRE(engine.initGraphics(readymapPath));

            THEN("eye caches are cleared and the scene is ellipsoid")
            {
                REQUIRE_FALSE(engine.synchronSystem().lastAppliedHostEye().has_value());
                REQUIRE_FALSE(engine.lastSentHostEye().has_value());
                REQUIRE(ellipsoidOf(engine));
                REQUIRE(engine.synchronSystem().igLinked());
            }
        }
    }
}

SCENARIO("Ellipsoid to Local initGraphics clears caches and switches scene mode",
         "[acceptance][bdd][sync][lla][scene-switch]")
{
    GIVEN("a linked Host+IG Engine on readymap with an LLA lastSent")
    {
        constexpr int kBase = 19870;
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;

        const vsg::Path readymapPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        const vsg::Path teapotPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(readymapPath, makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(ellipsoidOf(engine));

        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 ypr{10.0, 0.0, 0.0};
        engine.synchronSystem().setOffsetDeg({});
        REQUIRE(engine.setCameraPoseLla(lla, ypr));
        for (int i = 0; i < 3; ++i)
            REQUIRE(engine.tickOnFrame());

        auto sentBefore = engine.lastSentHostEye();
        REQUIRE(sentBefore.has_value());
        REQUIRE(sentBefore->frame == HostEyeCoordFrame::LLA);

        WHEN("initGraphics reloads teapot (Ellipsoid → Local) without sync shutdown")
        {
            REQUIRE(engine.initGraphics(teapotPath));

            THEN("eye caches are cleared and the scene is local")
            {
                REQUIRE_FALSE(engine.synchronSystem().lastAppliedHostEye().has_value());
                REQUIRE_FALSE(engine.lastSentHostEye().has_value());
                REQUIRE_FALSE(ellipsoidOf(engine));
                REQUIRE(engine.synchronSystem().igLinked());
            }
        }
    }
}

// lla设计 §4.3 / §7：场景已椭球而 `_lastSent` 仍为 WorldLocal（或反向）→ 丢弃，不扇出触发对端拒收。
SCENARIO("WorldLocal lastSent is discarded on ellipsoid scene and not fanned out",
         "[acceptance][bdd][sync][lla][lastSent-switch]")
{
    GIVEN("linked A(Host+IG) and B(IG) on readymap; A has residual WorldLocal lastSent")
    {
        constexpr int kBase = 19900;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "readymap.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(modelPath));
        REQUIRE(engineB.initSceneMode(modelPath));
        REQUIRE(ellipsoidOf(engineA));
        REQUIRE(engineB.ellipsoidModel());

        engineA.synchronSystem().setOffsetDeg({});
        engineB.synchronSystem().setOffsetDeg({});

        // Align camera via LLA apply so idle capture anti-echoes (no new _frameSample).
        const vsg::dvec3 lla{39.9, 116.4, 500.0};
        const vsg::dvec3 ypr{0.0, 0.0, 0.0};
        const HostEyePose appliedLla{toDVec3(lla), toDVec3(ypr), HostEyeCoordFrame::LLA};
        REQUIRE(engineA.setCameraPoseLla(lla, ypr));
        engineA.synchronSystem().queueHostEyePose(appliedLla);
        engineA.stepSync();
        REQUIRE(engineA.synchronSystem().lastAppliedHostEye().has_value());

        // Residual wrong-type cache (lla §4.3)：场景已椭球，`_lastSent` 仍为 WorldLocal。
        const HostEyePose staleLocal{{9.0, 8.0, 7.0}, {15.0, 0.0, 0.0}, HostEyeCoordFrame::WORLD_LOCAL};
        engineA.seedLastSentHostEye(staleLocal);
        REQUIRE(engineA.lastSentHostEye().has_value());
        REQUIRE(engineA.lastSentHostEye()->frame == HostEyeCoordFrame::WORLD_LOCAL);

        const auto rejectedBefore = engineB.synchronSystem().eyePoseRejectedByFrameMismatch();

        WHEN("A ticks idle (would otherwise resend stale WorldLocal lastSent)")
        {
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();

            THEN("A drops lastSent; B is not hit with a frame-mismatch eye")
            {
                REQUIRE_FALSE(engineA.lastSentHostEye().has_value());
                REQUIRE(engineB.synchronSystem().eyePoseRejectedByFrameMismatch() == rejectedBefore);
            }
        }
    }
}

SCENARIO("Lla lastSent is discarded on local scene and not fanned out",
         "[acceptance][bdd][sync][lla][lastSent-switch]")
{
    GIVEN("linked A(Host+IG) and B(IG) on teapot; A has residual Lla lastSent")
    {
        constexpr int kBase = 19950;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engineA.initSync(makeHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(modelPath));
        REQUIRE(engineB.initSceneMode(modelPath));
        REQUIRE_FALSE(engineA.ellipsoidModel());
        REQUIRE_FALSE(engineB.ellipsoidModel());

        engineA.synchronSystem().setOffsetDeg({});
        engineB.synchronSystem().setOffsetDeg({});

        const HostEyePose appliedLocal{{2.0, 3.0, 4.0}, {5.0, 0.0, 0.0}, HostEyeCoordFrame::WORLD_LOCAL};
        engineA.synchronSystem().queueHostEyePose(appliedLocal);
        engineA.stepSync();

        const HostEyePose staleLla{{39.9, 116.4, 500.0}, {20.0, 0.0, 0.0}, HostEyeCoordFrame::LLA};
        engineA.seedLastSentHostEye(staleLla);
        REQUIRE(engineA.lastSentHostEye()->frame == HostEyeCoordFrame::LLA);

        const auto rejectedBefore = engineB.synchronSystem().eyePoseRejectedByFrameMismatch();

        WHEN("A ticks idle (would otherwise resend stale Lla lastSent)")
        {
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();

            THEN("A drops lastSent; B is not hit with a frame-mismatch eye")
            {
                REQUIRE_FALSE(engineA.lastSentHostEye().has_value());
                REQUIRE(engineB.synchronSystem().eyePoseRejectedByFrameMismatch() == rejectedBefore);
            }
        }
    }
}

// =============================================================================
// viewhost E2E：独立 Host 进程入口（loadHostConfig → SynchronSystem）与带 IG 的
// Engine 真实 TCP/UDP/CIGI 收发（sync模块化设计.md §4.1）。
// =============================================================================

SCENARIO("viewhost loads hostConfig and exchanges CIGI with an IG engine",
         "[acceptance][bdd][sync][viewhost][cigi]")
{
    GIVEN("a viewhost HostSync loaded from a host-only config file, and an IG engine targeting it")
    {
        constexpr int kBase = 21000;

        // viewhost 配置：与 makeTestIgOnlyRole 的 target（tcp=base+100, udpRecv=base）对齐。
        const TempConfigFile viewhostFile(
            std::string(R"({ "hostConfig": { "udpPortSend": )") +
            std::to_string(kBase + 1) + R"(, "udpPortRecv": )" + std::to_string(kBase) +
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
        REQUIRE(engineIg.initSync(makeTestIgOnlyRole(kBase + 3, kBase)));

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
                const std::uint32_t sentBefore = viewhost->igCtrlSentCount();
                const std::uint32_t recvBefore = engineIg.synchronSystem().igSync().igCtrlReceivedCount();
                const std::uint32_t sofBefore = viewhost->sofReceivedCount();

                constexpr int kTicks = 5;
                for (int i = 0; i < kTicks; ++i)
                {
                    viewhost->update(i * 16.667); // 无渲染节拍：viewhost 扇出
                    engineIg.tickSync();          // IG 收包 + 回 SOF
                }

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
         "[acceptance][bdd][sync][standalone][cigi]")
{
    GIVEN("host HostSync from hostConfig file, and IG SynchronSystem from igConfig file")
    {
        constexpr int kBase = 22000;

        // host 独立配置（udpSend=base+1, udpRecv=base, tcp=base+100）。
        const TempConfigFile hostFile(
            std::string(R"({ "hostConfig": { "udpPortSend": )") +
            std::to_string(kBase + 1) + R"(, "udpPortRecv": )" + std::to_string(kBase) +
            R"(, "tcpPort": )" + std::to_string(kBase + 100) + R"( } })");
        // IG 独立配置（本地 udpRecv=base+3，target 指向 host 的 tcp=base+100 / udpRecv=base）。
        const TempConfigFile igFile(
            std::string(R"({ "igConfig": { "udpPortSend": )") +
            std::to_string(kBase) + R"(, "udpPortRecv": )" + std::to_string(kBase + 3) +
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
        SyncRoleConfig igRole;
        igRole.enableIg = true;
        igRole.igConfig = ig;
        // 装配参数程序化注入（外部 engine 不经 syncSystem 配置文件时的路径）。
        SyncSystemConfig igSystem;
        igSystem.channelId = 2;
        igSystem.offsetDeg = OffsetDeg{5.0, 0.0, 0.0};
        igSystem.hostEyeStalePolicy = HostEyeStalePolicy::REUSE_LAST;
        REQUIRE(igSync->initialize(igRole, igSystem));

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
                const std::uint32_t sentBefore = hostSync->igCtrlSentCount();
                const std::uint32_t recvBefore = igSync->igSync().igCtrlReceivedCount();
                const std::uint32_t sofBefore = hostSync->sofReceivedCount();

                constexpr int kTicks = 5;
                for (int i = 0; i < kTicks; ++i)
                {
                    hostSync->update(i * 16.667); // host 扇出 IGCtrl
                    igSync->preFrame();           // IG 收 IGCtrl + 回 SOF
                    igSync->update();
                }

                REQUIRE(hostSync->igCtrlSentCount() > sentBefore);
                REQUIRE(igSync->igSync().igCtrlReceivedCount() > recvBefore);
                REQUIRE(hostSync->sofReceivedCount() > sofBefore);
            }
        }
    }
}
