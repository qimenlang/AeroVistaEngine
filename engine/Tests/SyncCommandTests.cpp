// 状态同步设计.md §10 验收（新契约：引用式发送 + 无业务回执 + CCL 标准报文 + processor）。
// 命令面全绿：线格式契约 + E2E（TCP/UDP 收发对等）+ 分帧单测。
// §7 双 session 隔离：2 条反向用例（flushUdp 不打包 TCP 缓冲 → 收不到）+ 2 条正向（flushTcp 正常送达）。

#include <aerovista/sync/CigiIncludes.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Testutil.h"
#include "engine.h"
#include <aerovista/sync/CigiWire.h>
#include <aerovista/sync/HostSync.h>
#include <aerovista/sync/IgSync.h>

#include "CigiBaseCollDetSegDef.h"
#include "CigiBaseCollDetSegResp.h"
#include "CigiBaseCollDetVolDef.h"
#include "CigiBaseCollDetVolResp.h"
#include "CigiBaseEntityCtrl.h"
#include "CigiBaseEntityPositionCtrl.h"
#include "CigiBaseEventProcessor.h"
#include "CigiCollDetSegDefV4.h"
#include "CigiCollDetSegRespV4.h"
#include "CigiCollDetVolDefV4.h"
#include "CigiCollDetVolRespV4.h"
#include "CigiEntityCtrlV4.h"
#include "CigiEntityPositionCtrlV4.h"
#include "CigiHostSession.h"
#include "CigiIGCtrlV4.h"
#include "CigiIGMsgV4.h"
#include "CigiSymbolTextDefV4.h"
#include "CigiViewCtrlV4.h"

#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using aerovista::sync::HostSync;
using aerovista::sync::IgSync;
namespace cigi_wire = aerovista::sync::cigi_wire;

// =============================================================================
// 2. E2E：真实 socket 收发（outMsgWithIgCtrlTcp/flushTcp/addCallback）
// =============================================================================

// =============================================================================
// Host 经 TCP 下发 EntityPositionCtrlV4，核对 IG processor 解出的实体号与 LLA（状态同步设计.md §10，码 CMD-place-tcp）。
// 回环可能晚一拍，轮询 tickSync 直到收到或次数用尽。
// =============================================================================
SCENARIO("Host places an entity pose over TCP via outMsgWithIgCtrlTcp/flushTcp",
         "[acceptance][sync][cmd][e2e][CMD-place-tcp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 31000);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        subscribe<CigiEntityPositionCtrlV4>(engineA.igSync(), placeProc);

        WHEN("Host assembles EntityPositionCtrlV4 and flushes TCP")
        {
            auto& tcp = host.outMsgWithIgCtrlTcp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            tcp << place;
            host.flushTcp();
            tickIgUntil(engineA, [&] { return placeProc->received; });

            THEN("IG received the pose with matching fields")
            {
                REQUIRE(placeProc->received);
                REQUIRE(placeProc->entityId == 7);
                REQUIRE(placeProc->attachState == CigiBaseEntityPositionCtrl::Detach);
                REQUIRE(placeProc->lat == Catch::Approx(31.23));
                REQUIRE(placeProc->lon == Catch::Approx(121.47));
                REQUIRE(placeProc->alt == Catch::Approx(500.0));
            }
        }
    }
}

// =============================================================================
// Host 经 TCP 下发一条 SymbolTextDefV4，核对 IG processor 收到的文本与原文一致
// （状态同步设计.md §10，码 CMD-text-tcp）。
// 文本投递只要求一台 IG 的 tickSync 解包。
// 回环 TCP 可能晚一拍，轮询 tickSync 直到收到或次数用尽。
// =============================================================================
SCENARIO("Host sends a text command over TCP via SymbolTextDefV4",
         "[acceptance][sync][cmd][e2e][CMD-text-tcp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 31150);

        auto textProc = std::make_shared<TestTextProcessor>();
        subscribe<CigiSymbolTextDefV4>(engineA.igSync(), textProc);

        WHEN("Host assembles SymbolTextDefV4 and flushes TCP")
        {
            auto& tcp = host.outMsgWithIgCtrlTcp();
            CigiSymbolTextDefV4 cmd("place 7 121.47 31.23 500");
            tcp << cmd;
            host.flushTcp();
            tickIgUntil(engineA, [&] { return textProc->count() > 0; });

            THEN("IG received the exact text command")
            {
                REQUIRE(textProc->count() == 1);
                REQUIRE(textProc->texts().at(0) == "place 7 121.47 31.23 500");
            }
        }
    }
}

// =============================================================================
// 一次 flushTcp 扇出到两台 ready IG，两台 processor 都收到同一条位姿（状态同步设计.md §10，码 CMD-fanout-tcp）。
// 回环可能晚一拍，轮询 tickSync 直到收到或次数用尽。
// =============================================================================
SCENARIO("Host fans out a command to multiple IGs via flushTcp",
         "[acceptance][sync][cmd][e2e][CMD-fanout-tcp]")
{
    GIVEN("an independent Host and two IG-only engines linked over real sockets")
    {
        HostSync host;
        Engine engineB;
        Engine engineC;
        setupHostDoubleIgPair(host, engineB, engineC, 31300);

        auto procB = std::make_shared<TestPlaceProcessor>();
        auto procC = std::make_shared<TestPlaceProcessor>();
        subscribe<CigiEntityPositionCtrlV4>(engineB.igSync(), procB);
        subscribe<CigiEntityPositionCtrlV4>(engineC.igSync(), procC);

        WHEN("Host flushes one EntityPositionCtrlV4 to all ready IGs")
        {
            auto& tcp = host.outMsgWithIgCtrlTcp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            tcp << place;
            host.flushTcp();
            tickIgUntil(engineB, engineC, [&] { return procB->received && procC->received; });

            THEN("both IGs received the pose via registered processors")
            {
                REQUIRE(procB->received); // 全部 ready IG 都应收到
                REQUIRE(procC->received);
            }
        }
    }
}

// =============================================================================
// 3. 命令面行为 E2E（§10 验收点）
// =============================================================================

// =============================================================================
// Host 连发两条不同 SymbolTextDefV4，IG 按到达顺序收齐原文（状态同步设计.md §10，码 CMD-text-dispatch）。
// 回环可能晚一拍，轮询 tickSync 直到收到或次数用尽。
// =============================================================================
SCENARIO("IG dispatches different text commands by first token",
         "[acceptance][sync][cmd][e2e][CMD-text-dispatch]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 31200);

        auto textProc = std::make_shared<TestTextProcessor>();
        subscribe<CigiSymbolTextDefV4>(engineA.igSync(), textProc);

        WHEN("Host sends two different text commands over TCP")
        {
            CigiSymbolTextDefV4 place("place 7 121.47 31.23 500");
            CigiSymbolTextDefV4 reset("reset");
            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                tcp << place;
                host.flushTcp();
            }
            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                tcp << reset;
                host.flushTcp();
            }
            tickIgUntil(engineA, [&] { return textProc->count() >= 2; });

            THEN("IG received both commands in order with distinct tokens")
            {
                const auto texts = textProc->texts();
                REQUIRE(texts.size() == 2);
                REQUIRE(texts.at(0) == "place 7 121.47 31.23 500");
                REQUIRE(texts.at(1) == "reset");
            }
        }
    }
}

// =============================================================================
// 连续两次 flushTcp 各成一条消息，对端按发送顺序各收到一条（状态同步设计.md §10，码 CMD-fifo-send）。
// =============================================================================
SCENARIO("consecutive Host TCP flushes arrive in enqueue order",
         "[acceptance][sync][cmd][e2e][CMD-fifo-send]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 34200);

        auto textProc = std::make_shared<TestTextProcessor>();
        subscribe<CigiSymbolTextDefV4>(engineA.igSync(), textProc);

        WHEN("Host flushes two complete TCP messages in sequence")
        {
            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                CigiSymbolTextDefV4 first("first");
                tcp << first;
                host.flushTcp();
            }
            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                CigiSymbolTextDefV4 second("second");
                tcp << second;
                host.flushTcp();
            }
            tickIgUntil(engineA, [&] { return textProc->count() >= 2; });

            THEN("IG received both messages in that order after main-thread unpack")
            {
                const auto texts = textProc->texts();
                REQUIRE(texts.size() == 2);
                REQUIRE(texts.at(0) == "first");
                REQUIRE(texts.at(1) == "second");
            }
        }
    }
}

// =============================================================================
// 连续两次 flushTcp 各成一条消息，对端按发送顺序各收到一条（状态同步设计.md §10，码 CMD-fifo-send）。
// =============================================================================
SCENARIO("consecutive IG TCP flushes arrive in enqueue order",
         "[acceptance][sync][cmd][e2e][CMD-fifo-send]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 34250);

        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        subscribe<CigiIGMsgV4>(host, hostMsgProc);

        WHEN("IG flushes two complete TCP messages in sequence")
        {
            {
                auto& tcp = engineA.igSync().outMsgWithSofTcp();
                CigiIGMsgV4 a;
                a.SetMsgID(0x2001);
                a.SetMsg("first");
                tcp << a;
                engineA.igSync().flushTcp();
            }
            {
                auto& tcp = engineA.igSync().outMsgWithSofTcp();
                CigiIGMsgV4 b;
                b.SetMsgID(0x2002);
                b.SetMsg("second");
                tcp << b;
                engineA.igSync().flushTcp();
            }
            drainHostUntil(host, [&] { return hostMsgProc->count() >= 2; });

            THEN("Host received both messages in that order after drainIncoming")
            {
                REQUIRE(hostMsgProc->count() == 2);
                REQUIRE(hostMsgProc->messages().at(0).first == 0x2001);
                REQUIRE(hostMsgProc->messages().at(0).second == "first");
                REQUIRE(hostMsgProc->messages().at(1).first == 0x2002);
                REQUIRE(hostMsgProc->messages().at(1).second == "second");
            }
        }
    }
}

// =============================================================================
// 一条 TCP 消息里放两个 PacketID，IG 两个 processor 都被投递（状态同步设计.md §10，码 CMD-multi-packet）。
// 回环可能晚一拍，轮询 tickSync 直到收到或次数用尽。
// =============================================================================
SCENARIO("Host sends multiple packets in one message and IG dispatches each by PacketID",
         "[integration][sync][cmd][e2e][CMD-multi-packet]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 31400);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        auto textProc = std::make_shared<TestTextProcessor>();
        subscribe<CigiEntityPositionCtrlV4>(engineA.igSync(), placeProc);
        subscribe<CigiSymbolTextDefV4>(engineA.igSync(), textProc);

        WHEN("Host assembles two packets into one TCP message and flushes once")
        {
            auto& tcp = host.outMsgWithIgCtrlTcp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            CigiSymbolTextDefV4 cmd("reset");
            tcp << place << cmd; // 批量两个报文 = 一条消息
            host.flushTcp();
            tickIgUntil(engineA, [&] { return placeProc->received && textProc->count() > 0; });

            THEN("both processors were triggered")
            {
                REQUIRE(placeProc->received);
                REQUIRE(textProc->count() > 0);
            }
        }
    }
}

// =============================================================================
// Host 经 UDP 下发 EntityPositionCtrlV4，IG processor 收到该位姿（状态同步设计.md §10，码 CMD-pose-udp）。
// 回环可能晚一拍，轮询 tickSync 直到收到或次数用尽。
// =============================================================================
SCENARIO("Host streams real-time entity pose over UDP via outMsgWithIgCtrlUdp/flushUdp",
         "[acceptance][sync][cmd][e2e][CMD-pose-udp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 31500);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        subscribe<CigiEntityPositionCtrlV4>(engineA.igSync(), placeProc);

        WHEN("Host assembles EntityPositionCtrlV4 and flushes UDP each frame")
        {
            // outMsgWithIgCtrlUdp() 已自动前置 IGCtrl（§7.1），业务侧只 << 命令报文。
            auto& udp = host.outMsgWithIgCtrlUdp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            udp << place;
            host.flushUdp();
            tickIgUntil(engineA, [&] { return placeProc->received; });

            THEN("IG received the real-time pose via registered processor")
            {
                REQUIRE(placeProc->received);
            }
        }
    }
}

// =============================================================================
// 5. 双向命令面：IG 发送报文 → Host addCallback 订阅处理
// =============================================================================

// =============================================================================
// IG 经 TCP 上报 CigiIGMsgV4，Host drainIncoming 后 processor 收到原文（状态同步设计.md §10，码 CMD-igmsg-tcp）。
// =============================================================================
SCENARIO("IG sends a message to Host and Host processor receives it",
         "[acceptance][sync][cmd][e2e][CMD-igmsg-tcp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 31600);

        // Host 侧对等注册：处理 IG 发来的 CigiIGMsgV4（CCL 原生 IG→Host 报文，§8.1 对等）。
        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        subscribe<CigiIGMsgV4>(host, hostMsgProc);

        WHEN("IG assembles CigiIGMsgV4 and flushes TCP")
        {
            auto& tcp = engineA.igSync().outMsgWithSofTcp();
            CigiIGMsgV4 status;
            status.SetMsgID(0x1001);
            status.SetMsg("status ok");
            tcp << status;
            engineA.igSync().flushTcp();

            // Host 收包为 push 模式：等待 peer 线程收包分帧入队后，主线程 drain 解包。
            drainHostUntil(host, [&] { return hostMsgProc->count() > 0; });

            THEN("Host received the IGMsg via registered processor")
            {
                REQUIRE(hostMsgProc->count() == 1);
                REQUIRE(hostMsgProc->messages().at(0).first == 0x1001);
                REQUIRE(hostMsgProc->messages().at(0).second == "status ok");
            }
        }
    }
}

// =============================================================================
// Host 经 TCP 下发 CollDetSegDefV4，同一台 IG 回 CollDetSegRespV4，Host processor 收到（状态同步设计.md §10，码 CMD-colldet-seg）。
// =============================================================================
SCENARIO("Host sends CollDetSegDef and IG replies CollDetSegResp over TCP",
         "[acceptance][sync][cmd][e2e][CMD-colldet-seg]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 31700);

        // 双向注册：IG 处理 Host 发来的 CollDetSegDefV4；Host 处理 IG 回发的 CollDetSegRespV4。
        auto igDefProc = std::make_shared<TestCollDetSegDefProcessor>();
        subscribe<CigiCollDetSegDefV4>(engineA.igSync(), igDefProc);
        auto hostRespProc = std::make_shared<TestCollDetSegRespProcessor>();
        subscribe<CigiCollDetSegRespV4>(host, hostRespProc);

        WHEN("Host sends CollDetSegDefV4 over TCP, IG processes and replies CollDetSegRespV4")
        {
            // Host → IG：碰撞检测段定义。
            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                CigiCollDetSegDefV4 def;
                def.SetEntityID(7);
                def.SetSegmentEn(true);
                def.SetMask(0x00FF00FFu);
                tcp << def;
                host.flushTcp();
            }

            // IG 主线程解包（drainIncoming）并确认收到。
            tickIgUntil(engineA, [&] { return igDefProc->got; });
            REQUIRE(igDefProc->got);
            REQUIRE(igDefProc->entityId == 7);
            REQUIRE(igDefProc->segmentEn);
            REQUIRE(igDefProc->mask == 0x00FF00FFu);

            // IG → Host：碰撞检测段响应。
            {
                auto& tcp = engineA.igSync().outMsgWithSofTcp();
                CigiCollDetSegRespV4 resp;
                resp.SetEntityID(igDefProc->entityId);
                resp.SetMaterial(0xABC);
                tcp << resp;
                engineA.igSync().flushTcp();
            }

            // Host push 模式：等待 peer 线程收包入队后，主线程 drain 解包。
            drainHostUntil(host, [&] { return hostRespProc->got; });

            THEN("Host received the CollDetSegResp via registered processor")
            {
                REQUIRE(hostRespProc->got);
                REQUIRE(hostRespProc->entityId == 7);
                REQUIRE(hostRespProc->material == 0xABC);
            }
        }
    }
}

// =============================================================================
// IG 经 UDP 上报 CigiIGMsgV4，Host processor 收到原文（状态同步设计.md §10，码 CMD-igmsg-udp）。
// =============================================================================
SCENARIO("IG sends a UDP message and Host processor receives it",
         "[acceptance][sync][cmd][e2e][CMD-igmsg-udp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 31800);

        // Host 对等注册：处理 IG 经 UDP 发来的 CigiIGMsgV4（§8.1 对等，收发均支持 TCP/UDP）。
        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        subscribe<CigiIGMsgV4>(host, hostMsgProc);

        WHEN("IG assembles CigiIGMsgV4 and flushes UDP")
        {
            auto& udp = engineA.igSync().outMsgWithSofUdp();
            CigiIGMsgV4 status;
            status.SetMsgID(0x2001);
            status.SetMsg("udp status ok");
            udp << status;
            engineA.igSync().flushUdp();

            // Host push 模式：等待 I/O 线程收包入队（UDP 非阻塞 1ms 轮询）后，主线程 drain 解包。
            drainHostUntil(host, [&] { return hostMsgProc->count() > 0; });

            THEN("Host received the UDP IGMsg via registered processor")
            {
                REQUIRE(hostMsgProc->count() == 1);
                REQUIRE(hostMsgProc->messages().at(0).first == 0x2001);
                REQUIRE(hostMsgProc->messages().at(0).second == "udp status ok");
            }
        }
    }
}

// =============================================================================
// 6. 碰撞检测体积对（红测已转绿）：Host 发 CollDetVolDefV4 → IG 回 CollDetVolRespV4
//    经订阅 addCallback<CigiCollDetVolDefV4>() / addCallback<CigiCollDetVolRespV4>() 投递断言。
// =============================================================================

// =============================================================================
// Host 经 TCP 下发 CollDetVolDefV4，同一台 IG 回 CollDetVolRespV4，Host 订阅收到（状态同步设计.md §10，码 CMD-colldet-vol）。
// =============================================================================
SCENARIO("Host sends CollDetVolDef and IG replies CollDetVolResp over TCP",
         "[acceptance][sync][cmd][e2e][CMD-colldet-vol]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 31900);

        WHEN("Host sends CollDetVolDefV4 over TCP, IG processes and replies CollDetVolRespV4")
        {
            std::optional<CigiCollDetVolDefV4> igDef;
            std::optional<CigiCollDetVolRespV4> hostResp;

            // 订阅：捕获时投递（IG 侧）。
            engineA.igSync().addCallback<CigiCollDetVolDefV4>(
                [&](const CigiCollDetVolDefV4& def) { igDef = def; });

            // Host → IG：碰撞检测体积定义（首版默认值填充）。
            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                CigiCollDetVolDefV4 def;
                def.SetEntityID(7);
                def.SetVolID(3);
                def.SetVolEn(true);
                tcp << def;
                host.flushTcp();
            }

            // IG 主线程解包（drainIncoming）并经订阅投递。
            tickIgUntil(engineA, [&] { return igDef.has_value(); });
            REQUIRE(igDef.has_value());
            REQUIRE(igDef->GetEntityID() == 7);
            REQUIRE(igDef->GetVolID() == 3);
            REQUIRE(igDef->GetVolEn());

            // 订阅：捕获时投递（Host 侧）。
            host.addCallback<CigiCollDetVolRespV4>(
                [&](const CigiCollDetVolRespV4& resp) { hostResp = resp; });

            // IG → Host：碰撞检测体积响应（首版默认值填充）。
            {
                auto& tcp = engineA.igSync().outMsgWithSofTcp();
                CigiCollDetVolRespV4 resp;
                resp.SetEntityID(static_cast<std::uint16_t>(igDef->GetEntityID()));
                resp.SetCollType(CigiBaseCollDetVolResp::Entity);
                tcp << resp;
                engineA.igSync().flushTcp();
            }

            // Host push 模式：等待 peer 线程收包入队后，主线程 drain 解包，经订阅投递。
            drainHostUntil(host, [&] { return hostResp.has_value(); });

            THEN("Host received the CollDetVolResp via subscription")
            {
                REQUIRE(hostResp.has_value());
                REQUIRE(hostResp->GetEntityID() == 7);
                REQUIRE(hostResp->GetCollType() == CigiBaseCollDetVolResp::Entity);
            }
        }
    }
}

// =============================================================================
// 7. 双 session 隔离（状态同步设计初版.md §5.1/§7.1/§8.1 验收）：
//    outMsgWithIgCtrlTcp/flushTcp 只操作 _tcpSession、outMsgWithIgCtrlUdp/flushUdp 只操作 _udpSession——
//    命令面内容被 flushUdp 误发时对端收不到。双 session 隔离已实现，反向用例为回归绿测。
// =============================================================================

// =============================================================================
// Host 把内容写入 TCP 出站缓冲后调用 flushUdp，IG 收不到该报文（状态同步设计.md §10，码 CMD-host-tcp-not-udp）。
// 反向用例必须等满窗口：提前结束会把「还没到」当成「不会到」。
// =============================================================================
SCENARIO("Host TCP-filled message is not sent via flushUdp",
         "[acceptance][sync][cmd][e2e][negative][CMD-host-tcp-not-udp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 32000);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        subscribe<CigiEntityPositionCtrlV4>(engineA.igSync(), placeProc);

        WHEN("Host fills a TCP outgoing message but flushes UDP")
        {
            auto& tcp = host.outMsgWithIgCtrlTcp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            tcp << place;
            host.flushUdp(); // 误用：TCP 缓冲走 UDP flush

            // 给对端留足解包机会：若误发，IG 会收到并触发 processor（单 session 下即红）；
            // 双 session 后 flushUdp 打包空的 _udpSession（无内容）→ 不发 → IG 无报文可解。
            for (int i = 0; i < 5; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                engineA.tickSync();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            engineA.tickSync();

            THEN("IG received nothing (flushUdp ignores the TCP session buffer)")
            {
                REQUIRE_FALSE(placeProc->received);
            }
        }
    }
}

// =============================================================================
// Host 把同一条 TCP 出站缓冲经 flushTcp 发出，IG processor 收到位姿（状态同步设计.md §10，码 CMD-host-tcp-ok）。
// 回环可能晚一拍，轮询 tickSync 直到收到或次数用尽。
// =============================================================================
SCENARIO("Host TCP-filled message is delivered via flushTcp",
         "[acceptance][sync][cmd][e2e][CMD-host-tcp-ok]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 32050);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        subscribe<CigiEntityPositionCtrlV4>(engineA.igSync(), placeProc);

        WHEN("Host fills a TCP outgoing message and flushes TCP")
        {
            auto& tcp = host.outMsgWithIgCtrlTcp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            tcp << place;
            host.flushTcp();
            tickIgUntil(engineA, [&] { return placeProc->received; });

            THEN("IG received the pose with matching fields")
            {
                REQUIRE(placeProc->received);
                REQUIRE(placeProc->entityId == 7);
                REQUIRE(placeProc->attachState == CigiBaseEntityPositionCtrl::Detach);
                REQUIRE(placeProc->lat == Catch::Approx(31.23));
                REQUIRE(placeProc->lon == Catch::Approx(121.47));
                REQUIRE(placeProc->alt == Catch::Approx(500.0));
            }
        }
    }
}

// =============================================================================
// IG 把内容写入 TCP 出站缓冲后调用 flushUdp，Host 收不到该报文（状态同步设计.md §10，码 CMD-ig-tcp-not-udp）。
// =============================================================================
SCENARIO("IG TCP-filled message is not sent via flushUdp",
         "[acceptance][sync][cmd][e2e][negative][CMD-ig-tcp-not-udp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 32100);

        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        subscribe<CigiIGMsgV4>(host, hostMsgProc);

        WHEN("IG fills a TCP outgoing message but flushes UDP")
        {
            auto& tcp = engineA.igSync().outMsgWithSofTcp();
            CigiIGMsgV4 status;
            status.SetMsgID(0x1001);
            status.SetMsg("status ok");
            tcp << status;
            engineA.igSync().flushUdp(); // 误用：TCP 缓冲走 UDP flush

            // 若误发，Host 的 UDP I/O 线程会收到 → drain 解包触发 processor（单 session 下即红）；
            // 双 session 后 flushUdp 打包空的 _udpSession（无内容）→ 不发 → Host 无报文可解。
            for (int i = 0; i < 5; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                host.drainIncoming();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            host.drainIncoming();

            THEN("Host received nothing (flushUdp ignores the TCP session buffer)")
            {
                REQUIRE(hostMsgProc->count() == 0);
            }
        }
    }
}

// =============================================================================
// IG 把同一条 TCP 出站缓冲经 flushTcp 发出，Host processor 收到 IGMsg（状态同步设计.md §10，码 CMD-ig-tcp-ok）。
// =============================================================================
SCENARIO("IG TCP-filled message is delivered via flushTcp",
         "[acceptance][sync][cmd][e2e][CMD-ig-tcp-ok]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 32150);

        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        subscribe<CigiIGMsgV4>(host, hostMsgProc);

        WHEN("IG fills a TCP outgoing message and flushes TCP")
        {
            auto& tcp = engineA.igSync().outMsgWithSofTcp();
            CigiIGMsgV4 status;
            status.SetMsgID(0x1001);
            status.SetMsg("status ok");
            tcp << status;
            engineA.igSync().flushTcp();

            drainHostUntil(host, [&] { return hostMsgProc->count() > 0; });

            THEN("Host received the IGMsg via registered processor")
            {
                REQUIRE(hostMsgProc->count() == 1);
                REQUIRE(hostMsgProc->messages().at(0).first == 0x1001);
                REQUIRE(hostMsgProc->messages().at(0).second == "status ok");
            }
        }
    }
}

// =============================================================================
// 8. IGCtrl 自动填充（状态同步设计初版.md §7.1 / §10 验收）：
//    outMsgWithIgCtrlUdp() 自动前置 IGCtrl（帧号=数据面、TimeStamp=自计时、TimeStampValid=true）；
//    outMsgWithIgCtrlTcp() 自动前置 IGCtrl（帧号=命令面、TimeStampValid=false、帧号连续）。
// =============================================================================

// =============================================================================
// Host 连发两条 UDP 数据面帧，IG 收到的首包 IGCtrl 时间戳有效且帧号递增（状态同步设计.md §10，码 CIGI-udp-igctrl-ts）。
// UDP 可丢一条。固定多拍 tickSync，不断言两条都必须到达。
// =============================================================================
SCENARIO("Host UDP frames carry valid IGCtrl first packet with timestamp",
         "[acceptance][sync][cmd][e2e][CIGI-udp-igctrl-ts]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 32200);

        auto igCtrlCapture = std::make_shared<TestIgCtrlCapture>();
        subscribe<CigiIGCtrlV4>(engineA.igSync(), igCtrlCapture);

        WHEN("Host flushes two UDP data-plane frames")
        {
            // outMsgWithIgCtrlUdp() 自动前置 IGCtrl（帧号=数据面、TimeStamp=自计时、TimeStampValid=true）。
            for (int i = 0; i < 2; ++i)
                hostSendFrame(host);
            for (int i = 0; i < 10; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                engineA.tickSync();
            }

            THEN("IG received IGCtrl first packets with valid timestamp and continuous data frame counters")
            {
                // 业务 processor 双注册，UDP 数据面 IGCtrl 触发捕获。
                REQUIRE(igCtrlCapture->frameCntrs.size() >= 1);
                REQUIRE(igCtrlCapture->timeStampValids.size() == igCtrlCapture->frameCntrs.size());
                for (const bool valid : igCtrlCapture->timeStampValids)
                    REQUIRE(valid); // 数据面 IGCtrl TimeStampValid 必须为 true
                // 帧号连续：UDP 每帧 outMsgWithIgCtrlUdp 递增 1。
                for (std::size_t i = 1; i < igCtrlCapture->frameCntrs.size(); ++i)
                    REQUIRE(igCtrlCapture->frameCntrs[i] > igCtrlCapture->frameCntrs[i - 1]);
                // 时间戳有效：IG 相位展开后 Host 模拟时间 > 0（时钟同步方案.md §3）。
                REQUIRE(engineA.igSync().lastHostSimTimeUs() > 0);
            }
        }
    }
}

// =============================================================================
// Host 连发两条 TCP 命令面消息，IG 收到的 IGCtrl 时间戳无效且帧号差 1（状态同步设计.md §10，码 CIGI-tcp-igctrl-cmd）。
// =============================================================================
SCENARIO("Host TCP messages carry IGCtrl first packet with invalid timestamp and continuous frame counter",
         "[acceptance][sync][cmd][e2e][CIGI-tcp-igctrl-cmd]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 32250);

        auto igCtrlCapture = std::make_shared<TestIgCtrlCapture>();
        subscribe<CigiIGCtrlV4>(engineA.igSync(), igCtrlCapture);

        WHEN("Host flushes two TCP command-plane messages")
        {
            for (int i = 0; i < 2; ++i)
            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                CigiSymbolTextDefV4 cmd("reset");
                tcp << cmd;
                host.flushTcp();
            }
            // 只 tick IG 侧（不 tick Host 端）：避免 Host 数据面帧（outMsgWithIgCtrlUdp 的 IGCtrl）混入捕获。
            for (int i = 0; i < 5; ++i)
                engineA.tickSync();

            THEN("IG received IGCtrl first packets with invalid timestamp and continuous command frame counters")
            {
                // 命令面 IGCtrl（TCP）经 _tcpSession 解包，业务 processor 双注册故触发捕获。
                // 未 tick Host 帧循环 → 无数据面帧，所有捕获均来自 TCP 命令面。
                REQUIRE(igCtrlCapture->frameCntrs.size() == 2);
                REQUIRE(igCtrlCapture->timeStampValids.size() == 2);
                for (const bool valid : igCtrlCapture->timeStampValids)
                    REQUIRE_FALSE(valid); // 命令面 IGCtrl TimeStampValid 必须为 false
                // 帧号连续：命令面每条 outMsgWithIgCtrlTcp 帧号差 1。
                REQUIRE(igCtrlCapture->frameCntrs[1] == igCtrlCapture->frameCntrs[0] + 1);
                // 命令面 IGCtrl 不携带有效时间戳：不影响 IG 时钟（相位展开基准不被污染）。
            }
        }
    }
}

// =============================================================================
// 9. 帧头去重（状态同步设计初版.md §7.1）：
//    outMsgWithIgCtrlTcp/beginWithIgCtrlUdp（IG：beginWithSof/beginWithSofUdp）单次 flush 周期内
//    多次调用填充报文，帧头只填一次——接收端应恰好收到一个 IGCtrl（IG）或 SOF（Host）。
// =============================================================================

// =============================================================================
// 同一次 flush 内多次 outMsgWithIgCtrlTcp，IG 只收到一个 IGCtrl 和全部文本（状态同步设计.md §10，码 CMD-host-fill-tcp）。
// =============================================================================
SCENARIO("Host can fill multiple packets in one message via repeated outMsgWithIgCtrlTcp",
         "[acceptance][sync][cmd][e2e][CMD-host-fill-tcp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 32300);

        auto igCtrlCapture = std::make_shared<TestIgCtrlCapture>();
        subscribe<CigiIGCtrlV4>(engineA.igSync(), igCtrlCapture);
        auto textProc = std::make_shared<TestTextProcessor>();
        subscribe<CigiSymbolTextDefV4>(engineA.igSync(), textProc);

        WHEN("Host calls outMsgWithIgCtrlTcp multiple times filling three packets, then flushes once")
        {
            // 三次调用填充三条报文，但只 flush 一次 → 应是一条消息一个 IGCtrl。
            auto& tcp = host.outMsgWithIgCtrlTcp();
            CigiSymbolTextDefV4 a("reset");
            tcp << a;
            auto& tcp2 = host.outMsgWithIgCtrlTcp();
            CigiSymbolTextDefV4 b("reset");
            tcp2 << b;
            auto& tcp3 = host.outMsgWithIgCtrlTcp();
            CigiSymbolTextDefV4 c("reset");
            tcp3 << c;
            host.flushTcp();

            for (int i = 0; i < 5; ++i)
                engineA.tickSync();

            THEN("IG received exactly one IGCtrl header and all three text packets")
            {
                // 去重后只有 1 个 IGCtrl（否则多次 begin 会填多个帧头、分帧器切成多条消息）。
                REQUIRE(igCtrlCapture->frameCntrs.size() == 1);
                REQUIRE(igCtrlCapture->timeStampValids.size() == 1);
                REQUIRE_FALSE(igCtrlCapture->timeStampValids[0]); // 命令面
                REQUIRE(textProc->count() == 3);
            }
        }
    }
}

// =============================================================================
// 同一次 flush 内多次 outMsgWithIgCtrlUdp，IG 只收到一个 IGCtrl（状态同步设计.md §10，码 CIGI-udp-one-igctrl）。
// =============================================================================
SCENARIO("Host UDP message carries exactly one IGCtrl across repeated outMsgWithIgCtrlUdp",
         "[acceptance][sync][cmd][e2e][CIGI-udp-one-igctrl]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 32350);

        auto igCtrlCapture = std::make_shared<TestIgCtrlCapture>();
        subscribe<CigiIGCtrlV4>(engineA.igSync(), igCtrlCapture);

        WHEN("Host calls outMsgWithIgCtrlUdp multiple times filling packets, then flushes once")
        {
            // 同一消息内多次 begin 追加多个数据包（如实时位姿 + 眼点 + 额外报文），只 flush 一次。
            for (int i = 0; i < 3; ++i)
            {
                auto& udp = host.outMsgWithIgCtrlUdp();
                CigiEntityPositionCtrlV4 place;
                place.SetEntityID(7);
                place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
                place.SetLat(31.23);
                place.SetLon(121.47);
                place.SetAlt(500.0);
                udp << place;
            }
            host.flushUdp();
            // 只 tick IG 侧（不 tick Host 端）：避免 Host 数据面帧额外混入。
            for (int i = 0; i < 5; ++i)
                engineA.tickSync();

            THEN("IG received exactly one IGCtrl header in the UDP datagram")
            {
                // 三次 begin 只填一个帧头（去重），IG 侧每 UDP 数据报恰好一个 IGCtrl。
                REQUIRE(igCtrlCapture->frameCntrs.size() == 1);
                REQUIRE(igCtrlCapture->timeStampValids.size() == 1);
                REQUIRE(igCtrlCapture->timeStampValids[0]); // 数据面
            }
        }
    }
}

// =============================================================================
// 同一次 flush 内多次 outMsgWithSofTcp，Host 收到全部 IGMsg 且 SOF 只有一个（状态同步设计.md §10，码 CMD-sof-dedup）。
// =============================================================================
SCENARIO("IG can fill multiple packets in one message via repeated outMsgWithSofTcp",
         "[acceptance][sync][cmd][e2e][CMD-sof-dedup]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 32400);

        // Host 捕获 SOF：验证 IG 出站去重——一条消息恰好一个 SOF。
        auto hostSofCapture = std::make_shared<TestIgCtrlCapture>(); // 复用：捕获 SOF 的 FrameCntr
        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        subscribe<CigiIGMsgV4>(host, hostMsgProc);
        // Host 侧基础设施 SofCaptureProc 已注册（双 session）——用 sofReceivedCount 验证只收到 1 条 SOF。

        WHEN("IG calls outMsgWithSofTcp multiple times filling three messages, then flushes once")
        {
            auto& tcp = engineA.igSync().outMsgWithSofTcp();
            CigiIGMsgV4 a;
            a.SetMsgID(0x1001);
            a.SetMsg("a");
            tcp << a;
            auto& tcp2 = engineA.igSync().outMsgWithSofTcp();
            CigiIGMsgV4 b;
            b.SetMsgID(0x1002);
            b.SetMsg("b");
            tcp2 << b;
            auto& tcp3 = engineA.igSync().outMsgWithSofTcp();
            CigiIGMsgV4 c;
            c.SetMsgID(0x1003);
            c.SetMsg("c");
            tcp3 << c;
            engineA.igSync().flushTcp();

            drainHostUntil(host, [&] { return hostMsgProc->count() >= 3; });

            THEN("Host received all three IGMsgs with exactly one SOF header")
            {
                host.drainIncoming();
                REQUIRE(hostMsgProc->count() == 3);
                // IG 出站去重后只有 1 个 SOF：三条报文在一条消息内，分帧器切出一条消息 → Host 收 1 个 SOF。
                REQUIRE(host.sofReceivedCount() == 1);
            }
        }
    }
}

// =============================================================================
// 10. 全 9 类报文支持：通用捕获（PacketCaptureProc + addCallback<PacketT> 纯订阅投递）
//     按发送源（IgSync/HostSync）与链路（UDP 持续 / TCP 一次性）注册（cigi梳理.md 链路矩阵）。
//     各取一个代表性报文验证：TCP 一次性（EntityCtrl）、UDP 持续（ViewCtrl）、IG→Host 响应（IGMsg）。
//     数据交付为纯订阅（拉取 takeReceived / CaptureProcBase 已删）。
// =============================================================================

// =============================================================================
// IG 订阅 Host 经 TCP 下发的 EntityCtrl，sink 收到这一条（状态同步设计.md §10，码 CMD-sub-entityctrl-tcp）。
// =============================================================================
SCENARIO("IG subscribes a one-shot Host→IG EntityCtrl over TCP",
         "[acceptance][sync][cmd][e2e][all-packets][CMD-sub-entityctrl-tcp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 33000);

        WHEN("Host sends CigiEntityCtrlV4 over TCP (one-shot)")
        {
            int sinkCount = 0;
            CigiEntityCtrlV4 sinkValue;
            engineA.igSync().addCallback<CigiEntityCtrlV4>(
                [&](const CigiEntityCtrlV4& ent) {
                    ++sinkCount;
                    sinkValue = ent;
                });

            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                CigiEntityCtrlV4 ent;
                ent.SetEntityID(7);
                ent.SetEntityType(2);
                tcp << ent;
                host.flushTcp();
            }
            engineA.tickSync();

            THEN("IG sink receives the EntityCtrl synchronously")
            {
                REQUIRE(sinkCount == 1);
                REQUIRE(sinkValue.GetEntityID() == 7);
                REQUIRE(sinkValue.GetEntityType() == 2);
            }
        }
    }
}

// 现有「一次 TCP 消息多包」只验过 Position+SymbolText / 多条同类 EntityCtrl；
// viewhost Apply 两族都脏、以及手动 Host 同一次 flush 会把 Position 与 EntityCtrl
// 打进同一条 TCP 消息。此处钉 engine 侧 addCallback 两条订阅都要被调用。
// =============================================================================
// 一次 Host TCP flush 同时带上 PositionCtrl 与 EntityCtrl，两个 IG 回调都被调用（状态同步设计.md §10，码 CMD-dual-callback-tcp）。
// =============================================================================
SCENARIO("IG callbacks fire for EntityPositionCtrl and EntityCtrl in one Host TCP flush",
         "[acceptance][sync][cmd][e2e][mixed-tcp][CMD-dual-callback-tcp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 34100);

        int poseCount = 0;
        int ctrlCount = 0;
        CigiEntityPositionCtrlV4 poseValue;
        CigiEntityCtrlV4 ctrlValue;
        engineA.igSync().addCallback<CigiEntityPositionCtrlV4>(
            [&](const CigiEntityPositionCtrlV4& pose) {
                ++poseCount;
                poseValue = pose;
            });
        engineA.igSync().addCallback<CigiEntityCtrlV4>([&](const CigiEntityCtrlV4& ent) {
            ++ctrlCount;
            ctrlValue = ent;
        });

        auto tickUntilBoth = [&]() {
            tickIgUntil(engineA, [&] { return poseCount > 0 && ctrlCount > 0; });
        };

        WHEN("Host fills EntityPositionCtrl then EntityCtrl and flushes TCP once")
        {
            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                CigiEntityPositionCtrlV4 pose;
                pose.SetEntityID(7);
                pose.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
                pose.SetLat(31.23);
                pose.SetLon(121.47);
                pose.SetAlt(500.0);
                tcp << pose;
                CigiEntityCtrlV4 ent;
                ent.SetEntityID(7);
                ent.SetEntityType(2);
                ent.SetEntityState(CigiBaseEntityCtrl::Active);
                tcp << ent;
                host.flushTcp();
            }
            tickUntilBoth();

            THEN("both IG sinks receive their packets")
            {
                REQUIRE(poseCount == 1);
                REQUIRE(poseValue.GetEntityID() == 7);
                REQUIRE(poseValue.GetLat() == Catch::Approx(31.23));
                REQUIRE(ctrlCount == 1);
                REQUIRE(ctrlValue.GetEntityID() == 7);
                REQUIRE(ctrlValue.GetEntityType() == 2);
            }
        }

        WHEN("Host fills EntityCtrl then EntityPositionCtrl and flushes TCP once")
        {
            poseCount = 0;
            ctrlCount = 0;

            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                CigiEntityCtrlV4 ent;
                ent.SetEntityID(7);
                ent.SetEntityType(3);
                ent.SetEntityState(CigiBaseEntityCtrl::Active);
                CigiEntityPositionCtrlV4 pose;
                pose.SetEntityID(7);
                pose.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
                pose.SetLat(39.9);
                pose.SetLon(116.4);
                pose.SetAlt(100.0);
                tcp << ent << pose;
                host.flushTcp();
            }
            tickUntilBoth();

            THEN("both IG sinks still receive their packets")
            {
                REQUIRE(ctrlCount == 1);
                REQUIRE(ctrlValue.GetEntityID() == 7);
                REQUIRE(ctrlValue.GetEntityType() == 3);
                REQUIRE(poseCount == 1);
                REQUIRE(poseValue.GetEntityID() == 7);
                REQUIRE(poseValue.GetLat() == Catch::Approx(39.9));
            }
        }
    }
}

// =============================================================================
// Host 按帧经 UDP 下发 ViewCtrl，IG sink 在多数帧上收到（状态同步设计.md §10，码 CMD-sub-viewctrl-udp）。
// =============================================================================
SCENARIO("IG subscribes a per-frame Host→IG ViewCtrl over UDP",
         "[acceptance][sync][cmd][e2e][all-packets][CMD-sub-viewctrl-udp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 33100);

        WHEN("Host streams CigiViewCtrlV4 over UDP each frame")
        {
            int sinkCount = 0;
            CigiViewCtrlV4 sinkValue;
            engineA.igSync().addCallback<CigiViewCtrlV4>(
                [&](const CigiViewCtrlV4& view) {
                    ++sinkCount;
                    sinkValue = view;
                });

            // UDP 数据面可丢：合同是周期覆盖，不是单报必达（对齐 HostIGTests slack=3 / 10 帧）。
            constexpr int kFrames = 10;
            constexpr int kSlack = 3;
            for (int i = 0; i < kFrames; ++i)
            {
                auto& udp = host.outMsgWithIgCtrlUdp();
                CigiViewCtrlV4 view;
                view.SetViewID(1);
                view.SetYaw(30.0f);
                view.SetPitch(10.0f);
                udp << view;
                host.flushUdp();
                engineA.tickSync();
            }

            THEN("IG sink receives ViewCtrl on most frames")
            {
                REQUIRE(sinkCount >= kFrames - kSlack);
                REQUIRE(sinkCount <= kFrames);
                REQUIRE(sinkValue.GetViewID() == 1);
                REQUIRE(sinkValue.GetYaw() == Catch::Approx(30.0f));
            }
        }
    }
}

// =============================================================================
// Host 订阅 IG 经 TCP 上报的 CigiIGMsgV4，sink 收到这一条（状态同步设计.md §10，码 CMD-host-sub-igmsg）。
// =============================================================================
SCENARIO("Host subscribes an IG→Host CigiIGMsgV4 over TCP",
         "[acceptance][sync][cmd][e2e][all-packets][CMD-host-sub-igmsg]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 33200);

        WHEN("IG sends CigiIGMsgV4 over TCP (one-shot, IG→Host)")
        {
            int sinkCount = 0;
            CigiIGMsgV4 sinkValue;
            host.addCallback<CigiIGMsgV4>([&](const CigiIGMsgV4& msg) {
                ++sinkCount;
                sinkValue = msg;
            });

            {
                auto& tcp = engineA.igSync().outMsgWithSofTcp();
                CigiIGMsgV4 status;
                status.SetMsgID(0x3001);
                status.SetMsg("all-packets ok");
                tcp << status;
                engineA.igSync().flushTcp();
            }

            // Host push 模式：等待 peer 线程收包入队后，主线程 drain 解包，经订阅投递。
            drainHostUntil(host, [&] { return sinkCount > 0; });

            THEN("Host sink receives the IGMsg synchronously")
            {
                REQUIRE(sinkCount == 1);
                REQUIRE(sinkValue.GetMsgID() == 0x3001);
            }
        }
    }
}

// HostSync 侧订阅：IG→Host 报文的到达通知。
// =============================================================================
// HostSync addCallback 把 IG 经 TCP 上报的 IGMsg 送到 sink（状态同步设计.md §10，码 CMD-addcallback-sink）。
// =============================================================================
SCENARIO("HostSync addCallback delivers an IG→Host packet to the sink",
         "[acceptance][sync][cmd][e2e][all-packets][CMD-addcallback-sink]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 33500);

        WHEN("IG sends CigiIGMsgV4 over TCP with a subscribed Host sink")
        {
            int sinkCount = 0;
            std::uint16_t sinkMsgId = 0;
            host.addCallback<CigiIGMsgV4>([&](const CigiIGMsgV4& msg) {
                ++sinkCount;
                sinkMsgId = msg.GetMsgID();
            });

            {
                auto& tcp = engineA.igSync().outMsgWithSofTcp();
                CigiIGMsgV4 status;
                status.SetMsgID(0x4001);
                status.SetMsg("subscribe ok");
                tcp << status;
                engineA.igSync().flushTcp();
            }

            // Host push 模式：drainIncoming 内同步投递回调。
            drainHostUntil(host, [&] { return sinkCount > 0; });

            THEN("the Host sink is called with the IGMsg value")
            {
                REQUIRE(sinkCount == 1);
                REQUIRE(sinkMsgId == 0x4001);
            }
        }
    }
}

// =============================================================================
// 11. 命令实体位姿：Host 下发 EntityPositionCtrlV4（EntityID≠0）→ IG engine
//     订阅 → onEntityPose → findEntity + setPoseLla 更新位姿与 transform 矩阵。
//     ownship 眼点（EntityID==0）与命令实体（EntityID≠0）同 PacketID（EntityPositionCtrlV4），
//     UDP 链路（眼点）与 TCP 链路（命令实体）各注册通用捕获，经多播
//     addCallback<CigiEntityPositionCtrlV4> 同回调按 EntityID 分流（§4.1 / cigi梳理.md 链路矩阵）。
//     同步层只支持 LLA：命令实体摆放恒 Detach+LLA（见下方 Ellipsoid 用例）。
// =============================================================================

// Ellipsoid 正向摆放回归保护（实体与运动控制设计.md §4.2 / lla位姿传输设计.md §2.1）。
// 注意：本用例**不**验证「坐标系由场景判据决定」的新语义——那是反向用例（拒收）的职责
// （Host 填 Attach/XYZ 但 IG 是椭球场景 → 拒收 + entityPoseRejectedByFrameMismatch +1，暂缓实现）。
// 这里的 SetAttachState(Detach) 是 CCL 线格式硬约束：CigiBaseEntityPositionCtrl 的
// LatOrXoff/LonOrYoff/AltOrZoff 是同一组 union 成员，要发 LLA 就必须置 Detach（否则被
// 解释为相对父实体的 XYZ 偏移），并非用 AttachState 做业务层坐标系选择。本用例仅钉住
// 「Detach+LLA 正确走到 ELLIPSOID 语义位姿」这条既有路径，防止回归。

// =============================================================================
// 椭球场景里，Host 经 TCP 摆放实体后，IG 读到的是 LLA（状态同步设计.md §10，码 CMD-place-lla-tcp）。
// 实体 7，Detach。核对纬度、经度、高度和偏航。
// =============================================================================
SCENARIO("Host places an entity pose over TCP in Ellipsoid scene and IG reads LLA",
         "[acceptance][sync][cmd][e2e][entity-pose][ellipsoid][CMD-place-lla-tcp]")
{
    GIVEN("independent Host and an IG engine with an entity configured in Ellipsoid")
    {
        constexpr int kBase = 33650;

        const EntitiesConfig igCfg(
            R"([ { "id": 7, "model": "models/teapot.vsgt", )"
            R"("pose": { "ellipsoid": { "lla": { "lat": 39.9087, "lon": 116.3975, "alt": 0.0 }, )"
            R"("eulerYprDeg": [0, 0, 0] } } } ])",
            {}, true,
            std::string(R"("igConfig": { "udpRecvPort": )") + std::to_string(kBase + 1) +
                R"(, "hostAddr": "127.0.0.1", "hostTcpPort": )" + std::to_string(kBase + 100) +
                R"(, "hostUdpRecvPort": )" + std::to_string(kBase) + " }");

        HostSync hostA;
        REQUIRE(hostA.initialize(makeTestHostConfig(kBase)));

        Engine engineA;
        engineA.extent = {640, 480};
        engineA.showWindow = false;
        REQUIRE(engineA.loadConfig(igCfg.cfgFile->path()));
        REQUIRE(engineA.init());
        REQUIRE(hostA.readyIgCount() == 1);

        WHEN("Host sends EntityPositionCtrlV4 for entity 7 over TCP with Detach + LLA")
        {
            {
                auto& tcp = hostA.outMsgWithIgCtrlTcp();
                CigiEntityPositionCtrlV4 pose;
                pose.SetEntityID(7);
                pose.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
                pose.SetLat(39.9087);
                pose.SetLon(116.3975);
                pose.SetAlt(100.0);
                pose.SetYaw(15.0f);
                pose.SetPitch(0.0f);
                pose.SetRoll(0.0f);
                tcp << pose;
                hostA.flushTcp();
            }
            engineA.tickSync();

            THEN("engine entity 7 pose is read as LLA (ellipsoid semantic pose)")
            {
                vsg::dvec3 pos, ypr;
                requireEntity(engineA, 7).samplePose(pos, ypr);
                REQUIRE(pos.x == Catch::Approx(39.9087));
                REQUIRE(pos.y == Catch::Approx(116.3975));
                REQUIRE(pos.z == Catch::Approx(100.0));
                REQUIRE(ypr.x == Catch::Approx(15.0));

                auto mt = requireEntity(engineA, 7).transform();
                REQUIRE(mt);
            }
        }
    }
}

// =============================================================================
// EntityCtrl 业务生效（真 Host↔IG TCP）：现有「IG subscribes … EntityCtrl over TCP」
// 只验 sink 送达，不改。此处钉 IG 预建实体后 Switch / Alpha 是否随报文变化。
// 验收码 ENT-03-* 见 实体管理设计.md §10。
// =============================================================================

// =============================================================================
// 真链路下，Standby 实体收到 EntityCtrl Active 后变为可见（实体管理设计.md §10，码 ENT-03-active）。
// 与只验回调送达的用例分开。这里核对预建实体的可见性。
// =============================================================================
SCENARIO("linked IG shows a Standby entity after Host EntityCtrl Active",
         "[acceptance][sync][cmd][e2e][ENT-03-active]")
{
    GIVEN("a linked Host and an IG with a Standby catalog entity")
    {
        constexpr int kBase = 33800;
        const EntitiesConfig igCfg(R"([ { "id": 1, "model": "models/teapot.vsgt", "initialEntityState": "Standby" } ])",
                                   {}, false, makeIgConfigExtra(kBase));

        HostSync host;
        REQUIRE(host.initialize(makeTestHostConfig(kBase)));

        Engine engineA;
        engineA.extent = {640, 480};
        engineA.showWindow = false;
        REQUIRE(engineA.loadConfig(igCfg.cfgFile->path()));
        REQUIRE(engineA.init());
        REQUIRE(host.readyIgCount() == 1);
        REQUIRE_FALSE(requireEntity(engineA, 1).visible());

        WHEN("Host sends EntityCtrl Active")
        {
            sendHostEntityCtrl(host, engineA, 1, CigiBaseEntityCtrl::Active);

            THEN("the IG entity is visible")
            {
                REQUIRE(engineA.findEntity(1));
                REQUIRE(requireEntity(engineA, 1).visible());
            }
        }
    }
}

// =============================================================================
// 真链路下，Active 实体收到 Standby 后隐藏，登记还在（实体管理设计.md §10，码 ENT-03-standby）。
// 目录大小仍是 1。
// =============================================================================
SCENARIO("linked IG hides an Active entity after Host EntityCtrl Standby",
         "[acceptance][sync][cmd][e2e][ENT-03-standby]")
{
    GIVEN("a linked Host and an IG with an Active catalog entity")
    {
        constexpr int kBase = 33850;
        const EntitiesConfig igCfg(R"([ { "id": 1, "model": "models/teapot.vsgt" } ])", {}, false,
                                   makeIgConfigExtra(kBase));

        HostSync host;
        REQUIRE(host.initialize(makeTestHostConfig(kBase)));

        Engine engineA;
        engineA.extent = {640, 480};
        engineA.showWindow = false;
        REQUIRE(engineA.loadConfig(igCfg.cfgFile->path()));
        REQUIRE(engineA.init());
        REQUIRE(host.readyIgCount() == 1);
        REQUIRE(requireEntity(engineA, 1).visible());

        WHEN("Host sends EntityCtrl Standby")
        {
            sendHostEntityCtrl(host, engineA, 1, CigiBaseEntityCtrl::Standby);

            THEN("the IG entity is hidden and still registered")
            {
                REQUIRE(engineA.findEntity(1));
                REQUIRE_FALSE(requireEntity(engineA, 1).visible());
                REQUIRE(engineA.entitySize() == 1);
            }
        }
    }
}

// =============================================================================
// 真链路下，实体保持 Active 时 Alpha 变为 128（实体管理设计.md §10，码 ENT-03-attrs）。
// 可见性不变。
// =============================================================================
SCENARIO("linked IG applies EntityCtrl Alpha while the entity stays Active",
         "[acceptance][sync][cmd][e2e][ENT-03-attrs]")
{
    GIVEN("a linked Host and an IG with an Active catalog entity")
    {
        constexpr int kBase = 33900;
        const EntitiesConfig igCfg(R"([ { "id": 1, "model": "models/teapot.vsgt" } ])", {}, false,
                                   makeIgConfigExtra(kBase));

        HostSync host;
        REQUIRE(host.initialize(makeTestHostConfig(kBase)));

        Engine engineA;
        engineA.extent = {640, 480};
        engineA.showWindow = false;
        REQUIRE(engineA.loadConfig(igCfg.cfgFile->path()));
        REQUIRE(engineA.init());
        REQUIRE(host.readyIgCount() == 1);
        REQUIRE(requireEntity(engineA, 1).visible());
        REQUIRE(requireEntity(engineA, 1).alpha() == std::uint8_t{255});

        WHEN("Host sends EntityCtrl Active with Alpha 128")
        {
            sendHostEntityCtrl(host, engineA, 1, CigiBaseEntityCtrl::Active, 128);

            THEN("the IG entity stays visible and alpha is 128")
            {
                REQUIRE(requireEntity(engineA, 1).visible());
                REQUIRE(requireEntity(engineA, 1).alpha() == std::uint8_t{128});
            }
        }
    }
}

// =============================================================================
// 一次 TCP 消息打满权威表量级的 EntityCtrl，验 IG 分帧解包把每条都投递到业务回调
// （状态同步设计.md §10，码 CMD-burst-entityctrl）。
// 不是 Catch BENCHMARK：固定 200 条，断言条数与 id，不钉耗时。
// 不加载实体目录：本码不验收 entitySize，只数回调条数和互异 id。
// =============================================================================
SCENARIO("linked IG unpacks two hundred EntityCtrl from one Host TCP flush",
         "[integration][stress][sync][cmd][e2e][CMD-burst-entityctrl]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        constexpr int kEntityCount = 200;

        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 34000);

        int unpackcount = 0;
        std::unordered_set<Cigi_uint16> unpackedIds;
        engineA.igSync().addCallback<CigiEntityCtrlV4>([&](const CigiEntityCtrlV4& ctrl) {
            ++unpackcount;
            unpackedIds.insert(ctrl.GetEntityID());
        });

        WHEN("Host sends two hundred EntityCtrl Active in one TCP flush")
        {
            auto& tcp = host.outMsgWithIgCtrlTcp();
            for (int id = 1; id <= kEntityCount; ++id)
            {
                CigiEntityCtrlV4 ent;
                ent.SetEntityID(static_cast<Cigi_uint16>(id));
                ent.SetEntityState(CigiBaseEntityCtrl::Active);
                tcp << ent;
            }
            host.flushTcp();
            tickIgUntil(engineA, [&] { return unpackcount == kEntityCount; });

            THEN("the IG unpackcount every EntityCtrl and all entities are visible")
            {
                REQUIRE(unpackcount == kEntityCount);
                REQUIRE(unpackedIds.size() == static_cast<std::size_t>(kEntityCount));
            }
        }
    }
}

// =============================================================================
// 12. 跨链路单次投递：EntityPositionCtrlV4 在 UDP/TCP 双链路各注册一个通用捕获
//     （_eyeProc / _entityPoseProc），addCallback 一个回调会同时挂到两个 processor
//     （存储层冗余），但报文按链路喂 session，每次发送只触发其中一个——回调恰好调用一次。
// =============================================================================

// =============================================================================
// 同一条 EntityPositionCtrlV4 分别走 TCP 和 UDP，IG sink 每条只触发一次（状态同步设计.md §10，码 CMD-posctrl-tcp-udp）。
// =============================================================================
SCENARIO("IG sink callback fires once per EntityPositionCtrlV4 over both TCP and UDP",
         "[acceptance][sync][cmd][e2e][entity-pose][CMD-posctrl-tcp-udp]")
{
    GIVEN("an independent Host and one IG-only engine linked over real sockets")
    {
        HostSync host;
        Engine engineA;
        setupHostIgPair(host, engineA, 33700);

        int sinkCount = 0;
        CigiEntityPositionCtrlV4 sinkValue;
        // 同一回调注册到 UDP（_eyeProc）与 TCP（_entityPoseProc）两个通用捕获
        //（addCallback 按类型遍历全部匹配 processor，§8.1）。
        engineA.igSync().addCallback<CigiEntityPositionCtrlV4>(
            [&](const CigiEntityPositionCtrlV4& pose) {
                ++sinkCount;
                sinkValue = pose;
            });

        WHEN("Host sends one EntityPositionCtrlV4 over TCP")
        {
            {
                auto& tcp = host.outMsgWithIgCtrlTcp();
                CigiEntityPositionCtrlV4 pose;
                pose.SetEntityID(7);
                pose.SetAttachState(CigiBaseEntityPositionCtrl::Attach);
                pose.SetXoff(1.0);
                pose.SetYoff(2.0);
                pose.SetZoff(3.0);
                tcp << pose;
                host.flushTcp();
            }
            engineA.tickSync();

            THEN("IG sink receives the packet exactly once")
            {
                REQUIRE(sinkCount == 1);
                REQUIRE(sinkValue.GetEntityID() == 7);
                REQUIRE(sinkValue.GetXoff() == Catch::Approx(1.0));
            }
        }

        WHEN("Host sends one EntityPositionCtrlV4 over UDP")
        {
            sinkCount = 0;

            auto& udp = host.outMsgWithIgCtrlUdp();
            CigiEntityPositionCtrlV4 pose;
            pose.SetEntityID(7);
            pose.SetAttachState(CigiBaseEntityPositionCtrl::Attach);
            pose.SetXoff(4.0);
            pose.SetYoff(5.0);
            pose.SetZoff(6.0);
            udp << pose;
            host.flushUdp();
            // 不能「发完立刻 tick 再钉死收包数」：CI 上回环 UDP 经常晚一拍到达。
            // 合同仍是「到了就恰好一次」，不是「丢包也算过」。
            tickIgUntil(engineA, [&] { return sinkCount > 0; });

            THEN("IG sink receives the packet exactly once over UDP too")
            {
                REQUIRE(sinkCount == 1);
                REQUIRE(sinkValue.GetEntityID() == 7);
                REQUIRE(sinkValue.GetXoff() == Catch::Approx(4.0));
            }
        }
    }
}
