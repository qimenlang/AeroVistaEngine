// 状态同步设计初版.md §10 验收（新契约：引用式发送 + 无业务回执 + CCL 标准报文 + processor）。
// 命令面全绿：2 线格式契约（直接测 CCL）+ 6 E2E + 4 分帧单测。

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
#include "CigiBaseEntityPositionCtrl.h"
#include "CigiBaseEventProcessor.h"
#include "CigiCollDetSegDefV4.h"
#include "CigiCollDetSegRespV4.h"
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
    // Host+IG 引擎 A + IG-only 引擎 B（自连产生 2 个 ready peer）。
    void setupHostIgPair(Engine& a, Engine& b, int base)
    {
        a.extent = b.extent = {640, 480};
        a.showWindow = b.showWindow = false;
        REQUIRE(a.initSync(makeTestHostIgRole(base + 1, base)));
        REQUIRE(b.initSync(makeTestIgOnlyRole(base + 3, base)));
        REQUIRE(a.hostSync().readyIgCount() == 2);
        REQUIRE(a.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));
    }

    // Host+IG 引擎 A + 两个 IG-only 引擎 B/C（自连产生 3 个 ready peer）。
    void setupHostIgTriple(Engine& a, Engine& b, Engine& c, int base)
    {
        a.extent = b.extent = c.extent = {640, 480};
        a.showWindow = b.showWindow = c.showWindow = false;
        REQUIRE(a.initSync(makeTestHostIgRole(base + 1, base)));
        REQUIRE(b.initSync(makeTestIgOnlyRole(base + 3, base)));
        REQUIRE(c.initSync(makeTestIgOnlyRole(base + 5, base)));
        REQUIRE(a.hostSync().readyIgCount() == 3);
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
// 2. E2E：真实 socket 收发（tcpOutgoing/flushTcp/registerEventProcessor）
// =============================================================================

SCENARIO("Host places an entity pose over TCP via tcpOutgoing/flushTcp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        setupHostIgPair(engineA, engineB, 31000);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, placeProc.get());

        WHEN("Host assembles EntityPositionCtrlV4 and flushes TCP")
        {
            auto& tcp = engineA.hostSync().tcpOutgoing();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            tcp << place;
            engineA.hostSync().flushTcp();
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
        setupHostIgPair(engineA, engineB, 31150);

        auto textProc = std::make_shared<TestTextProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_SYMBOL_TEXT_DEFINITION_PACKET_ID_V4, textProc.get());

        WHEN("Host assembles SymbolTextDefV4 and flushes TCP")
        {
            auto& tcp = engineA.hostSync().tcpOutgoing();
            CigiSymbolTextDefV4 cmd("place 7 121.47 31.23 500");
            tcp << cmd;
            engineA.hostSync().flushTcp();
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
        setupHostIgTriple(engineA, engineB, engineC, 31300);

        auto procB = std::make_shared<TestPlaceProcessor>();
        auto procC = std::make_shared<TestPlaceProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, procB.get());
        engineC.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, procC.get());

        WHEN("Host flushes one EntityPositionCtrlV4 to all ready IGs")
        {
            auto& tcp = engineA.hostSync().tcpOutgoing();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            tcp << place;
            engineA.hostSync().flushTcp();
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
        setupHostIgPair(engineA, engineB, 31200);

        auto textProc = std::make_shared<TestTextProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_SYMBOL_TEXT_DEFINITION_PACKET_ID_V4, textProc.get());

        WHEN("Host sends two different text commands over TCP")
        {
            CigiSymbolTextDefV4 place("place 7 121.47 31.23 500");
            CigiSymbolTextDefV4 reset("reset");
            {
                auto& tcp = engineA.hostSync().tcpOutgoing();
                tcp << place;
                engineA.hostSync().flushTcp();
            }
            {
                auto& tcp = engineA.hostSync().tcpOutgoing();
                tcp << reset;
                engineA.hostSync().flushTcp();
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
        setupHostIgPair(engineA, engineB, 31400);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        auto textProc = std::make_shared<TestTextProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, placeProc.get());
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_SYMBOL_TEXT_DEFINITION_PACKET_ID_V4, textProc.get());

        WHEN("Host assembles two packets into one TCP message and flushes once")
        {
            auto& tcp = engineA.hostSync().tcpOutgoing();
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            CigiSymbolTextDefV4 cmd("reset");
            tcp << place << cmd; // 批量两个报文 = 一条消息
            engineA.hostSync().flushTcp();
            tickBoth(engineA, engineB);

            THEN("both processors were triggered")
            {
                REQUIRE(placeProc->received);
                REQUIRE(textProc->count() > 0);
            }
        }
    }
}

SCENARIO("Host streams real-time entity pose over UDP via udpOutgoing/flushUdp",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        setupHostIgPair(engineA, engineB, 31500);

        auto placeProc = std::make_shared<TestPlaceProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4, placeProc.get());

        WHEN("Host assembles EntityPositionCtrlV4 and flushes UDP each frame")
        {
            // 矛盾 A：udpOutgoing 不再自动前置 IGCtrl，业务侧组装完整消息（IGCtrl + 命令报文）。
            auto& udp = engineA.hostSync().udpOutgoing();
            CigiIGCtrlV4 igCtrl;
            udp << igCtrl;
            CigiEntityPositionCtrlV4 place;
            place.SetEntityID(7);
            place.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
            place.SetLat(31.23);
            place.SetLon(121.47);
            place.SetAlt(500.0);
            udp << place;
            engineA.hostSync().flushUdp();
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
        setupHostIgPair(engineA, engineB, 31600);

        // Host 侧对等注册：处理 IG 发来的 CigiIGMsgV4（CCL 原生 IG→Host 报文，§8.1 对等）。
        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        engineA.hostSync().registerEventProcessor(
            CIGI_IG_MSG_PACKET_ID_V4, hostMsgProc.get());

        WHEN("IG assembles CigiIGMsgV4 and flushes TCP")
        {
            auto& tcp = engineB.synchronSystem().igSync().tcpOutgoing();
            CigiIGMsgV4 status;
            status.SetMsgID(0x1001);
            status.SetMsg("status ok");
            tcp << status;
            engineB.synchronSystem().igSync().flushTcp();

            // Host 收包为 push 模式：等待 peer 线程收包分帧入队后，主线程 drain 解包。
            for (int i = 0; i < 20 && hostMsgProc->count() == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                engineA.hostSync().drainIncoming();
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
        setupHostIgPair(engineA, engineB, 31700);

        // 双向注册：IG 处理 Host 发来的 CollDetSegDefV4；Host 处理 IG 回发的 CollDetSegRespV4。
        auto igDefProc = std::make_shared<TestCollDetSegDefProcessor>();
        engineB.synchronSystem().igSync().registerEventProcessor(
            CIGI_COLL_DET_SEG_DEF_PACKET_ID_V4, igDefProc.get());
        auto hostRespProc = std::make_shared<TestCollDetSegRespProcessor>();
        engineA.hostSync().registerEventProcessor(
            CIGI_COLL_DET_SEG_RESP_PACKET_ID_V4, hostRespProc.get());

        WHEN("Host sends CollDetSegDefV4 over TCP, IG processes and replies CollDetSegRespV4")
        {
            // Host → IG：碰撞检测段定义。
            {
                auto& tcp = engineA.hostSync().tcpOutgoing();
                CigiCollDetSegDefV4 def;
                def.SetEntityID(7);
                def.SetSegmentEn(true);
                def.SetMask(0x00FF00FFu);
                tcp << def;
                engineA.hostSync().flushTcp();
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
                auto& tcp = engineB.synchronSystem().igSync().tcpOutgoing();
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
                engineA.hostSync().drainIncoming();
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
        setupHostIgPair(engineA, engineB, 31800);

        // Host 对等注册：处理 IG 经 UDP 发来的 CigiIGMsgV4（§8.1 对等，收发均支持 TCP/UDP）。
        auto hostMsgProc = std::make_shared<TestIgMsgProcessor>();
        engineA.hostSync().registerEventProcessor(
            CIGI_IG_MSG_PACKET_ID_V4, hostMsgProc.get());

        WHEN("IG assembles CigiIGMsgV4 and flushes UDP")
        {
            auto& udp = engineB.synchronSystem().igSync().udpOutgoing();
            CigiIGMsgV4 status;
            status.SetMsgID(0x2001);
            status.SetMsg("udp status ok");
            udp << status;
            engineB.synchronSystem().igSync().flushUdp();

            // Host push 模式：等待 I/O 线程收包入队（UDP 非阻塞 1ms 轮询）后，主线程 drain 解包。
            for (int i = 0; i < 5 && hostMsgProc->count() == 0; ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                engineA.hostSync().drainIncoming();
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
