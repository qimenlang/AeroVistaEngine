#include <catch2/catch_test_macros.hpp>

#include "engine.h"
#include "function/sync/HostSync.h"
#include "function/sync/IgSync.h"

#include <cstdint>

// 1.HostSync和IgSync的初始化、连接测试
namespace
{
    // Defaults from doc/多通道同步模块设计.md:
    //   HostSync UDP: send → 8001, recv ← 8000
    //   IgSync   UDP: send → 8000, recv ← 8001
    //   TCP command: 8100
    // Initialize(local) = local bind / listen identity
    // IgSync::Connect(hostEndpoint) only

    AddressConfig makeHostLocal(const std::string& addr = "127.0.0.1")
    {
        return AddressConfig{addr, 8001, 8000, 8100};
    }

    AddressConfig makeIgLocal(const std::string& localAddr = "127.0.0.1", int udpRecvPort = 8001)
    {
        // udpPortSend=8000 → Host recv; udpPortRecv = local bind (unique per co-located IG)
        return AddressConfig{localAddr, 8000, udpRecvPort, 8100};
    }

    AddressConfig makeHostEndpoint(const std::string& addr = "127.0.0.1")
    {
        return AddressConfig{addr, 8001, 8000, 8100};
    }

    // UDP may drop: expect `expected`, allow missing up to `slack` (not over-count).
    bool approxAtMost(std::uint32_t actual, int expected, int slack)
    {
        const auto exp = static_cast<std::uint32_t>(expected);
        const auto minOk = exp > static_cast<std::uint32_t>(slack) ? exp - static_cast<std::uint32_t>(slack) : 0u;
        return actual >= minOk && actual <= exp;
    }
} // namespace

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
                // HostSync does not call Connect; only IgSync initiates.
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
                // TCP 8100 still matches Host; UDP recv port 9999 will not sync.
                AddressConfig badUdpEndpoint{"127.0.0.1", 8001, 9999, 8100};
                const bool connected = ig.Connect(badUdpEndpoint);

                THEN("overall connect fails and neither plane is ready")
                {
                    // Connect uses hostEndpoint.udpPortRecv as Host UDP bind port (see IgSync::connectOnce).
                    REQUIRE_FALSE(connected);
                    REQUIRE_FALSE(ig.tcpConnected());
                    REQUIRE_FALSE(ig.udpSynced());
                    REQUIRE_FALSE(host.hasReadyIg());
                }
            }
        }
    }
}

SCENARIO("HostSync accepts multiple IgSync connections", "[bdd][sync][connect][multi-ig]")
{
    GIVEN("a HostSync waiting for IGs")
    {
        HostSync host;
        REQUIRE(host.Initialize(makeHostLocal()));

        WHEN("two co-located IGs initialize on distinct UDP recv ports and connect")
        {
            // Same machine cannot bind two IGs to UDP 8001; use 8001 and 8003.
            // Host learns each IG recv port when the IG connects (e.g. via TCP handshake).
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

// 2.host ig 状态同步测试

// HostSync::Run + Update(simTime): one Update => one IGCtrl (60fps = caller pace)
// IgSync::Update: recv IGCtrl (+ reply one SOF); Host counts SOF (sofReceivedCount)

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
                // Monotonic sim time at 60 Hz (ms)
                host.Update(/*simTimeMs=*/i * (1000.0 / 60.0));
                ig.Update();
            }

            THEN("both are RUNNING and Host sent one IGCtrl per Update")
            {
                REQUIRE(host.status() == HostStatus::Running);
                // Ig enters Running after Connect (or first Update); no separate Ig.Run().
                REQUIRE(ig.status() == IgStatus::Running);
                REQUIRE(host.igCtrlSentCount() == kFrames);
                // Same-loop host.Update then ig.Update can deliver all; UDP may drop some.
                REQUIRE(approxAtMost(ig.igCtrlReceivedCount(), kFrames, 3));
            }
        }
    }
}

// App contract: one SOF reply per IGCtrl actually received (UDP may drop either direction).
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

// FreeRun send: Host must keep sending IGCtrl even when IG never replies SOF.
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
                host.Update(/*simTimeMs=*/i * (1000.0 / 60.0)); // must not block on SOF
                ig.Update(/*sendSof=*/false);                   // recv IGCtrl only
            }

            THEN("Host sent all IGCtrl without any SOF, independent of UDP delivery")
            {
                // Send-side contract (not gated on SOF). Recv count may be < sent on UDP loss.
                REQUIRE(host.igCtrlSentCount() == kFrames);
                REQUIRE(host.sofReceivedCount() == 0);
                REQUIRE(ig.igCtrlReceivedCount() <= kFrames);
            }
        }
    }
}

// 3. Engine + SynchronSystem integration (HostSync + IgSync as SynchronSystem members)
// Loop: preFrame IgSync recv/reply SOF → render → HostSync send IGCtrl (FreeRun).

SCENARIO("Engine SynchronSystem with Host and IG exchanges IGCtrl and SOF over 10 ticks",
         "[bdd][sync][engine]")
{
    GIVEN("an offscreen Engine whose SynchronSystem owns both HostSync and IgSync")
    {
        Engine engine;
        engine.extent = {1920, 1080};
        engine.showWindow = false;

        // Main-window dual role: temporary HostSync + IgSync under SynchronSystem.
        SyncRoleConfig syncRole{};
        syncRole.enableHost = true;
        syncRole.enableIg = true;
        syncRole.hostLocal = makeHostLocal();
        syncRole.igLocal = makeIgLocal();
        syncRole.hostEndpoint = makeHostEndpoint();

        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt";

        WHEN("the engine is initialized and renderOneTick runs 10 times")
        {
            REQUIRE(engine.init(modelPath, syncRole));

            constexpr int kTicks = 10;
            for (int i = 0; i < kTicks; ++i)
                REQUIRE(engine.renderOneTick());

            THEN("HostSync and IgSync exchanged IGCtrl/SOF via the engine loop")
            {
                SynchronSystem& sync = engine.synchronSystem();
                HostSync& host = sync.hostSync();
                IgSync& ig = sync.igSync();

                // Loop phase (design): preFrame recv → … → end-of-frame send ⇒ often ~N-1.
                // If an impl sends before recv in the same tick, recv may reach N. Cap at N.
                REQUIRE(host.igCtrlSentCount() == kTicks);
                REQUIRE(approxAtMost(ig.igCtrlReceivedCount(), kTicks, 3));
                REQUIRE(ig.sofSentCount() == ig.igCtrlReceivedCount());
                REQUIRE(host.sofReceivedCount() <= ig.sofSentCount());
                REQUIRE(approxAtMost(host.sofReceivedCount(), kTicks, 3));
            }
        }
    }
}

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

        // Co-located IGs need distinct udpRecv ports (see multi-ig connect scenario).
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
            // Connect all sync peers before any Vulkan device (device count is limited).
            REQUIRE(engineA.initSync(roleA));
            REQUIRE(engineB.initSync(roleB));
            REQUIRE(engineC.initSync(roleC));
            REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 3);
            REQUIRE(engineA.initGraphics(modelPath));

            constexpr int kTicks = 10;
            constexpr int kIgCount = 3;
            for (int i = 0; i < kTicks; ++i)
            {
                REQUIRE(engineA.renderOneTick()); // Host fans out IGCtrl; local IG may recv/reply
                engineB.tickSync();               // IG-only: no second/third Vulkan device
                engineC.tickSync();
            }

            THEN("each IG got ~N IGCtrl and A's Host got ~N*3 SOF")
            {
                HostSync& host = engineA.synchronSystem().hostSync();
                IgSync& igA = engineA.synchronSystem().igSync();
                IgSync& igB = engineB.synchronSystem().igSync();
                IgSync& igC = engineC.synchronSystem().igSync();

                REQUIRE(host.igCtrlSentCount() == kTicks); // send rounds (fan-out per tick)

                // Tick order A→B→C with end-of-frame send: B/C often ~N, local A often ~N-1.
                // Still require Host fan-out to local IG for this count (camera may ignore self-loop).
                // If Host skips local IG, igA≈0 and sof≈2N — that policy would need different expects.
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