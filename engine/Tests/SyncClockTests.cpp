// ʱ��ͬ������.md ��6 ���գ�IG ��ʱ���ע�� / ��λչ�� / simTimeUs ���� / ���ƶ��ᡣ
// �ӿ���ʵ�֣�IgSync����ע��ʽ + ϵͳ��������� ��6 ���յ㡣

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Common.h"
#include "engine.h"
#include "function/sync/IgSync.h"

#include <cstdint>
#include <thread>

using aerovista::sync::IgSync;

namespace
{
    // ��� ��3/��4 ע��ṹ�� IgSync::HostTimeStamp �ṩ����ʵ�֣���
    using HostTimeStamp = IgSync::HostTimeStamp;
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
            const std::uint64_t nowUs = 26000; // ���� 6000us

            THEN("simTimeUs equals Host base plus local elapsed")
            {
                // �װ���lastSimTimeUs = raw*10 = 10000us������ (nowUs - receivedAtUs)
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
        ig.queueHostTimeStamp(HostTimeStamp{102, 2000, 40000}); // 101 ��

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
            ig.updateFreeze(80000); // nowUs - lastReceivedAtUs = 60000 > 50000 �� ����

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
        ig.updateFreeze(80000); // ��������
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
                // �װ� base = 0xfffffff0*10 us���ڶ��� base = (0xfffffff0+32)*10 us
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
                // ÿ�νӽ���ֵ��Խ������ֵ - i, Сֵ + i
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
    // ��� ��3���»Ự�װ� extendedTime = raw������ Host ���Ի�׼������ 0 �𣩡�
    IgSync ig;
    ig.queueHostTimeStamp(HostTimeStamp{1, 5000, 10000});
    REQUIRE(ig.simTimeUsAt(10000) == 5000 * 10);

    ig.queueHostTimeStamp(HostTimeStamp{2, 5010, 20000});
    REQUIRE(ig.simTimeUsAt(20000) == 5010 * 10);
}

TEST_CASE("session reset after Host restart starts from the small raw without inheriting the old base",
          "[unit][sync][clock][session]")
{
    // ��� ��3 �߽磺Host �������� �� TCP �������»Ự���� �װ� raw ΪСֵ��
    // resetHostSession() ʹ��λչ��״̬���»�׼�𣬲��̳оɻỰ��ֵ������ simTimeUs ���� ��2^32����
    IgSync ig;
    ig.queueHostTimeStamp(HostTimeStamp{1, 0xf0000000u, 10000}); // �ɻỰ��ֵ
    ig.queueHostTimeStamp(HostTimeStamp{2, 0xf0000010u, 20000});

    ig.resetHostSession();                                  // �Ự���ã����Ӳ��� TCP ����ʱ���ã�
    ig.queueHostTimeStamp(HostTimeStamp{1, 0x100u, 30000}); // �»Ự�װ�Сֵ

    // �»�׼ = 0x100*10 us�����̳оɻỰ 0xf0000010 ��ֵ��
    REQUIRE(ig.simTimeUsAt(30000) == 0x100u * 10);
}

TEST_CASE("unit conversion from raw tick to us and ms is exact", "[unit][sync][clock][convert]")
{
    const std::uint32_t raw = 12345; // 10��s tick
    const std::uint64_t us = static_cast<std::uint64_t>(raw) * 10;
    REQUIRE(us == 123450);

    const double ms = static_cast<double>(us) / 1000.0;
    REQUIRE(ms == Catch::Approx(123.45));
}

// =============================================================================
// ϵͳ���ԣ����� Engine��A=Host+IG��B=�� IG����ʵ socket ���ӣ�������ʵ CIGI
// ʱ������ģ���֤ IG �����ʱ�� = Host ����ʱ��� + �������Ų�����
// ������ʵ�ֽӿڣ�IgSync::queueHostTimeStamp / simTimeUsAt / setExtrapolateTimeoutUs / frozen��
// ����� = ��Լδ���㣻ʵ�ֺ���̡�
// ��ǩ [acceptance][bdd][sync][clock][e2e]��
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

        REQUIRE(engineA.initSync(makeTestHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeTestIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
        // Host ���� graphics ���� tickOnFrame ������֡ѭ������ postFrame �ȳ�ʱ�������
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

                // �ؼ����ԣ���׼���� Host ʱ��������Ǳ���ʱ�Ӵ� 0 �𣩡�
                // Host ÿ֡ simTimeMs ������Engine::postFrame += 1000/60����10 ֡��Ӧ�� ~166ms��
                const std::uint64_t hostBaseUs = ig.lastHostSimTimeUs();
                REQUIRE(hostBaseUs > 0);

                // ���������ʱ�� �� Host ��׼���������Ų�����Ч�������� Host ʱ���ͬ������> 1 ֡����
                const auto nowUs = [](auto now) {
                    return static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count());
                };
                const std::uint64_t s0 = ig.simTimeUsAt(nowUs(vsg::clock::now()));
                REQUIRE(s0 >= hostBaseUs);

                // Host ʱ���������֤��10 ֡ �� ~16.67ms �� 166ms��> 100ms����֤����׼ȷʵ���� Host��
                REQUIRE(hostBaseUs >= 100000); // �� 100ms
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

        REQUIRE(engineA.initSync(makeTestHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeTestIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineC.initSync(makeTestIgOnlyRole(kBase + 5, kBase)));
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

                // �� IG ��ͬһ Host ʱ�����׼������ͬһ֡��������ʱ��Ӧ�ӽ���
                // �������������ӳٲLAN �� < һ֡����ԶС��֡���� 16.67ms��
                REQUIRE(sB > 0);
                REQUIRE(sC > 0);
                const std::uint64_t diff = sB > sC ? sB - sC : sC - sB;
                REQUIRE(diff < 16670); // < һ֡��us������֤һ����
            }
        }
    }
}

TEST_CASE("simTimeUs uses the monotonic clock internally and is not affected by wall-clock changes",
          "[unit][sync][clock][monotonic]")
{
    // ��� ��4.2������/������ steady_clock��vsg::clock������ֹ system_clock��
    // �޲� simTimeUs() �ڲ��� vsg::clock::now() ȡ nowUs����ϵͳʱ�䲻������
    IgSync ig;
    ig.queueHostTimeStamp(HostTimeStamp{100, 1000, 0}); // lastSimTimeUs = 10000us

    const std::uint64_t s1 = ig.simTimeUs(); // �޲Σ��ڲ�ȡ nowUs
    const std::uint64_t s2 = ig.simTimeUs();

    // ���ζ�ȡӦ �� ��׼���ҵ����������������Ų�����steady_clock ��������
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

        REQUIRE(engineA.initSync(makeTestHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeTestIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));

        // ���̶�����ֵ������ʵ��·�ڼ�֡�ھʹ������ᣬ����ȴ�Ĭ�� 200ms��
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

            // Host ͣ����ֻ�� IG�������ų��� 50ms ������ֵ��
            for (int i = 0; i < 20; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                engineB.tickSync();
            }

            THEN("the IG enters frozen state and sim time stops advancing")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                REQUIRE(ig.frozen());

                // ���᣺simTimeUs ���ֺ㶨��������������������
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

        REQUIRE(engineA.initSync(makeTestHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeTestIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));

        WHEN("the Host pauses between frames and then sends the next time stamp")
        {
            // Ԥ�� 2 ֡���� t0 ���㣨���� A ��֡ simTime=0����
            for (int i = 0; i < 2; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }
            REQUIRE(engineB.synchronSystem().igSync().igCtrlReceivedCount() >= 2);
            const std::uint64_t t0 = engineB.synchronSystem().igSync().lastHostSimTimeUs();
            REQUIRE(t0 > 0); // Ԥ�Ⱥ��׼����

            // ���� 100ms ���ٷ�һ֡��
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();
            const std::uint64_t t1 = engineB.synchronSystem().igSync().lastHostSimTimeUs();

            THEN("the sim time advance follows the pause (real-time), not a fixed 16.67ms step")
            {
                REQUIRE(t1 > t0);
                const std::uint64_t advanceUs = t1 - t0;

                // ���� B��advance �� ���� 100ms��+һ֡�������ƽ������������ݲ
                // ���� A���̶���������advance �� 16.67ms��ԶС�ڿ��� �� �����Ժ졣
                REQUIRE(advanceUs >= 80000);  // �� 80ms��100ms ���ٵ� 80%��
                REQUIRE(advanceUs <= 200000); // �� 200ms����������/���ȶ����󱨣�
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

        REQUIRE(engineA.initSync(makeTestHostIgRole(kBase + 1, kBase)));
        REQUIRE(engineB.initSync(makeTestIgOnlyRole(kBase + 3, kBase)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));

        // ���̶�����ֵ����ʵ��·��֡�ڴ������ᡣ
        engineB.synchronSystem().igSync().setExtrapolateTimeoutUs(50000); // 50ms

        WHEN("Host and IG tick normally, then the Host goes offline while the IG keeps ticking")
        {
            // ���� tick��B �յ�ʱ����������ᡣ
            for (int i = 0; i < 5; ++i)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }
            REQUIRE(engineB.synchronSystem().igSync().igCtrlReceivedCount() > 0);
            REQUIRE_FALSE(engineB.synchronSystem().igSync().frozen());

            // A �˳����ȼ��� Host �ر� TCP/UDP����B ���� tick�����ų���������ֵ��
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

                // ���᣺simTimeUs ���ֺ㶨��������������������
                const std::uint64_t s1 = ig.simTimeUs();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                const std::uint64_t s2 = ig.simTimeUs();
                REQUIRE(s2 == s1);
            }
        }
    }
}
