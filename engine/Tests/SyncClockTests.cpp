// 时钟同步方案.md §6 验收：IG 侧时间戳注入 / 相位展开 / simTimeUs 补偿 / 外推冻结。
// 接口已实现（IgSync）；注入式 + 系统级覆盖设计 §6 验收点。

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "engine.h"
#include "function/sync/IgSync.h"

#include <cstdint>
#include <thread>

namespace
{
    // 设计 §3/§4 注入结构由 IgSync::HostTimeStamp 提供（已实现）。
    using HostTimeStamp = IgSync::HostTimeStamp;

    // 系统测试辅助：独立端口（避免与 HostIGTests 默认端口冲突），与 HostIGTests 模式一致。
    AddressConfig makeHostLocalE2E(int base)
    {
        return AddressConfig{"127.0.0.1", base + 1, base, base + 100};
    }

    AddressConfig makeIgLocalE2E(int udpRecvPort, int base)
    {
        return AddressConfig{"127.0.0.1", base, udpRecvPort, base + 100};
    }

    AddressConfig makeHostEndpointE2E(int base)
    {
        return AddressConfig{"127.0.0.1", base + 1, base, base + 100};
    }

    SyncRoleConfig makeHostIgRoleE2E(int igUdpRecv, int base)
    {
        SyncRoleConfig role{};
        role.enableHost = true;
        role.enableIg = true;
        role.hostLocal = makeHostLocalE2E(base);
        role.igLocal = makeIgLocalE2E(igUdpRecv, base);
        role.hostEndpoint = makeHostEndpointE2E(base);
        return role;
    }

    SyncRoleConfig makeIgOnlyRoleE2E(int igUdpRecv, int base)
    {
        SyncRoleConfig role{};
        role.enableHost = false;
        role.enableIg = true;
        role.igLocal = makeIgLocalE2E(igUdpRecv, base);
        role.hostEndpoint = makeHostEndpointE2E(base);
        return role;
    }
} // namespace

SCENARIO("linked IG reports simulation time as Host base plus local elapsed",
         "[acceptance][bdd][sync][clock]")
{
    GIVEN("an IgSync that has received one Host timestamp")
    {
        IgSync ig;
        const std::uint32_t raw = 1000; // 10000 us
        const std::uint64_t receivedAtUs = 20000;
        ig.queueHostTimeStamp(HostTimeStamp{100, raw, receivedAtUs});

        WHEN("the consumer asks for simulation time later in the same frame")
        {
            const std::uint64_t nowUs = 26000; // 流逝 6000us

            THEN("simTimeUs equals Host base plus local elapsed")
            {
                // 首包：lastSimTimeUs = raw*10 = 10000us；补偿 (nowUs - receivedAtUs)
                REQUIRE(ig.simTimeUsAt(nowUs) == raw * 10 + (nowUs - receivedAtUs));
            }
        }
    }
}

SCENARIO("IG takes the last of multiple same-frame time stamps",
         "[acceptance][bdd][sync][clock]")
{
    GIVEN("an IgSync that receives two packets for the same frame")
    {
        IgSync ig;
        ig.queueHostTimeStamp(HostTimeStamp{100, 1000, 20000}); // 10000us @20000us
        ig.queueHostTimeStamp(HostTimeStamp{100, 1500, 24000}); // 15000us @24000us

        WHEN("the consumer asks for simulation time")
        {
            const std::uint64_t nowUs = 26000;

            THEN("the last of the same-frame packets is the base")
            {
                REQUIRE(ig.simTimeUsAt(nowUs) == 1500 * 10 + (nowUs - 24000));
            }
        }
    }
}

SCENARIO("IG rejects an older-frame time stamp entirely",
         "[acceptance][bdd][sync][clock]")
{
    GIVEN("an IgSync that received frame 100 then an older frame 99")
    {
        IgSync ig;
        ig.queueHostTimeStamp(HostTimeStamp{100, 1000, 20000});
        ig.queueHostTimeStamp(HostTimeStamp{99, 999, 21000});

        WHEN("the consumer asks for simulation time")
        {
            const std::uint64_t nowUs = 22000;

            THEN("the older frame does not move the base")
            {
                REQUIRE(ig.simTimeUsAt(nowUs) == 1000 * 10 + (nowUs - 20000));
            }
        }
    }
}

SCENARIO("IG accepts a skipped frame number as a fresh base",
         "[acceptance][bdd][sync][clock]")
{
    GIVEN("an IgSync that receives frame 100 then a skipped frame 102")
    {
        IgSync ig;
        ig.queueHostTimeStamp(HostTimeStamp{100, 1000, 20000});
        ig.queueHostTimeStamp(HostTimeStamp{102, 2000, 40000}); // 101 丢

        WHEN("the consumer asks for simulation time")
        {
            const std::uint64_t nowUs = 41000;

            THEN("frame 102 is accepted and becomes the base")
            {
                REQUIRE(ig.simTimeUsAt(nowUs) == 2000 * 10 + (nowUs - 40000));
            }
        }
    }
}

SCENARIO("IG extrapolates simulation time with local elapsed while no frame arrives",
         "[acceptance][bdd][sync][clock]")
{
    GIVEN("an IgSync with a large extrapolate timeout that received one frame")
    {
        IgSync ig;
        ig.setExtrapolateTimeoutUs(1000000); // 1s
        ig.queueHostTimeStamp(HostTimeStamp{100, 1000, 20000});

        WHEN("time passes with no new frame")
        {
            THEN("simTimeUs keeps increasing with local elapsed")
            {
                REQUIRE(ig.simTimeUsAt(30000) == 1000 * 10 + 10000);
                REQUIRE(ig.simTimeUsAt(35000) == 1000 * 10 + 15000);
            }
        }
    }
}

SCENARIO("IG freezes beyond extrapolate timeout and returns a constant simulation time",
         "[acceptance][bdd][sync][clock]")
{
    GIVEN("an IgSync with a 50ms timeout that received one frame")
    {
        IgSync ig;
        ig.setExtrapolateTimeoutUs(50000); // 50ms
        ig.queueHostTimeStamp(HostTimeStamp{100, 1000, 20000});

        WHEN("more than the timeout elapses without a frame")
        {
            ig.updateFreeze(80000); // nowUs - lastReceivedAtUs = 60000 > 50000 → 冻结

            THEN("the IG enters frozen state and simTimeUs stays constant")
            {
                REQUIRE(ig.frozen());
                REQUIRE(ig.simTimeUsAt(80000) == 1000 * 10);
                REQUIRE(ig.simTimeUsAt(90000) == 1000 * 10);
            }
        }
    }
}

SCENARIO("IG does not freeze within the extrapolate timeout",
         "[acceptance][bdd][sync][clock]")
{
    GIVEN("an IgSync with a 50ms timeout that received one frame")
    {
        IgSync ig;
        ig.setExtrapolateTimeoutUs(50000);
        ig.queueHostTimeStamp(HostTimeStamp{100, 1000, 20000});

        WHEN("less than the timeout elapses")
        {
            ig.updateFreeze(60000); // nowUs - lastReceivedAtUs = 40000 < 50000

            THEN("the IG stays not-frozen and keeps compensating elapsed time")
            {
                REQUIRE_FALSE(ig.frozen());
                REQUIRE(ig.simTimeUsAt(60000) == 1000 * 10 + (60000 - 20000));
            }
        }
    }
}

SCENARIO("IG jumps directly to a new base after freeze recovery",
         "[acceptance][bdd][sync][clock]")
{
    GIVEN("an IgSync frozen after its timeout")
    {
        IgSync ig;
        ig.setExtrapolateTimeoutUs(50000);
        ig.queueHostTimeStamp(HostTimeStamp{100, 1000, 20000});
        ig.updateFreeze(80000); // 触发冻结
        REQUIRE(ig.frozen());

        WHEN("a new frame arrives after the freeze")
        {
            ig.queueHostTimeStamp(HostTimeStamp{101, 9000, 90000});

            THEN("freeze clears and simTimeUs jumps to the new base")
            {
                REQUIRE_FALSE(ig.frozen());
                REQUIRE(ig.simTimeUsAt(91000) == 9000 * 10 + (91000 - 90000));
            }
        }
    }
}

SCENARIO("IG phase unwrap crosses a single 2^32 wrap without a jump",
         "[acceptance][bdd][sync][clock][wrap]")
{
    GIVEN("an IgSync whose raw time stamps are about to wrap")
    {
        IgSync ig;
        ig.queueHostTimeStamp(HostTimeStamp{100, 0xfffffff0u, 10000});

        WHEN("the next stamp wraps past the uint32 limit")
        {
            ig.queueHostTimeStamp(HostTimeStamp{101, 0x10u, 11000}); // +32 tick

            THEN("extended time increases by exactly 32 ticks, no jump")
            {
                // 首包 base = 0xfffffff0*10 us；第二包 base = (0xfffffff0+32)*10 us
                REQUIRE(ig.simTimeUsAt(11000) == (static_cast<std::uint64_t>(0xfffffff0u) + 32) * 10);
            }
        }
    }
}

SCENARIO("IG phase unwrap stays monotonic across multiple wraps",
         "[acceptance][bdd][sync][clock][wrap]")
{
    GIVEN("an IgSync receiving stamps that wrap several times")
    {
        IgSync ig;
        ig.queueHostTimeStamp(HostTimeStamp{100, 0xfffffff0u, 10000});

        WHEN("stamps keep increasing across the uint32 limit")
        {
            std::uint64_t prev = 0;
            bool first = true;
            for (std::uint32_t i = 0; i < 5; ++i)
            {
                // 每次接近满值再越过：满值 - i, 小值 + i
                ig.queueHostTimeStamp(HostTimeStamp{101 + i * 2, static_cast<std::uint32_t>(0xfffffff0u - i), 10000 + i * 100});
                ig.queueHostTimeStamp(HostTimeStamp{102 + i * 2, static_cast<std::uint32_t>(0x10u + i), 10000 + i * 100 + 50});
                const std::uint64_t cur = ig.simTimeUsAt(10000 + i * 100 + 60);
                if (!first)
                    REQUIRE(cur > prev);
                prev = cur;
                first = false;
            }
        }
    }
}

TEST_CASE("session reset keeps Host absolute tick base on reconnect", "[unit][sync][clock][session]")
{
    // 设计 §3：新会话首包 extendedTime = raw（保留 Host 绝对基准，不从 0 起）。
    IgSync ig;
    ig.queueHostTimeStamp(HostTimeStamp{1, 5000, 10000});
    REQUIRE(ig.simTimeUsAt(10000) == 5000 * 10);

    ig.queueHostTimeStamp(HostTimeStamp{2, 5010, 20000});
    REQUIRE(ig.simTimeUsAt(20000) == 5010 * 10);
}

TEST_CASE("session reset after Host restart starts from the small raw without inheriting the old base",
          "[unit][sync][clock][session]")
{
    // 设计 §3 边界：Host 崩溃重启 → TCP 重连（新会话）→ 首包 raw 为小值。
    // resetHostSession() 使相位展开状态从新基准起，不继承旧会话大值（否则 simTimeUs 猛增 ≈2^32）。
    IgSync ig;
    ig.queueHostTimeStamp(HostTimeStamp{1, 0xf0000000u, 10000}); // 旧会话大值
    ig.queueHostTimeStamp(HostTimeStamp{2, 0xf0000010u, 20000});

    ig.resetHostSession();                                  // 会话重置（连接层在 TCP 重连时调用）
    ig.queueHostTimeStamp(HostTimeStamp{1, 0x100u, 30000}); // 新会话首包小值

    // 新基准 = 0x100*10 us，不继承旧会话 0xf0000010 的值。
    REQUIRE(ig.simTimeUsAt(30000) == 0x100u * 10);
}

TEST_CASE("unit conversion from raw tick to us and ms is exact", "[unit][sync][clock][convert]")
{
    const std::uint32_t raw = 12345; // 10µs tick
    const std::uint64_t us = static_cast<std::uint64_t>(raw) * 10;
    REQUIRE(us == 123450);

    const double ms = static_cast<double>(us) / 1000.0;
    REQUIRE(ms == Catch::Approx(123.45));
}

// =============================================================================
// 系统测试：两个 Engine（A=Host+IG、B=仅 IG）真实 socket 连接，发送真实 CIGI
// 时间戳报文，验证 IG 侧仿真时间 = Host 发送时间戳 + 本地流逝补偿。
// 依赖待实现接口：IgSync::queueHostTimeStamp / simTimeUsAt / setExtrapolateTimeoutUs / frozen。
// 编译红 = 契约未满足；实现后变绿。
// 标签 [acceptance][bdd][sync][clock][e2e]。
// =============================================================================

SCENARIO("IG derives simulation time from live Host time stamps plus local elapsed",
         "[acceptance][bdd][sync][clock][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only, linked over real sockets")
    {
        constexpr int kBase = 26000;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        REQUIRE(engineA.initSync(makeHostIgRoleE2E(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRoleE2E(kBase + 3, kBase)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
        // Host 侧需 graphics 才能 tickOnFrame 走完整帧循环（含 postFrame 扇出时间戳）。
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));

        WHEN("both engines tick and Host fans out real IGCtrl time stamps")
        {
            constexpr int kTicks = 10;
            for (int i = 0; i < kTicks; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }

            THEN("IG simulation time is anchored on the Host time stamp and advances with local elapsed")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                REQUIRE(ig.igCtrlReceivedCount() > 0);
                REQUIRE(ig.lastIgCtrlFrameCntr() > 0);

                // 关键断言：基准来自 Host 时间戳（而非本地时钟从 0 起）。
                // Host 每帧 simTimeMs 递增（Engine::postFrame += 1000/60），10 帧后应达 ~166ms。
                const std::uint64_t hostBaseUs = ig.lastHostSimTimeUs();
                REQUIRE(hostBaseUs > 0);

                // 补偿后仿真时间 ≥ Host 基准（本地流逝补偿生效），且与 Host 时间戳同量级（> 1 帧）。
                const auto nowUs = [](auto now) {
                    return static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
                };
                const std::uint64_t s0 = ig.simTimeUsAt(nowUs(vsg::clock::now()));
                REQUIRE(s0 >= hostBaseUs);

                // Host 时间戳量级验证：10 帧 × ~16.67ms ≈ 166ms（> 100ms），证明基准确实来自 Host。
                REQUIRE(hostBaseUs >= 100000); // ≥ 100ms
            }
        }
    }
}

SCENARIO("two IG channels derive nearly identical simulation time from the shared Host",
         "[acceptance][bdd][sync][clock][e2e][consistency]")
{
    GIVEN("Engine A as Host+IG and two IG-only engines B and C, linked over real sockets")
    {
        constexpr int kBase = 27000;
        Engine engineA;
        Engine engineB;
        Engine engineC;
        engineA.extent = engineB.extent = engineC.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = engineC.showWindow = false;

        REQUIRE(engineA.initSync(makeHostIgRoleE2E(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRoleE2E(kBase + 3, kBase)));
        REQUIRE(engineC.initSync(makeIgOnlyRoleE2E(kBase + 5, kBase)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 3);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));

        WHEN("all engines tick and Host fans out real time stamps to both IGs")
        {
            constexpr int kTicks = 10;
            for (int i = 0; i < kTicks; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
                engineC.tickSync();
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

                // 两 IG 从同一 Host 时间戳基准出发（同一帧），仿真时间应接近；
                // 差异来自网络延迟差（LAN 内 < 一帧），远小于帧周期 16.67ms。
                REQUIRE(sB > 0);
                REQUIRE(sC > 0);
                const std::uint64_t diff = sB > sC ? sB - sC : sC - sB;
                REQUIRE(diff < 16670); // < 一帧（us），验证一致性
            }
        }
    }
}

TEST_CASE("simTimeUs uses the monotonic clock internally and is not affected by wall-clock changes",
          "[unit][sync][clock][monotonic]")
{
    // 设计 §4.2：补偿/外推用 steady_clock（vsg::clock），禁止 system_clock。
    // 无参 simTimeUs() 内部用 vsg::clock::now() 取 nowUs；改系统时间不回跳。
    IgSync ig;
    ig.queueHostTimeStamp(HostTimeStamp{100, 1000, 0}); // lastSimTimeUs = 10000us

    const std::uint64_t s1 = ig.simTimeUs(); // 无参：内部取 nowUs
    const std::uint64_t s2 = ig.simTimeUs();

    // 两次读取应 ≥ 基准，且单调不减（本地流逝补偿；steady_clock 单调）。
    REQUIRE(s1 >= 10000);
    REQUIRE(s2 >= s1);
}

SCENARIO("IG freezes when the Host stops sending time stamps over the real link",
         "[acceptance][bdd][sync][clock][e2e][freeze]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets with a short freeze timeout")
    {
        constexpr int kBase = 28000;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        REQUIRE(engineA.initSync(makeHostIgRoleE2E(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRoleE2E(kBase + 3, kBase)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));

        // 极短冻结阈值：让真实链路在几帧内就触发冻结，避免等待默认 200ms。
        engineB.synchronSystem().igSync().setExtrapolateTimeoutUs(50000); // 50ms

        WHEN("Host ticks a few frames then stops, while the IG keeps ticking")
        {
            constexpr int kHostTicks = 5;
            for (int i = 0; i < kHostTicks; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }
            REQUIRE(engineB.synchronSystem().igSync().igCtrlReceivedCount() > 0);
            REQUIRE_FALSE(engineB.synchronSystem().igSync().frozen());

            // Host 停发：只跑 IG，让流逝超过 50ms 冻结阈值。
            for (int i = 0; i < 20; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                engineB.tickSync();
            }

            THEN("the IG enters frozen state and sim time stops advancing")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                REQUIRE(ig.frozen());

                // 冻结：simTimeUs 保持恒定（不再随流逝增长）。
                const std::uint64_t s1 = ig.simTimeUs();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const std::uint64_t s2 = ig.simTimeUs();
                REQUIRE(s2 == s1);
            }
        }
    }
}

SCENARIO("Host simulation time advances with wall-clock pauses, not fixed steps",
         "[acceptance][bdd][sync][clock][e2e][real-time]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        constexpr int kBase = 29000;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        REQUIRE(engineA.initSync(makeHostIgRoleE2E(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRoleE2E(kBase + 3, kBase)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));

        WHEN("the Host pauses between frames and then sends the next time stamp")
        {
            // 预热 2 帧：让 t0 非零（方案 A 首帧 simTime=0）。
            for (int i = 0; i < 2; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }
            REQUIRE(engineB.synchronSystem().igSync().igCtrlReceivedCount() >= 2);
            const std::uint64_t t0 = engineB.synchronSystem().igSync().lastHostSimTimeUs();
            REQUIRE(t0 > 0); // 预热后基准非零

            // 卡顿 100ms 后再发一帧。
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();
            const std::uint64_t t1 = engineB.synchronSystem().igSync().lastHostSimTimeUs();

            THEN("the sim time advance follows the pause (real-time), not a fixed 16.67ms step")
            {
                REQUIRE(t1 > t0);
                const std::uint64_t advanceUs = t1 - t0;

                // 方案 B：advance ≈ 卡顿 100ms（+一帧的正常推进）。允许宽容差。
                // 方案 A（固定步进）：advance ≈ 16.67ms，远小于卡顿 → 本断言红。
                REQUIRE(advanceUs >= 80000);  // ≥ 80ms（100ms 卡顿的 80%）
                REQUIRE(advanceUs <= 200000); // ≤ 200ms（避免网络/调度抖动误报）
            }
        }
    }
}

SCENARIO("IG freezes when the Host goes offline and stops sending time stamps",
         "[acceptance][bdd][sync][clock][e2e][freeze][host-offline]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets with a short freeze timeout")
    {
        constexpr int kBase = 30000;
        Engine engineA;
        Engine engineB;
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        REQUIRE(engineA.initSync(makeHostIgRoleE2E(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeIgOnlyRoleE2E(kBase + 3, kBase)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));

        // 极短冻结阈值：真实链路几帧内触发冻结。
        engineB.synchronSystem().igSync().setExtrapolateTimeoutUs(50000); // 50ms

        WHEN("Host and IG tick normally, then the Host goes offline while the IG keeps ticking")
        {
            // 正常 tick：B 收到时间戳、不冻结。
            for (int i = 0; i < 5; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }
            REQUIRE(engineB.synchronSystem().igSync().igCtrlReceivedCount() > 0);
            REQUIRE_FALSE(engineB.synchronSystem().igSync().frozen());

            // A 退出（等价于 Host 关闭 TCP/UDP）：B 继续 tick，流逝超过冻结阈值。
            engineA.synchronSystem().shutdown();

            for (int i = 0; i < 20; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                engineB.tickSync();
            }

            THEN("the IG enters frozen state and sim time stops advancing")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                REQUIRE(ig.frozen());

                // 冻结：simTimeUs 保持恒定（不再随流逝增长）。
                const std::uint64_t s1 = ig.simTimeUs();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const std::uint64_t s2 = ig.simTimeUs();
                REQUIRE(s2 == s1);
            }
        }
    }
}
