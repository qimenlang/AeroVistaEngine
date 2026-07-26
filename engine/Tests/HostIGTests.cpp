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
// 1. 连接面：Initialize / Connect / 多 IG
// =============================================================================

// 验证 HostSync、IgSync 单独 Initialize 成功，且初始未就绪/未连接。
SCENARIO("HostSync or IgSync initializes successfully", "[bdd][sync][initialize]")
{
    GIVEN("a new HostSync")
    {
        HostSync host;

        WHEN("it is initialized")
        {
            const bool ok = host.Initialize(makeHostLocal());

            THEN("initialization succeeds and no IG is ready yet")
            {
                REQUIRE(ok);
                REQUIRE_FALSE(host.hasReadyIg());
                REQUIRE(host.readyIgCount() == 0);
            }
        }
    }

    GIVEN("a new IgSync")
    {
        IgSync ig;

        WHEN("it is initialized")
        {
            const bool ok = ig.Initialize(makeIgLocal());

            THEN("initialization succeeds and it is not connected to a Host yet")
            {
                REQUIRE(ok);
                REQUIRE_FALSE(ig.tcpConnected());
                REQUIRE_FALSE(ig.udpSynced());
            }
        }
    }
}

// 验证 Host 未启动时 IgSync::Connect 失败，TCP/UDP 状态均为未连接。
SCENARIO("IgSync connect fails when Host is not running", "[bdd][sync][connect][failure]")
{
    GIVEN("an IgSync initialized without a running HostSync")
    {
        IgSync ig;
        REQUIRE(ig.Initialize(makeIgLocal()));

        WHEN("the IG connects to a Host endpoint")
        {
            const bool connected = ig.Connect(makeHostEndpoint());

            THEN("connect fails and neither plane reports connected")
            {
                REQUIRE_FALSE(connected);
                REQUIRE_FALSE(ig.tcpConnected());
                REQUIRE_FALSE(ig.udpSynced());
            }
        }
    }
}

// 验证先连失败、Host 起来后再连可成功（重连路径）。
SCENARIO("IgSync connect fails then succeeds after HostSync starts", "[bdd][sync][connect][reconnect]")
{
    GIVEN("an IgSync initialized without a running HostSync")
    {
        IgSync ig;
        REQUIRE(ig.Initialize(makeIgLocal()));

        WHEN("the IG connects while the Host is down")
        {
            REQUIRE_FALSE(ig.Connect(makeHostEndpoint()));
            REQUIRE_FALSE(ig.tcpConnected());
            REQUIRE_FALSE(ig.udpSynced());

            AND_WHEN("the HostSync is initialized and the IG connects again")
            {
                HostSync host;
                REQUIRE(host.Initialize(makeHostLocal()));

                const bool connected = ig.Connect(makeHostEndpoint());

                THEN("connect succeeds on both planes and Host sees a ready IG")
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

// 验证 Host 已监听时，IgSync Connect 成功且 readyIgCount==1。
SCENARIO("IgSync connects to an initialized HostSync successfully", "[bdd][sync][connect]")
{
    GIVEN("a HostSync that has been initialized and is waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.Initialize(makeHostLocal()));
        REQUIRE_FALSE(host.hasReadyIg());

        AND_GIVEN("an IgSync that has been initialized")
        {
            IgSync ig;
            REQUIRE(ig.Initialize(makeIgLocal()));

            WHEN("the IG connects to the Host endpoint")
            {
                const bool connected = ig.Connect(makeHostEndpoint());

                THEN("IG reports TCP and UDP sync, and Host has one ready IG")
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

// 验证 TCP 端口正确但 UDP 对端端口错误时，整体 Connect 失败且 Host 无就绪 IG。
SCENARIO("IgSync connect fails when UDP peer ports are wrong but TCP port is valid", "[bdd][sync][connect][failure]")
{
    GIVEN("a HostSync waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.Initialize(makeHostLocal()));

        AND_GIVEN("an IgSync initialized with correct local ports")
        {
            IgSync ig;
            REQUIRE(ig.Initialize(makeIgLocal()));

            WHEN("the IG connects using a Host endpoint with wrong UDP ports")
            {
                // Connect 使用 hostEndpoint.udpPortRecv 作为 Host UDP 收端口。
                AddressConfig badUdpEndpoint{"127.0.0.1", 8001, 9999, 8100};
                const bool connected = ig.Connect(badUdpEndpoint);

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

// 验证同机多 IG 使用不同 udpRecv 端口时可同时接入，Host readyIgCount==2。
SCENARIO("HostSync accepts multiple IgSync connections", "[bdd][sync][connect][multi-ig]")
{
    GIVEN("a HostSync waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.Initialize(makeHostLocal()));

        WHEN("two co-located IGs initialize on distinct UDP recv ports and connect")
        {
            IgSync ig1;
            IgSync ig2;
            REQUIRE(ig1.Initialize(makeIgLocal("127.0.0.1", 8001)));
            REQUIRE(ig2.Initialize(makeIgLocal("127.0.0.1", 8003)));

            REQUIRE(ig1.Connect(makeHostEndpoint()));
            REQUIRE(ig2.Connect(makeHostEndpoint()));

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
// 2. 帧节拍：IGCtrl / SOF / FreeRun
// =============================================================================

// 验证 Host::Update×N 发送 N 轮 IGCtrl，IG 能收到（允许少量 UDP 丢包），双方进入 Running。
SCENARIO("HostSync and IgSync enter RUNNING and deliver IGCtrl per Update", "[bdd][sync][status]")
{
    GIVEN("a HostSync and an IgSync that have been initialized and connected")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.Initialize(makeHostLocal()));
        REQUIRE(ig.Initialize(makeIgLocal()));
        REQUIRE(ig.Connect(makeHostEndpoint()));

        WHEN("host runs and sends 10 IGCtrl frames while IG updates")
        {
            host.Run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                host.Update(/*simTimeMs=*/i * (1000.0 / 60.0));
                ig.Update();
            }

            THEN("both are RUNNING and Host sent one IGCtrl per Update")
            {
                REQUIRE(host.status() == HostStatus::Running);
                REQUIRE(ig.status() == IgStatus::Running);
                REQUIRE(host.igCtrlSentCount() == kFrames);
                REQUIRE(approxAtMost(ig.igCtrlReceivedCount(), kFrames, 3));
            }
        }
    }
}

// 验证应用层契约：每成功收到 1 条 IGCtrl 回 1 条 SOF；Host 收到数不超过 IG 发出数。
SCENARIO("IG replies with one SOF per received IGCtrl", "[bdd][sync][status][sof]")
{
    GIVEN("a HostSync and an IgSync that have been initialized and connected")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.Initialize(makeHostLocal()));
        REQUIRE(ig.Initialize(makeIgLocal()));
        REQUIRE(ig.Connect(makeHostEndpoint()));

        WHEN("host sends 10 IGCtrl and IG updates each frame")
        {
            host.Run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                host.Update(/*simTimeMs=*/i * (1000.0 / 60.0));
                ig.Update(/*sendSof=*/true);
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

// 验证 FreeRun：IG 不回 SOF 时 Host 仍能发齐 N 轮 IGCtrl，发送不门控在 SOF 上。
SCENARIO("HostSync sends IGCtrl without depending on SOF", "[bdd][sync][status][freerun]")
{
    GIVEN("a connected HostSync/IgSync with FreeRun IGCtrl send pace")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.Initialize(makeHostLocal()));
        REQUIRE(ig.Initialize(makeIgLocal()));
        REQUIRE(ig.Connect(makeHostEndpoint()));

        SyncPaceConfig pace{};
        pace.igCtrlSendPace = SendPace::FreeRun;
        pace.frameGate = FrameGate::FreeRun;
        host.SetPaceConfig(pace);

        WHEN("host sends 10 IGCtrl while IG receives but never replies SOF")
        {
            host.Run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                host.Update(/*simTimeMs=*/i * (1000.0 / 60.0));
                ig.Update(/*sendSof=*/false);
            }

            THEN("Host sent all IGCtrl without any SOF, independent of UDP delivery")
            {
                REQUIRE(host.igCtrlSentCount() == kFrames);
                REQUIRE(host.sofReceivedCount() == 0);
                REQUIRE(ig.igCtrlReceivedCount() <= kFrames);
            }
        }
    }
}

// 验证 IG 可查询最新收到的 IGCtrl.FrameCntr；每成功收一包则与本轮 Host 下发帧号一致。
// 红灯 API：IgSync::lastIgCtrlFrameCntr()（待实现对外暴露）。
SCENARIO("IgSync exposes last received IGCtrl frame counter correctly", "[bdd][sync][status][frame]")
{
    GIVEN("a HostSync and an IgSync that have been initialized and connected")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.Initialize(makeHostLocal()));
        REQUIRE(ig.Initialize(makeIgLocal()));
        REQUIRE(ig.Connect(makeHostEndpoint()));

        WHEN("host sends IGCtrl frames 0..N-1 and IG updates each frame")
        {
            host.Run();
            constexpr int kFrames = 10;
            std::uint32_t prevReceived = 0;
            int matchedFrames = 0;

            for (int i = 0; i < kFrames; ++i)
            {
                host.Update(/*simTimeMs=*/i * (1000.0 / 60.0)); // Host 本轮 frameCntr == i
                ig.Update();

                if (ig.igCtrlReceivedCount() > prevReceived)
                {
                    REQUIRE(ig.lastIgCtrlFrameCntr() == static_cast<std::uint32_t>(i));
                    prevReceived = ig.igCtrlReceivedCount();
                    ++matchedFrames;
                }
            }

            THEN("at least one IGCtrl was received and lastIgCtrlFrameCntr matched Host frameCntr")
            {
                REQUIRE(matchedFrames >= 1);
                REQUIRE(ig.igCtrlReceivedCount() == static_cast<std::uint32_t>(matchedFrames));
                REQUIRE(ig.lastIgCtrlFrameCntr() < static_cast<std::uint32_t>(kFrames));
            }
        }
    }
}

// =============================================================================
// 3. Engine + SynchronSystem 集成
// =============================================================================

// 验证单 Engine（Host+IG）经 tickOnFrame×N 完成 IGCtrl/SOF 收发契约。
SCENARIO("Engine SynchronSystem with Host and IG exchanges IGCtrl and SOF over 10 ticks",
         "[bdd][sync][engine]")
{
    GIVEN("an offscreen Engine whose SynchronSystem owns both HostSync and IgSync")
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

        WHEN("the engine is initialized and tickOnFrame runs 10 times")
        {
            REQUIRE(engine.init(modelPath, syncRole));

            constexpr int kTicks = 10;
            for (int i = 0; i < kTicks; ++i)
                REQUIRE(engine.tickOnFrame());

            THEN("HostSync and IgSync exchanged IGCtrl/SOF via the engine loop")
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

// 验证三通道：A=Host+IG，B/C=仅 IG；先全员 initSync，A 再 initGraphics；
// 各 IG 约收到 N 条 IGCtrl，A.Host 约收到 N×3 条 SOF（B/C 用 tickSync 避免多 Vulkan Device）。
SCENARIO("three Engines (A Host+IG, B/C IG-only) exchange IGCtrl/SOF over 10 ticks",
         "[bdd][sync][engine][multi-ig]")
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

            THEN("each IG got ~N IGCtrl and A's Host got ~N*3 SOF")
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
// 4. Host 控制 IG 相机位姿（mainCamera / setCameraPose(pos,euler) / SynchronSystem::update）
// 约定：ABC 同为 IG，已连接时最终位姿来自 Host 回灌；本节目测 update 门控（queue 注入，非端到端报文）。
// =============================================================================

namespace
{
    vsg::dquat quatFromEulerYPR_deg(const vsg::dvec3& eulerYPR_deg)
    {
        return vsg::dquat(vsg::radians(eulerYPR_deg.x), vsg::dvec3(0.0, 0.0, 1.0)) *
               vsg::dquat(vsg::radians(eulerYPR_deg.y), vsg::dvec3(1.0, 0.0, 0.0)) *
               vsg::dquat(vsg::radians(eulerYPR_deg.z), vsg::dvec3(0.0, 1.0, 0.0));
    }

    void requireLookAtMatchesPose(Engine& engine, const vsg::dvec3& position, const vsg::dvec3& eulerYPR_deg)
    {
        auto lookAt = engine.mainCamera()->viewMatrix.cast<vsg::LookAt>();
        REQUIRE(lookAt);

        const vsg::dquat rotation = quatFromEulerYPR_deg(eulerYPR_deg);
        const vsg::dvec3 expectedForward = rotation * vsg::dvec3(0.0, 1.0, 0.0);
        const vsg::dvec3 expectedUp = rotation * vsg::dvec3(0.0, 0.0, 1.0);
        const vsg::dvec3 expectedCenter = position + expectedForward;

        REQUIRE(vsg::length(lookAt->eye - position) < 1e-9);
        REQUIRE(vsg::length(lookAt->center - expectedCenter) < 1e-9);
        REQUIRE(vsg::length(vsg::normalize(lookAt->up) - vsg::normalize(expectedUp)) < 1e-9);
    }
} // namespace

// 验证 mainCamera 可用，且 setCameraPose(pos, eulerYPR°) 正确写入 LookAt（Y-forward、Z-up）。
SCENARIO("Engine get camera and set camera pose from position and euler YPR", "[bdd][sync][hostctrl]")
{
    GIVEN("an offscreen Engine with graphics initialized")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath));

        WHEN("the main camera is obtained")
        {
            auto camera = engine.mainCamera();

            THEN("main camera and LookAt are available")
            {
                REQUIRE(camera);
                REQUIRE(camera->viewMatrix.cast<vsg::LookAt>());
            }

            AND_WHEN("camera pose is set from position and euler YPR degrees")
            {
                const vsg::dvec3 position{10.0, -20.0, 5.0};
                const vsg::dvec3 eulerYPR_deg{90.0, 0.0, 0.0}; // yaw 90° about Z

                REQUIRE(engine.setCameraPose(position, eulerYPR_deg));

                THEN("LookAt eye/center/up match position and euler (Y-forward, Z-up)")
                {
                    requireLookAtMatchesPose(engine, position, eulerYPR_deg);
                }
            }
        }
    }
}