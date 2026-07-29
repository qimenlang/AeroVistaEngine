#include <catch2/catch_test_macros.hpp>

#include "engine.h"
#include "function/sync/HostSync.h"
#include "function/sync/IgSync.h"

#include <cstdint>

namespace
{
    // 默认端口见 doc/多通道同步模块设计.md
    AddressConfig makeHostLocal(const std::string& addr = "127.0.0.1")
    {
        return AddressConfig{addr, 8001, 8000, 8100};
    }

    AddressConfig makeIgLocal(const std::string& localAddr = "127.0.0.1", int udpRecvPort = 8001)
    {
        return AddressConfig{localAddr, 8000, udpRecvPort, 8100};
    }

    AddressConfig makeHostEndpoint(const std::string& addr = "127.0.0.1")
    {
        return AddressConfig{addr, 8001, 8000, 8100};
    }

    // UDP 可丢：actual 落在 [expected-slack, expected]
    bool approxAtMost(std::uint32_t actual, int expected, int slack)
    {
        const auto exp = static_cast<std::uint32_t>(expected);
        const auto minOk = exp > static_cast<std::uint32_t>(slack) ? exp - static_cast<std::uint32_t>(slack) : 0u;
        return actual >= minOk && actual <= exp;
    }
} // namespace

// =============================================================================
// 1. 连接面（集成 / 协议契约，非 Engine 产品验收）
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
            const bool connected = ig.connect(makeHostEndpoint());

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
                const bool connected = ig.connect(makeHostEndpoint());

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
        REQUIRE(ig.connect(makeHostEndpoint()));
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

            WHEN("the IG connects using a Host endpoint with wrong UDP ports")
            {
                // Connect 使用 hostEndpoint.udpPortRecv 作为 Host UDP 收端口。
                AddressConfig badUdpEndpoint{"127.0.0.1", 8001, 9999, 8100};
                const bool connected = ig.connect(badUdpEndpoint);

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
            REQUIRE(ig1.initialize(makeIgLocal("127.0.0.1", 8001)));
            REQUIRE(ig2.initialize(makeIgLocal("127.0.0.1", 8003)));

            REQUIRE(ig1.connect(makeHostEndpoint()));
            REQUIRE(ig2.connect(makeHostEndpoint()));

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
// 2. 帧节拍：IGCtrl / SOF / FreeRun（集成 / 协议契约）
// =============================================================================

SCENARIO("connected Host and IG enter RUNNING and exchange IGCtrl each update",
         "[integration][sync][status]")
{
    GIVEN("a Host and an IG that have been initialized and connected")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeHostEndpoint()));

        WHEN("Host runs and sends 10 IGCtrl frames while IG updates")
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

SCENARIO("IG replies with one SOF per received IGCtrl", "[integration][sync][status][sof]")
{
    GIVEN("a Host and an IG that have been initialized and connected")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeHostEndpoint()));

        WHEN("Host sends 10 IGCtrl and IG updates each frame")
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

SCENARIO("Host keeps sending IGCtrl when IG never replies SOF", "[integration][sync][status][freerun]")
{
    GIVEN("a connected Host and IG with FreeRun send pace")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeHostEndpoint()));

        SyncPaceConfig pace{};
        pace.igCtrlSendPace = SendPace::FREE_RUN;
        pace.frameGate = FrameGate::FREE_RUN;
        host.setPaceConfig(pace);

        WHEN("Host sends 10 IGCtrl while IG receives but never replies SOF")
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

SCENARIO("IG last received frame counter matches Host frame numbers",
         "[integration][sync][status][frame]")
{
    GIVEN("a Host and an IG that have been initialized and connected")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeHostEndpoint()));

        WHEN("Host sends IGCtrl frames 0..N-1 and IG updates each frame")
        {
            host.run();
            constexpr int kFrames = 10;
            std::uint32_t prevReceived = 0;
            int matchedFrames = 0;

            for (int i = 0; i < kFrames; ++i)
            {
                host.update(/*simTimeMs=*/i * (1000.0 / 60.0)); // Host 本轮 frameCntr == i
                ig.update();

                if (ig.igCtrlReceivedCount() > prevReceived)
                {
                    REQUIRE(ig.lastIgCtrlFrameCntr() == static_cast<std::uint32_t>(i));
                    prevReceived = ig.igCtrlReceivedCount();
                    ++matchedFrames;
                }
            }

            THEN("at least one IGCtrl was received and frame counters matched")
            {
                REQUIRE(matchedFrames >= 1);
                REQUIRE(ig.igCtrlReceivedCount() == static_cast<std::uint32_t>(matchedFrames));
                REQUIRE(ig.lastIgCtrlFrameCntr() < static_cast<std::uint32_t>(kFrames));
            }
        }
    }
}

// =============================================================================
// 3. Engine + SynchronSystem 集成（帧交换契约）
// =============================================================================

SCENARIO("single Engine with Host and IG exchanges frame control over ticks",
         "[integration][sync][engine]")
{
    GIVEN("an offscreen Engine whose SynchronSystem owns both Host and IG")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        SyncRoleConfig syncRole{};
        syncRole.enableHost = true;
        syncRole.enableIg = true;
        syncRole.hostLocal = makeHostLocal();
        syncRole.igLocal = makeIgLocal();
        syncRole.hostEndpoint = makeHostEndpoint();

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";

        WHEN("the Engine is initialized and tickOnFrame runs 10 times")
        {
            REQUIRE(engine.init(modelPath, syncRole));

            constexpr int kTicks = 10;
            for (int i = 0; i < kTicks; ++i)
                REQUIRE(engine.tickOnFrame());

            THEN("Host and IG exchanged IGCtrl/SOF via the Engine loop")
            {
                SynchronSystem& sync = engine.synchronSystem();
                HostSync& host = sync.hostSync();
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

SCENARIO("three Engines exchange frame control across one Host and three IGs",
         "[integration][sync][engine][multi-ig]")
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
        roleA.hostLocal = makeHostLocal();
        roleA.igLocal = makeIgLocal("127.0.0.1", 8001);
        roleA.hostEndpoint = makeHostEndpoint();

        SyncRoleConfig roleB{};
        roleB.enableHost = false;
        roleB.enableIg = true;
        roleB.igLocal = makeIgLocal("127.0.0.1", 8003);
        roleB.hostEndpoint = makeHostEndpoint();

        SyncRoleConfig roleC{};
        roleC.enableHost = false;
        roleC.enableIg = true;
        roleC.igLocal = makeIgLocal("127.0.0.1", 8005);
        roleC.hostEndpoint = makeHostEndpoint();

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";

        WHEN("all three engines sync-connect, A loads graphics, then A leads 10 ticks")
        {
            REQUIRE(engineA.initSync(roleA));
            REQUIRE(engineB.initSync(roleB));
            REQUIRE(engineC.initSync(roleC));
            REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 3);
            REQUIRE(engineA.initGraphics(modelPath));

            constexpr int kTicks = 10;
            constexpr int kIgCount = 3;
            for (int i = 0; i < kTicks; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
                engineC.tickSync();
            }

            THEN("each IG got about N IGCtrl and A's Host got about N times 3 SOF")
            {
                HostSync& host = engineA.synchronSystem().hostSync();
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
    vsg::dquat quatFromEulerYprDeg(const vsg::dvec3& eulerYprDeg)
    {
        return vsg::dquat(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0)) *
               vsg::dquat(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) *
               vsg::dquat(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0));
    }

    void requireLookAtMatchesPose(Engine& engine, const vsg::dvec3& position, const vsg::dvec3& eulerYprDeg)
    {
        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);

        const vsg::dquat rotation = quatFromEulerYprDeg(eulerYprDeg);
        const vsg::dvec3 expectedForward = rotation * vsg::dvec3(0.0, 1.0, 0.0);
        const vsg::dvec3 expectedUp = rotation * vsg::dvec3(0.0, 0.0, 1.0);
        const vsg::dvec3 expectedCenter = position + expectedForward;

        REQUIRE(vsg::length(lookAt->eye - position) < 1e-9);
        REQUIRE(vsg::length(lookAt->center - expectedCenter) < 1e-9);
        REQUIRE(vsg::length(vsg::normalize(lookAt->up) - vsg::normalize(expectedUp)) < 1e-9);
    }

    void requirePoseNear(const HostEyePose& actual, const HostEyePose& expected, double eps = 1e-6)
    {
        REQUIRE(vsg::length(actual.position - expected.position) < eps);
        REQUIRE(vsg::length(actual.eulerYprDeg - expected.eulerYprDeg) < eps);
    }

    HostEyePose hostEyePlusOffset(const HostEyePose& host, const OffsetDeg& offset)
    {
        HostEyePose out = host;
        out.eulerYprDeg.x += offset.yaw;
        out.eulerYprDeg.y += offset.pitch;
        out.eulerYprDeg.z += offset.roll;
        return out;
    }

    // Host 眼点用例使用独立端口，避免与 §1–3 默认 8000/8001 并行冲突。
    AddressConfig makeHostLocalEye(int base = 18000)
    {
        return AddressConfig{"127.0.0.1", base + 1, base, base + 100};
    }

    AddressConfig makeIgLocalEye(int udpRecvPort, int base = 18000)
    {
        return AddressConfig{"127.0.0.1", base, udpRecvPort, base + 100};
    }

    AddressConfig makeHostEndpointEye(int base = 18000)
    {
        return AddressConfig{"127.0.0.1", base + 1, base, base + 100};
    }

    SyncRoleConfig makeHostIgRole(int igUdpRecv, int base = 18000)
    {
        SyncRoleConfig role{};
        role.enableHost = true;
        role.enableIg = true;
        role.hostLocal = makeHostLocalEye(base);
        role.igLocal = makeIgLocalEye(igUdpRecv, base);
        role.hostEndpoint = makeHostEndpointEye(base);
        return role;
    }

    SyncRoleConfig makeIgOnlyRole(int igUdpRecv, int base = 18000)
    {
        SyncRoleConfig role{};
        role.enableHost = false;
        role.enableIg = true;
        role.igLocal = makeIgLocalEye(igUdpRecv, base);
        role.hostEndpoint = makeHostEndpointEye(base);
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
    requireLookAtMatchesPose(engine, position, eulerYprDeg);
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
        role.igLocal = makeIgLocalEye(18001, 18000);
        role.hostEndpoint = makeHostEndpointEye(18000);
        // Host 未启动 → Connect 失败，但仍完成本地 Init。
        REQUIRE(engine.synchronSystem().initialize(role, /*requireIgConnect=*/false));
        REQUIRE_FALSE(engine.synchronSystem().igLinked());

        const HostEyePose localPose{{1.0, 2.0, 3.0}, {10.0, 0.0, 0.0}};
        const HostEyePose hostPose{{100.0, 200.0, 50.0}, {45.0, 0.0, 0.0}};
        REQUIRE(engine.setCameraPose(localPose.position, localPose.eulerYprDeg));

        WHEN("a Host eye becomes available and sync update runs")
        {
            // 测试手法：queue 注入，绕过真报文，只钉门控行为。
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.synchronSystem().update(engine);

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
        REQUIRE(engine.setCameraPose(localPose.position, localPose.eulerYprDeg));

        WHEN("a Host eye becomes available and sync update runs")
        {
            engine.synchronSystem().setOffsetDeg({});
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.synchronSystem().update(engine);

            THEN("camera matches the Host eye")
            {
                requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYprDeg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.3 位姿合成：Host ⊕ offsetDeg
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
            engine.synchronSystem().update(engine);

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
            engine.synchronSystem().update(engine);

            THEN("camera matches Host position and Host euler plus offset")
            {
                requireLookAtMatchesPose(engine, expected.position, expected.eulerYprDeg);
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
        engine.synchronSystem().update(engine);
        requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYprDeg);

        WHEN("local pose is changed and update runs without a new Host eye")
        {
            REQUIRE(engine.setCameraPose(vsg::dvec3{0.0, 0.0, 0.0}, vsg::dvec3{0.0, 0.0, 0.0}));
            engine.synchronSystem().update(engine);

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
        engine.synchronSystem().update(engine);
        requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYprDeg);

        const HostEyePose localPose{{0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}};

        WHEN("local pose is changed and update runs without a new Host eye")
        {
            REQUIRE(engine.setCameraPose(localPose.position, localPose.eulerYprDeg));
            engine.synchronSystem().update(engine);

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
        engine.synchronSystem().update(engine);
        REQUIRE(engine.synchronSystem().igLinked());

        engine.synchronSystem().igSync().shutdown();
        REQUIRE_FALSE(engine.synchronSystem().igLinked());

        WHEN("local pose is changed and update runs while disconnected")
        {
            REQUIRE(engine.setCameraPose(vsg::dvec3{0.0, 0.0, 0.0}, vsg::dvec3{90.0, 0.0, 0.0}));
            engine.synchronSystem().update(engine);

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
        REQUIRE(engineA.synchronSystem().hasHost());
        REQUIRE(engineA.synchronSystem().igLinked());

        const HostEyePose localPose{{1.0, 1.0, 1.0}, {5.0, 0.0, 0.0}};
        const HostEyePose hostPose{{50.0, 60.0, 70.0}, {12.0, 0.0, 0.0}};
        REQUIRE(engineA.setCameraPose(localPose.position, localPose.eulerYprDeg));

        WHEN("a Host eye becomes available and sync update runs")
        {
            engineA.synchronSystem().setOffsetDeg({});
            engineA.synchronSystem().queueHostEyePose(hostPose);
            engineA.synchronSystem().update(engineA);

            THEN("camera matches Host eye with no authority bypass")
            {
                requireLookAtMatchesPose(engineA, hostPose.position, hostPose.eulerYprDeg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.6 端到端验收：真连接 + 真报文 + 多通道 offset / 防回声
// -----------------------------------------------------------------------------

SCENARIO("remote IG applies Host eye from live packets with channel offset",
         "[acceptance][bdd][sync][hostctrl][e2e]")
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
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(modelPath));

        OffsetDeg offsetB{60.0, 0.0, 0.0};
        engineA.synchronSystem().setOffsetDeg({});
        engineB.synchronSystem().setOffsetDeg(offsetB);

        const HostEyePose intentPose{{11.0, 22.0, 33.0}, {40.0, 0.0, 0.0}};
        const HostEyePose expectedB = hostEyePlusOffset(intentPose, offsetB);

        WHEN("A publishes authority pose and both engines tick")
        {
            REQUIRE(engineA.setCameraPose(intentPose.position, intentPose.eulerYprDeg));
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();
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
         "[acceptance][bdd][sync][hostctrl][e2e][anti-echo]")
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

        // 建立 Pose_old 为当前权威（应用层注入；E2E 断言走后续真 tick 扇出）。
        engineA.synchronSystem().queueHostEyePose(poseOld);
        engineA.synchronSystem().update(engineA);
        engineB.synchronSystem().queueHostEyePose(poseOld);
        engineB.synchronSystem().update(engineB);

        WHEN("A camera becomes Pose_new then a full tick fans out to B")
        {
            REQUIRE(engineA.setCameraPose(poseNew.position, poseNew.eulerYprDeg));
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
                auto sent = engineA.synchronSystem().lastSentHostEye();
                REQUIRE(sent.has_value());
                requirePoseNear(*sent, poseNew);
            }
        }
    }
}

SCENARIO("three channels share Host eye and differ only by channel offset",
         "[acceptance][bdd][sync][hostctrl][e2e][multi-ig]")
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
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 3);
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

        WHEN("A publishes intent and all channels tick")
        {
            REQUIRE(engineA.setCameraPose(intent.position, intent.eulerYprDeg));
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
