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
            const bool ok = host.initialize(makeHostLocal());

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

// 验证 Host 未启动时 IgSync::Connect 失败，TCP/UDP 状态均为未连接。
SCENARIO("IgSync connect fails when Host is not running", "[bdd][sync][connect][failure]")
{
    GIVEN("an IgSync initialized without a running HostSync")
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

// 验证先连失败、Host 起来后再连可成功（重连路径）。
SCENARIO("IgSync connect fails then succeeds after HostSync starts", "[bdd][sync][connect][reconnect]")
{
    GIVEN("an IgSync initialized without a running HostSync")
    {
        IgSync ig;
        REQUIRE(ig.initialize(makeIgLocal()));

        WHEN("the IG connects while the Host is down")
        {
            REQUIRE_FALSE(ig.connect(makeHostEndpoint()));
            REQUIRE_FALSE(ig.tcpConnected());
            REQUIRE_FALSE(ig.udpSynced());

            AND_WHEN("the HostSync is initialized and the IG connects again")
            {
                HostSync host;
                REQUIRE(host.initialize(makeHostLocal()));

                const bool connected = ig.connect(makeHostEndpoint());

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
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE_FALSE(host.hasReadyIg());

        AND_GIVEN("an IgSync that has been initialized")
        {
            IgSync ig;
            REQUIRE(ig.initialize(makeIgLocal()));

            WHEN("the IG connects to the Host endpoint")
            {
                const bool connected = ig.connect(makeHostEndpoint());

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
        REQUIRE(host.initialize(makeHostLocal()));

        AND_GIVEN("an IgSync initialized with correct local ports")
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

// 验证同机多 IG 使用不同 udpRecv 端口时可同时接入，Host readyIgCount==2。
SCENARIO("HostSync accepts multiple IgSync connections", "[bdd][sync][connect][multi-ig]")
{
    GIVEN("a HostSync waiting for IGs")
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
// 2. 帧节拍：IGCtrl / SOF / FreeRun
// =============================================================================

// 验证 Host::Update×N 发送 N 轮 IGCtrl，IG 能收到（允许少量 UDP 丢包），双方进入 Running。
SCENARIO("HostSync and IgSync enter RUNNING and deliver IGCtrl per Update", "[bdd][sync][status]")
{
    GIVEN("a HostSync and an IgSync that have been initialized and connected")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeHostEndpoint()));

        WHEN("host runs and sends 10 IGCtrl frames while IG updates")
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
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeHostEndpoint()));

        WHEN("host sends 10 IGCtrl and IG updates each frame")
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

// 验证 FreeRun：IG 不回 SOF 时 Host 仍能发齐 N 轮 IGCtrl，发送不门控在 SOF 上。
SCENARIO("HostSync sends IGCtrl without depending on SOF", "[bdd][sync][status][freerun]")
{
    GIVEN("a connected HostSync/IgSync with FreeRun IGCtrl send pace")
    {
        HostSync host;
        IgSync ig;
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeHostEndpoint()));

        SyncPaceConfig pace{};
        pace.igCtrlSendPace = SendPace::FreeRun;
        pace.frameGate = FrameGate::FreeRun;
        host.setPaceConfig(pace);

        WHEN("host sends 10 IGCtrl while IG receives but never replies SOF")
        {
            host.run();
            constexpr int kFrames = 10;
            for (int i = 0; i < kFrames; ++i)
            {
                host.update(/*simTimeMs=*/i * (1000.0 / 60.0));
                ig.update(/*sendSof=*/false);
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
        REQUIRE(host.initialize(makeHostLocal()));
        REQUIRE(ig.initialize(makeIgLocal()));
        REQUIRE(ig.connect(makeHostEndpoint()));

        WHEN("host sends IGCtrl frames 0..N-1 and IG updates each frame")
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
// 4. Host 控制 IG 相机位姿
// 约定：ABC 同为 IG；已连接时最终位姿 = Host 眼点 ⊕ 本地 offsetDeg。
// 分层：queue 注入钉门控/合成/无新包策略；E2E 钉真报文 + 覆盖前采样防回声。
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

    void requirePoseNear(const HostEyePose& actual, const HostEyePose& expected, double eps = 1e-6)
    {
        REQUIRE(vsg::length(actual.position - expected.position) < eps);
        REQUIRE(vsg::length(actual.eulerYPR_deg - expected.eulerYPR_deg) < eps);
    }

    /// 分量相加：最终 euler = Host.euler + offsetDeg（初步约定，与实现注释对齐）。
    HostEyePose hostEyePlusOffset(const HostEyePose& host, const OffsetDeg& offset)
    {
        HostEyePose out = host;
        out.eulerYPR_deg.x += offset.yaw;
        out.eulerYPR_deg.y += offset.pitch;
        out.eulerYPR_deg.z += offset.roll;
        return out;
    }

    // Host 眼点 BDD 使用独立端口，避免与 §1–3 默认 8000/8001 并行冲突。
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
// 4.1 基础：mainCamera / setCameraPose
// -----------------------------------------------------------------------------

// 验证 mainCamera 可用，且 setCameraPose(pos, eulerYPR°) 正确写入 LookAt（Y-forward、Z-up）。
// 这是后续 Host 回灌断言的标尺，本身不测同步门控。
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

// -----------------------------------------------------------------------------
// 4.2 门控（queue 注入）：未连接不覆盖 / 已连接覆盖
// -----------------------------------------------------------------------------

// 门控-关：未连接 Host 时，即便 queue 注入眼点，SynchronSystem::update 也不得改写相机。
SCENARIO("unlinked IG does not apply queued Host eye on update", "[bdd][sync][hostctrl][gate]")
{
    GIVEN("an Engine with graphics but SynchronSystem IG not linked to a Host")
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
        REQUIRE(engine.setCameraPose(localPose.position, localPose.eulerYPR_deg));

        WHEN("a Host eye is queue-injected and update runs")
        {
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.synchronSystem().update(engine);

            THEN("camera stays at the local pose")
            {
                requireLookAtMatchesPose(engine, localPose.position, localPose.eulerYPR_deg);
            }
        }
    }
}

// 门控-开：已连接时 queue 注入 Host 眼点，update 后相机变为 Host 位姿（offset=0）。
SCENARIO("linked IG applies queued Host eye on update", "[bdd][sync][hostctrl][gate]")
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
        REQUIRE(engine.setCameraPose(localPose.position, localPose.eulerYPR_deg));

        WHEN("a Host eye is queue-injected and update runs")
        {
            engine.synchronSystem().setOffsetDeg({});
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.synchronSystem().update(engine);

            THEN("camera matches the Host eye")
            {
                requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYPR_deg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.3 位姿合成：Host ⊕ offsetDeg（queue 注入）
// -----------------------------------------------------------------------------

// offset=0：最终位姿等于 Host 眼点。
SCENARIO("linked IG with zero offset applies Host eye unchanged", "[bdd][sync][hostctrl][offset]")
{
    GIVEN("a linked Host+IG Engine with offsetDeg all zero")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(18001, 18100)));
        engine.synchronSystem().setOffsetDeg({0.0, 0.0, 0.0});

        const HostEyePose hostPose{{10.0, -5.0, 2.0}, {0.0, 0.0, 0.0}};

        WHEN("Host eye is queued and update runs")
        {
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.synchronSystem().update(engine);

            THEN("final pose equals Host eye")
            {
                requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYPR_deg);
            }
        }
    }
}

// 非零 offset：最终 euler = Host.euler + offsetDeg（位置仍用 Host.pos）。
SCENARIO("linked IG applies Host eye plus channel offsetDeg", "[bdd][sync][hostctrl][offset]")
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

        WHEN("Host eye is queued and update runs")
        {
            engine.synchronSystem().queueHostEyePose(hostPose);
            engine.synchronSystem().update(engine);

            THEN("camera matches Host position and Host.euler + offsetDeg")
            {
                requireLookAtMatchesPose(engine, expected.position, expected.eulerYPR_deg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.4 无新包策略与断线（queue 注入）
// -----------------------------------------------------------------------------

// ReuseLast：已连接、本帧无新眼点时，update 仍用缓存 Host 眼点写相机（可覆盖本地改动）。
SCENARIO("ReuseLast re-applies cached Host eye when no new packet", "[bdd][sync][hostctrl][stale]")
{
    GIVEN("a linked Engine with ReuseLast after one Host eye was applied")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(18001, 18300)));
        engine.synchronSystem().setHostEyeStalePolicy(HostEyeStalePolicy::ReuseLast);
        engine.synchronSystem().setOffsetDeg({});

        const HostEyePose hostPose{{8.0, 9.0, 10.0}, {15.0, 0.0, 0.0}};
        engine.synchronSystem().queueHostEyePose(hostPose);
        engine.synchronSystem().update(engine);
        requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYPR_deg);

        WHEN("local pose is changed and update runs without a new queued eye")
        {
            REQUIRE(engine.setCameraPose(vsg::dvec3{0.0, 0.0, 0.0}, vsg::dvec3{0.0, 0.0, 0.0}));
            engine.synchronSystem().update(engine);

            THEN("camera returns to the cached Host eye")
            {
                requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYPR_deg);
            }
        }
    }
}

// Freeze：已连接、本帧无新眼点时，update 不再改写 LookAt（本地改动得以保留）。
SCENARIO("Freeze leaves camera unchanged when no new Host eye", "[bdd][sync][hostctrl][stale]")
{
    GIVEN("a linked Engine with Freeze after one Host eye was applied")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";
        REQUIRE(engine.init(modelPath, makeHostIgRole(18001, 18400)));
        engine.synchronSystem().setHostEyeStalePolicy(HostEyeStalePolicy::Freeze);
        engine.synchronSystem().setOffsetDeg({});

        const HostEyePose hostPose{{8.0, 9.0, 10.0}, {15.0, 0.0, 0.0}};
        engine.synchronSystem().queueHostEyePose(hostPose);
        engine.synchronSystem().update(engine);
        requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYPR_deg);

        const HostEyePose localPose{{0.0, 0.0, 1.0}, {0.0, 0.0, 0.0}};

        WHEN("local pose is changed and update runs without a new queued eye")
        {
            REQUIRE(engine.setCameraPose(localPose.position, localPose.eulerYPR_deg));
            engine.synchronSystem().update(engine);

            THEN("camera stays at the local pose")
            {
                requireLookAtMatchesPose(engine, localPose.position, localPose.eulerYPR_deg);
            }
        }
    }
}

// 断线：曾连接并应用过 Host 眼点后断开；update 仍保留最后一帧 Host 位姿，不回到断线前 Trackball。
SCENARIO("after disconnect, update keeps last Host eye pose", "[bdd][sync][hostctrl][disconnect]")
{
    GIVEN("a linked Engine that applied a Host eye then IG plane shut down")
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
                requireLookAtMatchesPose(engine, hostPose.position, hostPose.eulerYPR_deg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.5 权威窗 A 同样回灌（queue 注入，不为 A 开旁路）
// -----------------------------------------------------------------------------

// A（Host+IG）已连接时同样被 Host 眼点覆盖，验证权威窗无“跳过回灌”旁路。
SCENARIO("authority window A also applies Host eye on update", "[bdd][sync][hostctrl][authority]")
{
    GIVEN("Engine A with Host+IG linked")
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
        REQUIRE(engineA.setCameraPose(localPose.position, localPose.eulerYPR_deg));

        WHEN("Host eye is queue-injected onto A and update runs")
        {
            engineA.synchronSystem().setOffsetDeg({});
            engineA.synchronSystem().queueHostEyePose(hostPose);
            engineA.synchronSystem().update(engineA);

            THEN("A camera matches Host eye (no bypass)")
            {
                requireLookAtMatchesPose(engineA, hostPose.position, hostPose.eulerYPR_deg);
            }
        }
    }
}

// -----------------------------------------------------------------------------
// 4.6 端到端：真连接 + 真报文 + 多通道 offset / 防回声
// 说明：同机多 Engine 仅 A initGraphics；B/C 用 tickSync + lastAppliedHostEye 断言
//      （规避「Device 数量超限」；位姿契约与有窗 IG 相同）。
// -----------------------------------------------------------------------------

// E2E：A(Host+IG) 与 B(仅 IG) 真连接；A 扇出 Host 眼点报文后，B 应用为 Host ⊕ offset_B。
SCENARIO("E2E Host eye packet from A is applied on B with offsetDeg", "[bdd][sync][hostctrl][e2e]")
{
    GIVEN("Engine A Host+IG (graphics) and Engine B IG-only (sync-only)")
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

        WHEN("A sets authority pose and both engines tick so Host fans out EntityPosition+IGCtrl")
        {
            REQUIRE(engineA.setCameraPose(intentPose.position, intentPose.eulerYPR_deg));
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();

            THEN("B lastApplied matches Host intent ⊕ offsetB")
            {
                auto applied = engineB.synchronSystem().lastAppliedHostEye();
                REQUIRE(applied.has_value());
                requirePoseNear(*applied, expectedB);
            }
        }
    }
}

// E2E 防回声：A 在回灌前采到 Pose_new；扇出应为 Pose_new，而非 update 盖回的 Pose_old。
// B 的 lastApplied 与 A 的 lastSent 共同验证（不依赖真 Trackball）。
SCENARIO("E2E sample-before-overwrite fans out Pose_new not echoed Pose_old",
         "[bdd][sync][hostctrl][e2e][anti-echo]")
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

        // 建立 Pose_old 为当前权威（queue 注入；报文路径落地后亦可由上一帧建立）。
        engineA.synchronSystem().queueHostEyePose(poseOld);
        engineA.synchronSystem().update(engineA);
        engineB.synchronSystem().queueHostEyePose(poseOld);
        engineB.synchronSystem().update(engineB);

        WHEN("A LookAt becomes Pose_new then a full tick fans out to B")
        {
            REQUIRE(engineA.setCameraPose(poseNew.position, poseNew.eulerYPR_deg));
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();

            THEN("B follows Pose_new (not stuck on echoed Pose_old)")
            {
                auto applied = engineB.synchronSystem().lastAppliedHostEye();
                REQUIRE(applied.has_value());
                requirePoseNear(*applied, poseNew);
            }

            AND_THEN("Host lastSent authority eye is Pose_new when observable")
            {
                auto sent = engineA.synchronSystem().lastSentHostEye();
                REQUIRE(sent.has_value());
                requirePoseNear(*sent, poseNew);
            }
        }
    }
}

// E2E 三通道：同一 Host 眼点，B/C 仅因 offsetDeg 不同而最终位姿不同。
SCENARIO("E2E three channels share Host eye and differ only by offsetDeg",
         "[bdd][sync][hostctrl][e2e][multi-ig]")
{
    GIVEN("A Host+IG (graphics), B and C IG-only (sync-only) with yaw offsets -60 / +60")
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
            REQUIRE(engineA.setCameraPose(intent.position, intent.eulerYPR_deg));
            for (int i = 0; i < 2; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
                engineC.tickSync();
            }

            THEN("A LookAt and B/C lastApplied match Host intent ⊕ respective offsetDeg")
            {
                requireLookAtMatchesPose(engineA, expectA.position, expectA.eulerYPR_deg);
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