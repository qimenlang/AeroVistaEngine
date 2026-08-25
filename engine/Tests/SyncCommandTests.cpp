// 状态同步设计初版.md §10 验收（新契约：引用式发送 + 无业务回执 + CCL 标准报文 + processor）。
// 命令面全绿：2 线格式契约（直接测 CCL）+ 6 E2E + 4 分帧单测。
// §7 双 session 隔离红测（2026-08）：2 负向（flushUdp 误发 TCP 缓冲当前会真发 → 红）+ 2 正向（flushTcp 正常送达 → 绿）。

#include <aerovista/sync/CigiIncludes.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Common.h"
#include "engine.h"
#include <aerovista/sync/CigiWire.h>
#include <aerovista/sync/HostSync.h>
#include <aerovista/sync/IgSync.h>

#include "CigiBaseCollDetSegDef.h"
#include "CigiBaseCollDetSegResp.h"
#include "CigiBaseCollDetVolDef.h"
#include "CigiBaseCollDetVolResp.h"
#include "CigiBaseEntityPositionCtrl.h"
#include "CigiBaseEventProcessor.h"
#include "CigiCollDetSegDefV4.h"
#include "CigiCollDetSegRespV4.h"
#include "CigiCollDetVolDefV4.h"
#include "CigiCollDetVolRespV4.h"
#include "CigiEntityPositionCtrlV4.h"
#include "CigiHostSession.h"
#include "CigiIGCtrlV4.h"
#include "CigiIGMsgV4.h"
#include "CigiSymbolTextDefV4.h"

#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using aerovista::sync::HostSync;
using aerovista::sync::IgSync;
namespace cigi_wire = aerovista::sync::cigi_wire;

namespace
{
    // 独立 Host 端点 + 两个 IG-only 引擎（自连产生 2 个 ready peer）。
    // engine 拆 Host 后（2026-08）HostSync 独立持有，不再经 Engine::hostSync。
    void setupHostIgPair(HostSync& hostA, Engine& a, Engine& b, int base)
    {
        a.extent = b.extent = {640, 480};
        a.showWindow = b.showWindow = false;
        REQUIRE(hostA.initialize(makeTestHostConfig(base)));
        hostA.run();
        REQUIRE(a.initSync(makeTestIgConfig(base + 1, base)));
        REQUIRE(b.initSync(makeTestIgConfig(base + 3, base)));
        REQUIRE(hostA.readyIgCount() == 2);
        REQUIRE(a.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));
    }

    // 独立 Host 端点 + 两个 IG-only 引擎 B/C（自连产生 3 个 ready peer）。
    void setupHostIgTriple(HostSync& hostA, Engine& a, Engine& b, Engine& c, int base)
    {
        a.extent = b.extent = c.extent = {640, 480};
        a.showWindow = b.showWindow = c.showWindow = false;
        REQUIRE(hostA.initialize(makeTestHostConfig(base)));
        hostA.run();
        REQUIRE(a.initSync(makeTestIgConfig(base + 1, base)));
        REQUIRE(b.initSync(makeTestIgConfig(base + 3, base)));
        REQUIRE(c.initSync(makeTestIgConfig(base + 5, base)));
        REQUIRE(hostA.readyIgCount() == 3);
        REQUIRE(a.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));
    }

    void tickBoth(Engine& a, Engine& b, int frames = 5)
    {
        for (int i = 0; i < frames; ++i)
        {
            REQUIRE(a.tickOnFrame());
            b.tickSync();
        }
    }

    void tickAll(Engine& a, Engine& b, Engine& c, int frames = 5)
    {
        for (int i = 0; i < frames; ++i)
        {
            REQUIRE(a.tickOnFrame());
            b.tickSync();
            c.tickSync();
        }
    }

    // engine 层定义的业务 processor（§8.1）：捕获 EntityPositionCtrlV4 字段。
    // OnPacketReceived 只在主线程解包时调用（§6），测试主线程读写，无并发，用普通成员。
    // ownship（EntityID==0）是数据面眼点保留实体（§4.1），业务 processor 须过滤。
    class TestPlaceProcessor : public CigiBaseEventProcessor
    {
    public:
        void OnPacketReceived(CigiBasePacket* packet) override
        {
            auto* place = dynamic_cast<CigiEntityPositionCtrlV4*>(packet);
            if (!place || place->GetEntityID() == 0)
                return;
            received = true;
            entityId = place->GetEntityID();
            attachState = place->GetAttachState();
            lat = place->GetLat();
            lon = place->GetLon();
            alt = place->GetAlt();
        }
        bool received = false;
        std::uint16_t entityId = 0;
        CigiBaseEntityPositionCtrl::AttachStateGrp attachState = CigiBaseEntityPositionCtrl::Detach;
        double lat = 0.0;
        double lon = 0.0;
        double alt = 0.0;
    };

    // engine 层定义的业务 processor（§8.1）：捕获 SymbolTextDefV4 并记录文本（多指令分发用）。
    class TestTextProcessor : public CigiBaseEventProcessor
    {
    public:
        void OnPacketReceived(CigiBasePacket* packet) override
        {
            auto* txt = dynamic_cast<CigiSymbolTextDefV4*>(packet);
            if (!txt)
                return;
            _texts.push_back(txt->GetText());
        }
        std::size_t count() const
        {
            return _texts.size();
        }
        std::vector<std::string> texts() const
        {
            return _texts;
        }

    private:
        std::vector<std::string> _texts;
    };

    // 业务 processor：捕获 IG→Host 的 CigiIGMsgV4（CCL 原生 IG→Host 报文，§8.1 对等）。
    class TestIgMsgProcessor : public CigiBaseEventProcessor
    {
    public:
        void OnPacketReceived(CigiBasePacket* packet) override
        {
            auto* igmsg = dynamic_cast<CigiIGMsgV4*>(packet);
            if (!igmsg)
                return;
            // CCL 把 IGMsg.Msg 按 8 字节对齐（含 NUL 填充），按 C 字符串语义取文本。
            const std::size_t len = ::strnlen(igmsg->GetMsg(), igmsg->GetMsgLen());
            _messages.emplace_back(igmsg->GetMsgID(), std::string(igmsg->GetMsg(), len));
        }
        std::size_t count() const
        {
            return _messages.size();
        }
        std::vector<std::pair<std::uint16_t, std::string>> messages() const
        {
            return _messages;
        }

    private:
        std::vector<std::pair<std::uint16_t, std::string>> _messages;
    };

    // 捕获 CigiCollDetSegDefV4（Host→IG）：记录 EntityID / SegmentEn / Mask。
    class TestCollDetSegDefProcessor : public CigiBaseEventProcessor
    {
    public:
        void OnPacketReceived(CigiBasePacket* packet) override
        {
            auto* def = dynamic_cast<CigiCollDetSegDefV4*>(packet);
            if (!def)
                return;
            got = true;
            entityId = def->GetEntityID();
            segmentEn = def->GetSegmentEn();
            mask = def->GetMask();
        }
        bool got = false;
        std::uint16_t entityId = 0;
        bool segmentEn = false;
        std::uint32_t mask = 0;
    };

    // 捕获 CigiCollDetSegRespV4（IG→Host）：记录 EntityID / Material。
    class TestCollDetSegRespProcessor : public CigiBaseEventProcessor
    {
    public:
        void OnPacketReceived(CigiBasePacket* packet) override
        {
            auto* resp = dynamic_cast<CigiCollDetSegRespV4*>(packet);
            if (!resp)
                return;
            got = true;
            entityId = resp->GetEntityID();
            material = resp->GetMaterial();
        }
        bool got = false;
        std::uint16_t entityId = 0;
        std::uint32_t material = 0;
    };

    // 用 CCL HostSession 组装一条 CIGI 消息（自动前置 IGCtrl 帧头），返回线上字节。
    // 供 CigiFrameAssembler 分帧单测使用（验证消息级切流：IGCtrl 开头 + 后续包）。
    std::vector<unsigned char> packPoseMessage(std::uint16_t entityId)
    {
        auto session = std::make_unique<CigiHostSession>(1, 4096, 1, 4096);
        auto& omsg = session->GetOutgoingMsgMgr();
        omsg.BeginMsg();
        CigiIGCtrlV4 igCtrl;
        omsg << igCtrl;
        CigiEntityPositionCtrlV4 place;
        place.SetEntityID(entityId);
        place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
        place.SetLat(31.23);
        place.SetLon(121.47);
        place.SetAlt(500.0);
        omsg << place;
        Cigi_uint8* buf = nullptr;
        int len = 0;
        if (omsg.PackageMsg(&buf, len) != CIGI_SUCCESS || buf == nullptr)
            return {};
        std::vector<unsigned char> out(buf, buf + len);
        omsg.FreeMsg();
        return out;
    }

    // IGCtrl + EntityPositionCtrlV4 + SymbolTextDefV4（一条消息，两个数据包）。
    std::vector<unsigned char> packPosePlusTextMessage(std::uint16_t entityId, const std::string& text)
    {
        auto session = std::make_unique<CigiHostSession>(1, 4096, 1, 4096);
        auto& omsg = session->GetOutgoingMsgMgr();
        omsg.BeginMsg();
        CigiIGCtrlV4 igCtrl;
        omsg << igCtrl;
        CigiEntityPositionCtrlV4 place;
        place.SetEntityID(entityId);
        place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
        place.SetLat(31.23);
        place.SetLon(121.47);
        place.SetAlt(500.0);
        CigiSymbolTextDefV4 cmd(text.c_str());
        omsg << place << cmd;
        Cigi_uint8* buf = nullptr;
        int len = 0;
        if (omsg.PackageMsg(&buf, len) != CIGI_SUCCESS || buf == nullptr)
            return {};
        std::vector<unsigned char> out(buf, buf + len);
        omsg.FreeMsg();
        return out;
    }
} // namespace

// =============================================================================
// 1. 线格式契约（单元，直接测 CCL，绿，锚定 §4.1）
// =============================================================================

TEST_CASE("CigiEntityPositionCtrlV4 packs to fixed 48B", "[unit][sync][cmd][wire-contract]")
{
    CigiEntityPositionCtrlV4 place;
    place.SetEntityID(7);
    place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
    place.SetLat(31.23);
    place.SetLon(121.47);
    place.SetAlt(500.0);

    Cigi_uint8 buf[CIGI_ENTITY_POSITION_CTRL_PACKET_SIZE_V4] = {};
    const int size = place.Pack(&place, buf, nullptr);
    REQUIRE(size == CIGI_ENTITY_POSITION_CTRL_PACKET_SIZE_V4);
    // CCL 在 x86 输出小端（CIGI 4 不强制字节序，接收方检测）；按小端解析头字段。
    const int packetSize = buf[0] | (buf[1] << 8);
    const int packetId = buf[2] | (buf[3] << 8);
    REQUIRE(packetSize == CIGI_ENTITY_POSITION_CTRL_PACKET_SIZE_V4);
    REQUIRE(packetId == CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4);
}

TEST_CASE("CigiSymbolTextDefV4 packs variable-length Text", "[unit][sync][cmd][wire-contract]")
{
    CigiSymbolTextDefV4 cmd("place 7 121.47 31.23 500");
    Cigi_uint8 buf[128] = {};
    const int size = cmd.Pack(&cmd, buf, nullptr);
    REQUIRE(size >= CIGI_SYMBOL_TEXT_DEFINITION_PACKET_SIZE_V4);
    REQUIRE(size % 8 == 0);
    const int packetSize = buf[0] | (buf[1] << 8);
    const int packetId = buf[2] | (buf[3] << 8);
    REQUIRE(packetSize == size);
    REQUIRE(packetId == CIGI_SYMBOL_TEXT_DEFINITION_PACKET_ID_V4);
}

// =============================================================================
// 2. E2E：真实 socket 收发（beginWithIgCtrl/flushTcp/registerEventProcessor）
// =============================================================================

SCENARIO("Host places an entity pose over TCP via outMsgWithIgCtrlTcp/flushTcp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 31000);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, placeProc.get());

        WHEN("Host assembles EntityPositionCtrlV4 and flushes TCP")
        {
            auto& tcp = hostA.outMsgWithIgCtrlTcp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            tcp << place;
            hostA.flushTcp();
            tickBoth(engineA, engineB);

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

SCENARIO("Host sends a text command over TCP via SymbolTextDefV4",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 31150);

        auto textProc = std::make_shared<TestTextProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_SYMBOL_TEXT_DEFINITION_PACKET_ID_V4, textProc.get());

        WHEN("Host assembles SymbolTextDefV4 and flushes TCP")
        {
            auto& tcp = hostA.outMsgWithIgCtrlTcp();
            CigiSymbolTextDefV4 cmd("place 7 121.47 31.23 500");
            tcp << cmd;
            hostA.flushTcp();
            tickBoth(engineA, engineB);

            THEN("IG received the exact text command")
            {
                REQUIRE(textProc->count() == 1);
                REQUIRE(textProc->texts().at(0) == "place 7 121.47 31.23 500");
            }
        }
    }
}

SCENARIO("Host fans out a command to multiple IGs via flushTcp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and two IG-only engines B and C linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        Engine engineC;
        HostSync hostA;
        setupHostIgTriple(hostA, engineA, engineB, engineC, 31300);

        auto procB = std::make_shared<TestPlaceProcessor>();
        auto procC = std::make_shared<TestPlaceProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, procB.get());
        engineC.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, procC.get());

        WHEN("Host flushes one EntityPositionCtrlV4 to all ready IGs")
        {
            auto& tcp = hostA.outMsgWithIgCtrlTcp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            tcp << place;
            hostA.flushTcp();
            tickAll(engineA, engineB, engineC);

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

SCENARIO("IG dispatches different text commands by first token",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 31200);

        auto textProc = std::make_shared<TestTextProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_SYMBOL_TEXT_DEFINITION_PACKET_ID_V4, textProc.get());

        WHEN("Host sends two different text commands over TCP")
        {
            CigiSymbolTextDefV4 place("place 7 121.47 31.23 500");
            CigiSymbolTextDefV4 reset("reset");
            {
                auto& tcp = hostA.outMsgWithIgCtrlTcp();
                tcp << place;
                hostA.flushTcp();
            }
            {
                auto& tcp = hostA.outMsgWithIgCtrlTcp();
                tcp << reset;
                hostA.flushTcp();
            }
            tickBoth(engineA, engineB);

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

SCENARIO("Host sends multiple packets in one message and IG dispatches each by PacketID",
         "[integration][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 31400);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        auto textProc = std::make_shared<TestTextProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, placeProc.get());
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_SYMBOL_TEXT_DEFINITION_PACKET_ID_V4, textProc.get());

        WHEN("Host assembles two packets into one TCP message and flushes once")
        {
            auto& tcp = hostA.outMsgWithIgCtrlTcp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            CigiSymbolTextDefV4 cmd("reset");
            tcp << place << cmd; // 批量两个报文 = 一条消息
            hostA.flushTcp();
            tickBoth(engineA, engineB);

            THEN("both processors were triggered")
            {
                REQUIRE(placeProc->received);
                REQUIRE(textProc->count() > 0);
            }
        }
    }
}

SCENARIO("Host streams real-time entity pose over UDP via outMsgWithIgCtrlUdp/flushUdp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 31500);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, placeProc.get());

        WHEN("Host assembles EntityPositionCtrlV4 and flushes UDP each frame")
        {
            // outMsgWithIgCtrlUdp() 已自动前置 IGCtrl（§7.1），业务侧只 << 命令报文。
            auto& udp = hostA.outMsgWithIgCtrlUdp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            udp << place;
            hostA.flushUdp();
            tickBoth(engineA, engineB);

            THEN("IG received the real-time pose via registered processor")
            {
                REQUIRE(placeProc->received);
            }
        }
    }
}

// =============================================================================
// 4. TCP 分帧器（CigiFrameAssembler）单元测试——§4.2 消息级切流
// =============================================================================

TEST_CASE("CigiFrameAssembler emits one complete message as one frame",
          "[unit][sync][cmd][framing]")
{
    cigi_wire::CigiFrameAssembler assembler;
    const auto msg = packPoseMessage(7);
    REQUIRE_FALSE(msg.empty());

    std::vector<std::vector<unsigned char>> frames;
    assembler.feed(msg.data(), static_cast<int>(msg.size()),
                   [&](const std::vector<unsigned char>& f) { frames.push_back(f); });

    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0] == msg);
    REQUIRE(assembler.bufferEmpty());
}

TEST_CASE("CigiFrameAssembler splits sticky messages (two messages in one feed)",
          "[unit][sync][cmd][framing]")
{
    cigi_wire::CigiFrameAssembler assembler;
    const auto a = packPoseMessage(7);
    const auto b = packPoseMessage(8);
    REQUIRE_FALSE(a.empty());
    REQUIRE_FALSE(b.empty());

    std::vector<unsigned char> two = a;
    two.insert(two.end(), b.begin(), b.end());

    std::vector<std::vector<unsigned char>> frames;
    assembler.feed(two.data(), static_cast<int>(two.size()),
                   [&](const std::vector<unsigned char>& f) { frames.push_back(f); });

    REQUIRE(frames.size() == 2);
    REQUIRE(frames[0] == a);
    REQUIRE(frames[1] == b);
    REQUIRE(assembler.bufferEmpty());
}

TEST_CASE("CigiFrameAssembler buffers a split message across feeds",
          "[unit][sync][cmd][framing]")
{
    cigi_wire::CigiFrameAssembler assembler;
    const auto msg = packPoseMessage(7);
    REQUIRE_FALSE(msg.empty());
    const std::size_t half = msg.size() / 2;

    std::vector<std::vector<unsigned char>> frames;
    auto onFrame = [&](const std::vector<unsigned char>& f) { frames.push_back(f); };

    assembler.feed(msg.data(), static_cast<int>(half), onFrame);
    REQUIRE(frames.empty());                // 半包：尚未切出
    REQUIRE_FALSE(assembler.bufferEmpty()); // 残留缓冲

    assembler.feed(msg.data() + half, static_cast<int>(msg.size() - half), onFrame);
    REQUIRE(frames.size() == 1); // 补齐后切出一条完整
    REQUIRE(frames[0] == msg);
    REQUIRE(assembler.bufferEmpty());
}

TEST_CASE("CigiFrameAssembler keeps a multi-packet message as one frame",
          "[unit][sync][cmd][framing]")
{
    cigi_wire::CigiFrameAssembler assembler;
    const auto msg = packPosePlusTextMessage(7, "reset");
    REQUIRE_FALSE(msg.empty());

    std::vector<std::vector<unsigned char>> frames;
    assembler.feed(msg.data(), static_cast<int>(msg.size()),
                   [&](const std::vector<unsigned char>& f) { frames.push_back(f); });

    // IGCtrl + EntityPositionCtrlV4 + SymbolTextDefV4 = 一条消息，不按单包切。
    REQUIRE(frames.size() == 1);
    REQUIRE(frames[0] == msg);
    REQUIRE(assembler.bufferEmpty());
}

// =============================================================================
// 5. 双向命令面：IG 发送报文 → Host 注册 processor 处理（HostSync::registerEventProcessor）
// =============================================================================

SCENARIO("IG sends a message to Host and Host processor receives it",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 31600);

        // Host 侧对等注册：处理 IG 发来的 CigiIGMsgV4（CCL 原生 IG→Host 报文，§8.1 对等）。
        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        hostA.registerEventProcessor(
            CIGI_IG_MSG_PACKET_ID_V4, hostMsgProc.get());

        WHEN("IG assembles CigiIGMsgV4 and flushes TCP")
        {
            auto& tcp = engineB.synchronSystem().igSync().outMsgWithSofTcp();
            CigiIGMsgV4 status;
            status.SetMsgID(0x1001);
            status.SetMsg("status ok");
            tcp << status;
            engineB.synchronSystem().igSync().flushTcp();

            // Host 收包为 push 模式：等待 peer 线程收包分帧入队后，主线程 drain 解包。
            for (int i = 0; i < 20 && hostMsgProc->count() == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                hostA.drainIncoming();
            }

            THEN("Host received the IGMsg via registered processor")
            {
                REQUIRE(hostMsgProc->count() == 1);
                REQUIRE(hostMsgProc->messages().at(0).first == 0x1001);
                REQUIRE(hostMsgProc->messages().at(0).second == "status ok");
            }
        }
    }
}

SCENARIO("Host sends CollDetSegDef and IG replies CollDetSegResp over TCP",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 31700);

        // 双向注册：IG 处理 Host 发来的 CollDetSegDefV4；Host 处理 IG 回发的 CollDetSegRespV4。
        auto igDefProc = std::make_shared<TestCollDetSegDefProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_COLL_DET_SEG_DEF_PACKET_ID_V4, igDefProc.get());
        auto hostRespProc = std::make_shared<TestCollDetSegRespProcessor>();
        hostA.registerEventProcessor(
            CIGI_COLL_DET_SEG_RESP_PACKET_ID_V4, hostRespProc.get());

        WHEN("Host sends CollDetSegDefV4 over TCP, IG processes and replies CollDetSegRespV4")
        {
            // Host → IG：碰撞检测段定义。
            {
                auto& tcp = hostA.outMsgWithIgCtrlTcp();
                CigiCollDetSegDefV4 def;
                def.SetEntityID(7);
                def.SetSegmentEn(true);
                def.SetMask(0x00FF00FFu);
                tcp << def;
                hostA.flushTcp();
            }

            // IG 主线程解包（drainIncoming）并确认收到。
            for (int i = 0; i < 20 && !igDefProc->got; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                engineB.tickSync();
            }
            REQUIRE(igDefProc->got);
            REQUIRE(igDefProc->entityId == 7);
            REQUIRE(igDefProc->segmentEn);
            REQUIRE(igDefProc->mask == 0x00FF00FFu);

            // IG → Host：碰撞检测段响应。
            {
                auto& tcp = engineB.synchronSystem().igSync().outMsgWithSofTcp();
                CigiCollDetSegRespV4 resp;
                resp.SetEntityID(igDefProc->entityId);
                resp.SetMaterial(0xABC);
                tcp << resp;
                engineB.synchronSystem().igSync().flushTcp();
            }

            // Host push 模式：等待 peer 线程收包入队后，主线程 drain 解包。
            for (int i = 0; i < 20 && !hostRespProc->got; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                hostA.drainIncoming();
            }

            THEN("Host received the CollDetSegResp via registered processor")
            {
                REQUIRE(hostRespProc->got);
                REQUIRE(hostRespProc->entityId == 7);
                REQUIRE(hostRespProc->material == 0xABC);
            }
        }
    }
}

SCENARIO("IG sends a UDP message and Host processor receives it",
         "[acceptance][bdd][sync][cmd][e2e][debug]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 31800);

        // Host 对等注册：处理 IG 经 UDP 发来的 CigiIGMsgV4（§8.1 对等，收发均支持 TCP/UDP）。
        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        hostA.registerEventProcessor(
            CIGI_IG_MSG_PACKET_ID_V4, hostMsgProc.get());

        WHEN("IG assembles CigiIGMsgV4 and flushes UDP")
        {
            auto& udp = engineB.synchronSystem().igSync().outMsgWithSofUdp();
            CigiIGMsgV4 status;
            status.SetMsgID(0x2001);
            status.SetMsg("udp status ok");
            udp << status;
            engineB.synchronSystem().igSync().flushUdp();

            // Host push 模式：等待 I/O 线程收包入队（UDP 非阻塞 1ms 轮询）后，主线程 drain 解包。
            for (int i = 0; i < 5 && hostMsgProc->count() == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                hostA.drainIncoming();
            }

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
// 6. 碰撞检测体积对（ATDD 红测）：Host 发 CollDetVolDefV4 → IG 回 CollDetVolRespV4
//    设计规则：processor 定义于 EventProcess（§8.1），由 IgSync/HostSync 内置注册并缓存，
//    经 takeReceivedCollDetVolDef()/takeReceivedCollDetVolResp() 取缓存——当前接口不存在，
//    测试编译失败（真红）；实现后转绿。
// =============================================================================

SCENARIO("Host sends CollDetVolDef and IG replies CollDetVolResp over TCP",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 31900);

        WHEN("Host sends CollDetVolDefV4 over TCP, IG processes and replies CollDetVolRespV4")
        {
            // Host → IG：碰撞检测体积定义（首版默认值填充）。
            {
                auto& tcp = hostA.outMsgWithIgCtrlTcp();
                CigiCollDetVolDefV4 def;
                def.SetEntityID(7);
                def.SetVolID(3);
                def.SetVolEn(true);
                tcp << def;
                hostA.flushTcp();
            }

            // IG 主线程解包（drainIncoming）并经内置 processor 缓存。
            std::optional<CigiCollDetVolDefV4> igDef;
            for (int i = 0; i < 20 && !igDef; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                engineB.tickSync();
                igDef = engineB.synchronSystem().igSync().takeReceivedCollDetVolDef();
            }
            REQUIRE(igDef.has_value()); // 红：takeReceivedCollDetVolDef 尚未实现（编译红）
            REQUIRE(igDef->GetEntityID() == 7);
            REQUIRE(igDef->GetVolID() == 3);
            REQUIRE(igDef->GetVolEn());

            // IG → Host：碰撞检测体积响应（首版默认值填充）。
            {
                auto& tcp = engineB.synchronSystem().igSync().outMsgWithSofTcp();
                CigiCollDetVolRespV4 resp;
                resp.SetEntityID(igDef->GetEntityID());
                resp.SetCollType(CigiBaseCollDetVolResp::Entity);
                tcp << resp;
                engineB.synchronSystem().igSync().flushTcp();
            }

            // Host push 模式：等待 peer 线程收包入队后，主线程 drain 解包，经内置 processor 缓存。
            std::optional<CigiCollDetVolRespV4> hostResp;
            for (int i = 0; i < 20 && !hostResp; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                hostA.drainIncoming();
                hostResp = hostA.takeReceivedCollDetVolResp();
            }

            THEN("Host received the CollDetVolResp via built-in processor cache")
            {
                REQUIRE(hostResp.has_value()); // 红：takeReceivedCollDetVolResp 尚未实现（编译红）
                REQUIRE(hostResp->GetEntityID() == 7);
                REQUIRE(hostResp->GetCollType() == CigiBaseCollDetVolResp::Entity);
            }
        }
    }
}

// =============================================================================
// 7. 双 session 隔离（状态同步设计初版.md §5.1/§7.1/§8.1 验收，ATDD 红测）：
//    outMsgWithIgCtrlTcp/flushTcp 只操作 _tcpSession、beginWithIgCtrlUdp/flushUdp 只操作 _udpSession——
//    命令面内容被 flushUdp 误发时对端收不到。当前单 session 下误发会真发（负向红）。
// =============================================================================

SCENARIO("Host TCP-filled message is not sent via flushUdp",
         "[acceptance][bdd][sync][cmd][e2e][negative]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 32000);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, placeProc.get());

        WHEN("Host fills a TCP outgoing message but flushes UDP")
        {
            auto& tcp = hostA.outMsgWithIgCtrlTcp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            tcp << place;
            hostA.flushUdp(); // 误用：TCP 缓冲走 UDP flush

            // 给对端留足解包机会：若误发，IG 会收到并触发 processor（单 session 下即红）；
            // 双 session 后 flushUdp 打包空的 _udpSession（无内容）→ 不发 → IG 无报文可解。
            tickBoth(engineA, engineB);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            tickBoth(engineA, engineB);

            THEN("IG received nothing (flushUdp ignores the TCP session buffer)")
            {
                REQUIRE_FALSE(placeProc->received);
            }
        }
    }
}

SCENARIO("Host TCP-filled message is delivered via flushTcp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 32050);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, placeProc.get());

        WHEN("Host fills a TCP outgoing message and flushes TCP")
        {
            auto& tcp = hostA.outMsgWithIgCtrlTcp();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            tcp << place;
            hostA.flushTcp();
            tickBoth(engineA, engineB);

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

SCENARIO("IG TCP-filled message is not sent via flushUdp",
         "[acceptance][bdd][sync][cmd][e2e][negative]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 32100);

        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        hostA.registerEventProcessor(CIGI_IG_MSG_PACKET_ID_V4, hostMsgProc.get());

        WHEN("IG fills a TCP outgoing message but flushes UDP")
        {
            auto& tcp = engineB.synchronSystem().igSync().outMsgWithSofTcp();
            CigiIGMsgV4 status;
            status.SetMsgID(0x1001);
            status.SetMsg("status ok");
            tcp << status;
            engineB.synchronSystem().igSync().flushUdp(); // 误用：TCP 缓冲走 UDP flush

            // 若误发，Host 的 UDP I/O 线程会收到 → drain 解包触发 processor（单 session 下即红）；
            // 双 session 后 flushUdp 打包空的 _udpSession（无内容）→ 不发 → Host 无报文可解。
            for (int i = 0; i < 5; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                hostA.drainIncoming();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            hostA.drainIncoming();

            THEN("Host received nothing (flushUdp ignores the TCP session buffer)")
            {
                REQUIRE(hostMsgProc->count() == 0);
            }
        }
    }
}

SCENARIO("IG TCP-filled message is delivered via flushTcp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 32150);

        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        hostA.registerEventProcessor(CIGI_IG_MSG_PACKET_ID_V4, hostMsgProc.get());

        WHEN("IG fills a TCP outgoing message and flushes TCP")
        {
            auto& tcp = engineB.synchronSystem().igSync().outMsgWithSofTcp();
            CigiIGMsgV4 status;
            status.SetMsgID(0x1001);
            status.SetMsg("status ok");
            tcp << status;
            engineB.synchronSystem().igSync().flushTcp();

            for (int i = 0; i < 20 && hostMsgProc->count() == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                hostA.drainIncoming();
            }

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
// 8. IGCtrl 自动填充（状态同步设计初版.md §7.1 / §10 验收，2026-08-25）：
//    outMsgWithIgCtrlUdp() 自动前置 IGCtrl（帧号=数据面、TimeStamp=自计时、TimeStampValid=true）；
//    outMsgWithIgCtrlTcp() 自动前置 IGCtrl（帧号=命令面、TimeStampValid=false、帧号连续）。
// =============================================================================

namespace
{
    // 捕获 IGCtrl 帧头字段（FrameCntr / TimeStampValid）——验证 Host 出站首包约束。
    class TestIgCtrlCapture : public CigiBaseEventProcessor
    {
    public:
        void OnPacketReceived(CigiBasePacket* packet) override
        {
            auto* ig = dynamic_cast<CigiIGCtrlV4*>(packet);
            if (!ig)
                return;
            frameCntrs.push_back(ig->GetFrameCntr());
            timeStampValids.push_back(ig->GetTimeStampValid());
        }
        std::vector<std::uint32_t> frameCntrs;
        std::vector<bool> timeStampValids;
    };
} // namespace

SCENARIO("Host UDP frames carry valid IGCtrl first packet with timestamp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 32200);

        auto igCtrlCapture = std::make_shared<TestIgCtrlCapture>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_IG_CTRL_PACKET_ID_V4, igCtrlCapture.get());

        WHEN("Host flushes two UDP data-plane frames")
        {
            // outMsgWithIgCtrlUdp() 自动前置 IGCtrl（帧号=数据面、TimeStamp=自计时、TimeStampValid=true）。
            for (int i = 0; i < 2; ++i)
            {
                auto& udp = hostA.outMsgWithIgCtrlUdp();
                hostA.flushUdp();
            }
            tickBoth(engineA, engineB);

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
                REQUIRE(engineB.synchronSystem().igSync().lastHostSimTimeUs() > 0);
            }
        }
    }
}

SCENARIO("Host TCP messages carry IGCtrl first packet with invalid timestamp and continuous frame counter",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 32250);

        auto igCtrlCapture = std::make_shared<TestIgCtrlCapture>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_IG_CTRL_PACKET_ID_V4, igCtrlCapture.get());

        WHEN("Host flushes two TCP command-plane messages")
        {
            for (int i = 0; i < 2; ++i)
            {
                auto& tcp = hostA.outMsgWithIgCtrlTcp();
                CigiSymbolTextDefV4 cmd("reset");
                tcp << cmd;
                hostA.flushTcp();
            }
            // 只 tick IG 侧（不 tick Host 端）：避免 Host 数据面帧（outMsgWithIgCtrlUdp 的 IGCtrl）混入捕获。
            for (int i = 0; i < 5; ++i)
                engineB.tickSync();

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
// 9. 帧头去重（状态同步设计初版.md §7.1，2026-08-25）：
//    outMsgWithIgCtrlTcp/beginWithIgCtrlUdp（IG：beginWithSof/beginWithSofUdp）单次 flush 周期内
//    多次调用填充报文，帧头只填一次——接收端应恰好收到一个 IGCtrl（IG）或 SOF（Host）。
// =============================================================================

SCENARIO("Host can fill multiple packets in one message via repeated outMsgWithIgCtrlTcp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 32300);

        auto igCtrlCapture = std::make_shared<TestIgCtrlCapture>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_IG_CTRL_PACKET_ID_V4, igCtrlCapture.get());
        auto textProc = std::make_shared<TestTextProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_SYMBOL_TEXT_DEFINITION_PACKET_ID_V4, textProc.get());

        WHEN("Host calls outMsgWithIgCtrlTcp multiple times filling three packets, then flushes once")
        {
            // 三次调用填充三条报文，但只 flush 一次 → 应是一条消息一个 IGCtrl。
            auto& tcp = hostA.outMsgWithIgCtrlTcp();
            CigiSymbolTextDefV4 a("reset");
            tcp << a;
            auto& tcp2 = hostA.outMsgWithIgCtrlTcp();
            CigiSymbolTextDefV4 b("reset");
            tcp2 << b;
            auto& tcp3 = hostA.outMsgWithIgCtrlTcp();
            CigiSymbolTextDefV4 c("reset");
            tcp3 << c;
            hostA.flushTcp();

            for (int i = 0; i < 5; ++i)
                engineB.tickSync();

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

SCENARIO("Host UDP message carries exactly one IGCtrl across repeated outMsgWithIgCtrlUdp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 32350);

        auto igCtrlCapture = std::make_shared<TestIgCtrlCapture>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_IG_CTRL_PACKET_ID_V4, igCtrlCapture.get());

        WHEN("Host calls outMsgWithIgCtrlUdp multiple times filling packets, then flushes once")
        {
            // 同一消息内多次 begin 追加多个数据包（如实时位姿 + 眼点 + 额外报文），只 flush 一次。
            for (int i = 0; i < 3; ++i)
            {
                auto& udp = hostA.outMsgWithIgCtrlUdp();
                CigiEntityPositionCtrlV4 place;
                place.SetEntityID(7);
                place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
                place.SetLat(31.23);
                place.SetLon(121.47);
                place.SetAlt(500.0);
                udp << place;
            }
            hostA.flushUdp();
            // 只 tick IG 侧（不 tick Host 端）：避免 Host 数据面帧额外混入。
            for (int i = 0; i < 5; ++i)
                engineB.tickSync();

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

SCENARIO("IG can fill multiple packets in one message via repeated outMsgWithSofTcp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        HostSync hostA;
        setupHostIgPair(hostA, engineA, engineB, 32400);

        // Host 捕获 SOF：验证 IG 出站去重——一条消息恰好一个 SOF。
        auto hostSofCapture = std::make_shared<TestIgCtrlCapture>(); // 复用：捕获 SOF 的 FrameCntr
        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        hostA.registerEventProcessor(CIGI_IG_MSG_PACKET_ID_V4, hostMsgProc.get());
        // Host 侧基础设施 SofCaptureProc 已注册（双 session）——用 sofReceivedCount 验证只收到 1 条 SOF。

        WHEN("IG calls outMsgWithSofTcp multiple times filling three messages, then flushes once")
        {
            auto& tcp = engineB.synchronSystem().igSync().outMsgWithSofTcp();
            CigiIGMsgV4 a;
            a.SetMsgID(0x1001);
            a.SetMsg("a");
            tcp << a;
            auto& tcp2 = engineB.synchronSystem().igSync().outMsgWithSofTcp();
            CigiIGMsgV4 b;
            b.SetMsgID(0x1002);
            b.SetMsg("b");
            tcp2 << b;
            auto& tcp3 = engineB.synchronSystem().igSync().outMsgWithSofTcp();
            CigiIGMsgV4 c;
            c.SetMsgID(0x1003);
            c.SetMsg("c");
            tcp3 << c;
            engineB.synchronSystem().igSync().flushTcp();

            for (int i = 0; i < 20 && hostMsgProc->count() < 3; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                hostA.drainIncoming();
            }

            THEN("Host received all three IGMsgs with exactly one SOF header")
            {
                REQUIRE(hostMsgProc->count() == 3);
                // IG 出站去重后只有 1 个 SOF：三条报文在一条消息内，分帧器切出一条消息 → Host 收 1 个 SOF。
                REQUIRE(hostA.sofReceivedCount() == 1);
            }
        }
    }
}
