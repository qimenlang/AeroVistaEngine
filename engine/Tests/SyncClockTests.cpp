// 时钟同步方案.md §6 验收码 CLK-*：注入 / 相位展开 / 冻结 / e2e 真链路。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Common.h"
#include "engine.h"
#include <aerovista/sync/HostSync.h>
#include <aerovista/sync/IgSync.h>
#include <aerovista/sync/SynchronSystem.h>

#include <chrono>
#include <cstdint>
#include <thread>

using aerovista::sync::HostSync;
using aerovista::sync::IgSync;
namespace
{
    // 时钟同步方案.md §3/§4 注入结构体：IgSync::HostTimeStamp 提供真实实现。
    using HostTimeStamp = IgSync::HostTimeStamp;

    /// 业务侧扇出一帧 IGCtrl（outMsgWithIgCtrlUdp 自动前置 IGCtrl + 自计时时间戳）。
    void hostSendFrame(HostSync& host)
    {
        auto& omsg = host.outMsgWithIgCtrlUdp();
        host.flushUdp();
    }

    /// UDP 回环仍可能跨过「同一次 tick」：发送后轮询 drain，直到可观察条件成立。
    template<typename Pred>
    bool tickUntil(Engine& engine, Pred ready, int maxAttempts = 50)
    {
        for (int i = 0; i < maxAttempts; ++i)
        {
            if (ready())
                return true;
            engine.tickSync();
            if (ready())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return ready();
    }
} // namespace

// =============================================================================
// 系统测试：真实 socket。验收码 CLK-consume / CLK-consistent / CLK-freeze / CLK-realtime。
// =============================================================================

SCENARIO("IG derives simulation time from live Host time stamps plus local elapsed",
         "[acceptance][bdd][sync][clock][e2e][CLK-consume]")
{
    GIVEN("an independent HostSync and IG-only Engine B, linked over real sockets")
    {
        constexpr int kBase = 26000;
        HostSync hostA;
        Engine engineB;
        engineB.extent = {640, 480};
        engineB.showWindow = false;

        REQUIRE(hostA.initialize(makeTestHostConfig(kBase)));
        hostA.run();
        REQUIRE(engineB.initSync(makeTestIgConfig(kBase + 3, kBase)));
        REQUIRE(hostA.readyIgCount() == 1);

        WHEN("Host fans out real IGCtrl time stamps and IG ticks")
        {
            constexpr int kTicks = 10;
            for (int i = 0; i < kTicks; ++i)
            {
                hostSendFrame(hostA);
                engineB.tickSync();
                // 模拟帧节奏，保证 Host 自计时时间戳随真实时间推进（10 帧 ≥ 100ms）。
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            THEN("IG simulation time is anchored on the Host time stamp and advances with local elapsed")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                REQUIRE(ig.igCtrlReceivedCount() > 0);
                REQUIRE(ig.lastIgCtrlFrameCntr() > 0);

                // 关键断言：基准即 Host 时间戳（非本地时钟从 0 起）。
                // HostSync 自计时（_startTime 于 initialize 记录，steady_clock 连续推进），
                // 10 帧 × ~16.67ms ≈ 166ms（含握手耗时）。
                const std::uint64_t hostBaseUs = ig.lastHostSimTimeUs();
                REQUIRE(hostBaseUs > 0);

                // 消费时刻 ≥ Host 基准（本地流逝补偿有效，且不小于 Host 时间戳基准，含 >1 帧延迟）。
                const auto nowUs = [](auto now) {
                    return static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
                };
                const std::uint64_t s0 = ig.simTimeUsAt(nowUs(vsg::clock::now()));
                REQUIRE(s0 >= hostBaseUs);

                // Host 时间戳前进验证：10 帧 × ~16.67ms ≈ 166ms（> 100ms），证明基准确实来自 Host。
                REQUIRE(hostBaseUs >= 100000); // ≥ 100ms
            }
        }
    }
}

SCENARIO("two IG channels derive nearly identical simulation time from the shared Host",
         "[acceptance][bdd][sync][clock][e2e][consistency][CLK-consistent]")
{
    GIVEN("an independent HostSync and two IG-only engines B and C, linked over real sockets")
    {
        constexpr int kBase = 27000;
        HostSync hostA;
        Engine engineB;
        Engine engineC;
        engineB.extent = engineC.extent = {640, 480};
        engineB.showWindow = engineC.showWindow = false;

        REQUIRE(hostA.initialize(makeTestHostConfig(kBase)));
        hostA.run();
        REQUIRE(engineB.initSync(makeTestIgConfig(kBase + 3, kBase), makeTestSyncSystem(0)));
        REQUIRE(engineC.initSync(makeTestIgConfig(kBase + 5, kBase), makeTestSyncSystem(1)));
        REQUIRE(hostA.readyIgCount() == 2);

        WHEN("Host fans out real time stamps to both IGs")
        {
            constexpr int kTicks = 10;
            for (int i = 0; i < kTicks; ++i)
            {
                hostSendFrame(hostA);
                engineB.tickSync();
                engineC.tickSync();
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
            }

            THEN("both IGs anchor on the same Host time stamp and stay within a small difference")
            {
                IgSync& igB = engineB.synchronSystem().igSync();
                IgSync& igC = engineC.synchronSystem().igSync();
                REQUIRE(igB.igCtrlReceivedCount() > 0);
                REQUIRE(igC.igCtrlReceivedCount() > 0);

                const auto nowUs = [](auto now) {
                    return static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
                };
                const std::uint64_t now = nowUs(vsg::clock::now());
                const std::uint64_t sB = igB.simTimeUsAt(now);
                const std::uint64_t sC = igC.simTimeUsAt(now);

                // 两 IG 用同一 Host 时间戳基准（同一帧），模拟时间应接近。
                // 网络接收延迟差（LAN 内 < 一帧，远小于帧时长 16.67ms）。
                REQUIRE(sB > 0);
                REQUIRE(sC > 0);
                const std::uint64_t diff = sB > sC ? sB - sC : sC - sB;
                REQUIRE(diff < 16670); // < 一帧 us，证明一致性
            }
        }
    }
}

TEST_CASE("simTimeUs does not go backwards on consecutive reads",
          "[unit][sync][clock][monotonic][CLK-monotonic]")
{
    // CLK-monotonic：simTimeUs() 内部取 now，连续两次读取不小于基准、不回退。
    // 禁止 system_clock 是 §4.2 实现约束，本用例不拨墙钟。
    IgSync ig;
    ig.queueHostTimeStamp(HostTimeStamp{100, 1000, 0}); // lastSimTimeUs = 10000us

    const std::uint64_t s1 = ig.simTimeUs();
    const std::uint64_t s2 = ig.simTimeUs();

    REQUIRE(s1 >= 10000);
    REQUIRE(s2 >= s1);
}

SCENARIO("IG freezes when the Host stops sending time stamps over the real link",
         "[acceptance][bdd][sync][clock][e2e][freeze][CLK-freeze]")
{
    GIVEN("an independent HostSync and IG-only Engine B linked with a short freeze timeout")
    {
        constexpr int kBase = 28000;
        HostSync hostA;
        Engine engineB;
        engineB.extent = {640, 480};
        engineB.showWindow = false;

        REQUIRE(hostA.initialize(makeTestHostConfig(kBase)));
        hostA.run();
        REQUIRE(engineB.initSync(makeTestIgConfig(kBase + 3, kBase)));
        REQUIRE(hostA.readyIgCount() == 1);

        // 设置冻结阈值（真实链路几帧内就会触发，否则默认 200ms）。
        engineB.synchronSystem().igSync().setExtrapolateTimeoutUs(50000); // 50ms

        WHEN("Host sends a few frames then stops, while the IG keeps ticking")
        {
            constexpr int kHostTicks = 5;
            for (int i = 0; i < kHostTicks; ++i)
            {
                hostSendFrame(hostA);
                engineB.tickSync();
            }
            REQUIRE(engineB.synchronSystem().igSync().igCtrlReceivedCount() > 0);
            REQUIRE_FALSE(engineB.synchronSystem().igSync().frozen());

            // Host 停止后，只有 IG 持续 tick，超过 50ms 后冻结。
            for (int i = 0; i < 20; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                engineB.tickSync();
            }

            THEN("the IG enters frozen state and sim time stops advancing")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                REQUIRE(ig.frozen());

                // 冻结：simTimeUs 保持恒定，不再随本地流逝推进。
                const std::uint64_t s1 = ig.simTimeUs();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const std::uint64_t s2 = ig.simTimeUs();
                REQUIRE(s2 == s1);
            }
        }
    }
}

SCENARIO("Host simulation time advances with wall-clock pauses, not fixed steps",
         "[acceptance][bdd][sync][clock][e2e][real-time][CLK-realtime]")
{
    GIVEN("an independent HostSync and IG-only Engine B linked over real sockets")
    {
        constexpr int kBase = 29000;
        HostSync hostA;
        Engine engineB;
        engineB.extent = {640, 480};
        engineB.showWindow = false;

        REQUIRE(hostA.initialize(makeTestHostConfig(kBase)));
        hostA.run();
        REQUIRE(engineB.initSync(makeTestIgConfig(kBase + 3, kBase)));
        REQUIRE(hostA.readyIgCount() == 1);

        WHEN("the Host pauses between frames and then sends the next time stamp")
        {
            IgSync& ig = engineB.synchronSystem().igSync();

            // 契约只需要两个已确认的 Host 时间戳，中间夹一次墙钟暂停。
            // 不能「发完立刻 tick 再钉死收包数」：CI 上回环 UDP 经常晚一拍到达。
            hostSendFrame(hostA);
            REQUIRE(tickUntil(engineB, [&] { return ig.igCtrlReceivedCount() >= 1; }));
            const std::uint64_t t0 = ig.lastHostSimTimeUs();
            REQUIRE(t0 > 0);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            hostSendFrame(hostA);
            REQUIRE(tickUntil(engineB, [&] { return ig.lastHostSimTimeUs() > t0; }));
            const std::uint64_t t1 = ig.lastHostSimTimeUs();

            THEN("the sim time advance follows the pause (real-time), not a fixed 16.67ms step")
            {
                REQUIRE(t1 > t0);
                const std::uint64_t advanceUs = t1 - t0;

                // HostSync 自计时（steady_clock 连续推进）：advance ≈ 暂停 100ms，不是固定 16.67ms。
                REQUIRE(advanceUs >= 80000);  // ≥ 80ms（100ms 暂停的 80%）
                REQUIRE(advanceUs <= 200000); // ≤ 200ms（含调度波动上限）
            }
        }
    }
}

SCENARIO("IG freezes when the Host goes offline and stops sending time stamps",
         "[acceptance][bdd][sync][clock][e2e][freeze][host-offline][CLK-freeze]")
{
    GIVEN("an independent HostSync and IG-only Engine B linked with a short freeze timeout")
    {
        constexpr int kBase = 30000;
        HostSync hostA;
        Engine engineB;
        engineB.extent = {640, 480};
        engineB.showWindow = false;

        REQUIRE(hostA.initialize(makeTestHostConfig(kBase)));
        hostA.run();
        REQUIRE(engineB.initSync(makeTestIgConfig(kBase + 3, kBase)));
        REQUIRE(hostA.readyIgCount() == 1);

        // 设置冻结阈值（真实链路几帧内就会触发，否则默认 200ms）。
        engineB.synchronSystem().igSync().setExtrapolateTimeoutUs(50000); // 50ms

        WHEN("Host and IG tick normally, then the Host goes offline while the IG keeps ticking")
        {
            // 正常 tick，B 收到时间戳且未触发冻结。
            for (int i = 0; i < 5; ++i)
            {
                hostSendFrame(hostA);
                engineB.tickSync();
            }
            REQUIRE(engineB.synchronSystem().igSync().igCtrlReceivedCount() > 0);
            REQUIRE_FALSE(engineB.synchronSystem().igSync().frozen());

            // hostA 关闭（等价于 Host 进程退出，关闭 TCP/UDP），B 持续 tick，超过冻结阈值。
            hostA.shutdown();

            for (int i = 0; i < 20; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                engineB.tickSync();
            }

            THEN("the IG enters frozen state and sim time stops advancing")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                REQUIRE(ig.frozen());

                // 冻结：simTimeUs 保持恒定，不再随本地流逝推进。
                const std::uint64_t s1 = ig.simTimeUs();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const std::uint64_t s2 = ig.simTimeUs();
                REQUIRE(s2 == s1);
            }
        }
    }
}
