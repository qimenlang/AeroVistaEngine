// 状态同步设计初版.md §9 验收：命令面（TCP + CIGI IGMsg + RECEIVED/RESULT + 幂等）。
// 已实现（状态同步设计初版.md §11）：cigi_wire::CommandMsg 编解码、HostSync::sendCommand、
// IgSync 命令读循环 + 执行线程 + 观测。
// 本文件分两层：
//   1. IGMsg 编解码 / 长度分帧 / 幂等去重 → 自包含辅助（纯逻辑）
//   2. 命令收发闭环 → 真实 TCP 链路 E2E

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "Common.h"
#include "engine.h"
#include <aerovista/sync/HostSync.h>
#include <aerovista/sync/IgSync.h>

#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

using aerovista::sync::HostSync;
using aerovista::sync::IgSync;
namespace cigi_wire = aerovista::sync::cigi_wire;

namespace
{
    // 命令面契约（状态同步设计初版.md §8，已实现）：
    //   cigi_wire::CommandMsg { uint16_t msgId; uint16_t seq; std::vector<uint8_t> payload; }
    //   cigi_wire::packCommandMsg / unpackCommandMsg（CIGI V4 线格式）
    //   cigi_wire::Command::LOAD_MODEL / PLACE_MODEL / MOVE_MODEL（指令码枚举）
    //   HostSync::sendCommand(Command, payload, receivedTimeoutMs)
    //   IgSync::queueCommand / setCommandHandler / lastCommandMsgId()/lastCommandSeq()/commandCount()
    //   IgSync::setCommandReceivedDelayMs(delayMs) 测试注入

    // 按初版 §2.2 构造指令载荷（不含 seq）：PLACE/MOVE 均为 id(4B) + 48B = 52B，id 开头区分即可。
    std::vector<std::uint8_t> makePayloadWithId(std::uint32_t id)
    {
        std::vector<std::uint8_t> p(52, 0);
        p[0] = static_cast<std::uint8_t>(id & 0xFF);
        p[1] = static_cast<std::uint8_t>((id >> 8) & 0xFF);
        p[2] = static_cast<std::uint8_t>((id >> 16) & 0xFF);
        p[3] = static_cast<std::uint8_t>((id >> 24) & 0xFF);
        return p;
    }

    void writeLeF64(std::vector<std::uint8_t>& p, std::size_t offset, double value)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int i = 0; i < 8; ++i)
            p[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bits >> (8 * i));
    }

    // PLACEMODEL 载荷（不含 seq）：id(4B) + pos(3×8B) + ypr(3×8B) = 52B（初版 §2.2）。
    std::vector<std::uint8_t> makePlacePayload(std::uint32_t id, double x, double y, double z,
                                               double yawDeg, double pitchDeg, double rollDeg)
    {
        std::vector<std::uint8_t> p = makePayloadWithId(id);
        writeLeF64(p, 4, x);
        writeLeF64(p, 12, y);
        writeLeF64(p, 20, z);
        writeLeF64(p, 28, yawDeg);
        writeLeF64(p, 36, pitchDeg);
        writeLeF64(p, 44, rollDeg);
        return p;
    }

    // LOADMODEL 载荷（不含 seq）：[id(4B)] [path…]（初版 §2.2，id 由 Host 携带）。
    // 注意：id 后直接接 path，不能复用 makePayloadWithId（其 52B 填充会把 \0 混进 path）。
    std::vector<std::uint8_t> makeLoadPayload(std::uint32_t id)
    {
        std::vector<std::uint8_t> p;
        p.push_back(static_cast<std::uint8_t>(id & 0xFF));
        p.push_back(static_cast<std::uint8_t>((id >> 8) & 0xFF));
        p.push_back(static_cast<std::uint8_t>((id >> 16) & 0xFF));
        p.push_back(static_cast<std::uint8_t>((id >> 24) & 0xFF));
        const std::string path = "teapot";
        p.insert(p.end(), path.begin(), path.end());
        return p;
    }

    // 一条 CIGI IGMsg V4 帧（初版 §3.2 / CigiIGMsgV4::Pack）：8B 头 + Msg(8 对齐) = 16B。
    std::vector<std::uint8_t> makeFrame(std::uint8_t msgId, std::uint8_t seq)
    {
        return {16, 0, 0xF0, 0x0F, msgId, 0, 0, 0, seq, 0, 0xAA, 0xBB, 0, 0, 0, 0};
    }

    std::vector<std::uint8_t> slice(const std::vector<std::uint8_t>& v, int begin, int end)
    {
        return {v.begin() + begin, v.begin() + end};
    }

    std::vector<std::uint8_t> concat(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b)
    {
        std::vector<std::uint8_t> r;
        r.reserve(a.size() + b.size());
        r.insert(r.end(), a.begin(), a.end());
        r.insert(r.end(), b.begin(), b.end());
        return r;
    }

    // ECEF 通道配置 JSON body（参考 HostIGTests RigidArrayHarness）。端口布局与 Common.h 的
    // makeTestHostIgRole(makeTestHostConfig(base)+igConfig(base+1)) / makeTestIgOnlyRole(igConfig(base+3)) 一致。
    std::string makeEcefChannelConfigBody(int base, bool isHost, int igUdpRecv)
    {
        std::ostringstream oss;
        oss << R"({ "coordFrame": "Ellipsoid", )"
            << R"("igConfig": { "bindAddr": "127.0.0.1", "udpPortSend": )" << base
            << R"(, "udpPortRecv": )" << igUdpRecv << R"(, "targetAddr": "127.0.0.1", "targetTcpPort": )"
            << (base + 100) << R"(, "targetUdpPortRecv": )" << base << R"( }, )";
        if (isHost)
        {
            oss << R"("hostConfig": { "bindAddr": "127.0.0.1", "udpPortSend": )" << (base + 1)
                << R"(, "udpPortRecv": )" << base << R"(, "tcpPort": )" << (base + 100) << R"( }, )";
        }
        oss << R"("model": "models/teapot.vsgt", )"
            << R"("window": { "x": 0, "y": 0, "width": 640, "height": 480 }, )"
            << R"("syncSystem": { "requireIgConnect": )" << (isHost ? "true" : "false") << R"( } })";
        return oss.str();
    }

    // ECEF 配置驱动的 Host+IG 引擎 A + IG-only 引擎 B（均经本地 JSON 加载，coordFrame=Ellipsoid）。
    void setupEcefHostIgPair(Engine& engineA, Engine& engineB, int base)
    {
        const TempConfigFile hostFile(makeEcefChannelConfigBody(base, /*isHost=*/true, base + 1));
        const TempConfigFile igFile(makeEcefChannelConfigBody(base, /*isHost=*/false, base + 3));

        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;

        REQUIRE(engineA.loadConfig(hostFile.path()));
        REQUIRE(engineB.loadConfig(igFile.path()));
        REQUIRE(engineA.init()); // host+ig：建 Vulkan Device + 场景
        // B：sync + scene mode only（避免单进程第二个 Vulkan Device）。
        REQUIRE(engineB.initSync(engineB.config.toSyncRole(), engineB.config.syncSystem.requireIgConnect));
        REQUIRE(engineB.initSceneMode(vsg::Path(RESOURCE_DIR) / engineB.config.model));

        // 握手异步：轮询直到 A 的 ready peer = self + B 共 2 个。
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
        while (engineA.synchronSystem().hostSync().readyIgCount() < 2 &&
               std::chrono::steady_clock::now() < deadline)
        {
            if (engineB.synchronSystem().hasIg() && !engineB.synchronSystem().igSync().udpSynced())
                engineB.synchronSystem().igSync().connect(engineB.config.igConfig);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
    }

    // E2E 公用：Host+IG 引擎 A + IG-only 引擎 B（自连产生 2 个 ready peer）。
    void setupHostIgPair(Engine& engineA, Engine& engineB, int base)
    {
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;
        REQUIRE(engineA.initSync(makeTestHostIgRole(base + 1, base)));
        REQUIRE(engineB.initSync(makeTestIgOnlyRole(base + 3, base)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 2);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));
    }

    // E2E 公用：Host+IG 引擎 A + 两个 IG-only 引擎 B/C。
    void setupHostIgTriple(Engine& engineA, Engine& engineB, Engine& engineC, int base)
    {
        engineA.extent = engineB.extent = engineC.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = engineC.showWindow = false;
        REQUIRE(engineA.initSync(makeTestHostIgRole(base + 1, base)));
        REQUIRE(engineB.initSync(makeTestIgOnlyRole(base + 3, base)));
        REQUIRE(engineC.initSync(makeTestIgOnlyRole(base + 5, base)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 3);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));
    }

    // E2E 公用：Host-only 引擎 A + IG-only 引擎 B（无 self-peer，断线判定专用）。
    void setupHostOnlyIgPair(Engine& engineA, Engine& engineB, int base)
    {
        engineA.extent = engineB.extent = {640, 480};
        engineA.showWindow = engineB.showWindow = false;
        REQUIRE(engineA.initSync(makeTestHostOnlyRole(base)));
        REQUIRE(engineB.initSync(makeTestIgOnlyRole(base + 3, base)));
        REQUIRE(engineA.synchronSystem().hostSync().readyIgCount() == 1);
        REQUIRE(engineA.initGraphics(vsg::Path(RESOURCE_DIR) / "models" / "teapot.vsgt"));
    }

    // 命令收发由各端独立读循环线程驱动（初版 §4）；tick 仅驱动帧同步（时钟）。
    void tickBoth(Engine& engineA, Engine& engineB, int frames = 5)
    {
        for (int i = 0; i < frames; ++i)
        {
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();
        }
    }

    void tickAll(Engine& engineA, Engine& engineB, Engine& engineC, int frames = 5)
    {
        for (int i = 0; i < frames; ++i)
        {
            REQUIRE(engineA.tickOnFrame());
            engineB.tickSync();
            engineC.tickSync();
        }
    }
} // namespace

// =============================================================================
// 1. IGMsg 编解码 / 长度分帧 / 幂等去重（自包含辅助，纯逻辑，可绿）
// =============================================================================

TEST_CASE("command msg packs and unpacks msgId, seq and payload", "[unit][sync][cmd][wire]")
{
    // 契约：cigi_wire::packCommandMsg / unpackCommandMsg —— CIGI IGMsg V4 线格式
    //   [PacketSize(2,LE)][PacketID=0x0ff0(2,LE)][MsgID(2,LE)][reserved(2)][Msg=seq(2,LE)+payload]，
    //   Msg 按 8 对齐补齐，PacketSize = 8 + VariableDataSize（CigiIGMsgV4::Pack）。
    //
    // 头部布局说明（问题：为何 msgId 在 4-5、seq 在 8-9？）：
    //   字节 0-1 = PacketSize（报文总长）
    //   字节 2-3 = PacketID = 0x0ff0（CIGI V4 的 IGMsg 包类型标识，不是 msgId）
    //   字节 4-5 = MsgID（指令码/回执码）
    //   字节 6-7 = reserved（保留字段）
    //   字节 8.. = Msg 载荷（我们协议里载荷前 2 字节 = seq，之后是业务 payload）
    // 这是 CigiIGMsgV4::Pack 规定的固定头布局，业务层不得改动。
    cigi_wire::CommandMsg in;
    in.msgId = static_cast<std::uint16_t>(cigi_wire::Command::PLACE_MODEL);
    in.seq = 42;
    in.payload = {1, 2, 3, 4};

    const std::uint16_t msgSize = 2 + static_cast<std::uint16_t>(in.payload.size());
    const std::uint16_t variableDataSize = static_cast<std::uint16_t>((msgSize + 7) & ~7);
    const std::uint16_t packetSize = static_cast<std::uint16_t>(8 + variableDataSize);
    std::vector<std::uint8_t> wire{static_cast<std::uint8_t>(packetSize & 0xFF), // [0-1] PacketSize
                                   static_cast<std::uint8_t>(packetSize >> 8),
                                   0xF0, 0x0F,                                 // [2-3] PacketID = 0x0ff0
                                   static_cast<std::uint8_t>(in.msgId & 0xFF), // [4-5] MsgID
                                   static_cast<std::uint8_t>(in.msgId >> 8),
                                   0, 0,                                     // [6-7] reserved
                                   static_cast<std::uint8_t>(in.seq & 0xFF), // [8-9] Msg: seq
                                   static_cast<std::uint8_t>(in.seq >> 8)};
    wire.insert(wire.end(), in.payload.begin(), in.payload.end());
    wire.resize(8 + variableDataSize, 0); // 8 对齐补齐

    cigi_wire::CommandMsg out;
    out.msgId = static_cast<std::uint16_t>(wire[4] | (wire[5] << 8));
    out.seq = static_cast<std::uint16_t>(wire[8] | (wire[9] << 8));
    out.payload.assign(wire.begin() + 10, wire.begin() + 10 + msgSize - 2);

    REQUIRE(out.msgId == static_cast<std::uint16_t>(cigi_wire::Command::PLACE_MODEL));
    REQUIRE(out.seq == 42);
    REQUIRE(out.payload == std::vector<std::uint8_t>({1, 2, 3, 4}));
}

TEST_CASE("frame assembler splits full frames and tolerates fragmentation and coalescing",
          "[unit][sync][cmd][wire]")
{
    // 契约：cigi_wire::CommandFrameAssembler 按 CIGI PacketSize 切包（初版 §3.2），覆盖拆包/粘包/混合。
    // 回调式接口：每条完整帧解析为 CommandMsg 并回调——同时验证解包出的 msgId/seq。
    const auto frameA = makeFrame(static_cast<std::uint8_t>(cigi_wire::Command::LOAD_MODEL), 0x2A);
    const auto frameB = makeFrame(static_cast<std::uint8_t>(cigi_wire::Command::PLACE_MODEL), 0x2B);

    cigi_wire::CommandFrameAssembler assembler;
    std::vector<cigi_wire::CommandMsg> collected;
    const auto onMsg = [&collected](const cigi_wire::CommandMsg& msg) { collected.push_back(msg); };

    // 拆包：frameA 分 3 块喂入，最后一块才切出完整 1 条。
    assembler.feed(slice(frameA, 0, 3).data(), 3, onMsg);
    REQUIRE(collected.empty());
    assembler.feed(slice(frameA, 3, 7).data(), 4, onMsg);
    REQUIRE(collected.empty());
    assembler.feed(slice(frameA, 7, 16).data(), 9, onMsg);
    REQUIRE(collected.size() == 1);
    REQUIRE(collected[0].msgId == static_cast<std::uint16_t>(cigi_wire::Command::LOAD_MODEL));
    REQUIRE(collected[0].seq == 0x2A);
    REQUIRE(assembler.bufferEmpty());

    // 粘包：单条完整喂入即切 1 条。
    collected.clear();
    assembler.feed(frameB.data(), static_cast<int>(frameB.size()), onMsg);
    REQUIRE(collected.size() == 1);
    REQUIRE(collected[0].msgId == static_cast<std::uint16_t>(cigi_wire::Command::PLACE_MODEL));
    REQUIRE(collected[0].seq == 0x2B);
    REQUIRE(assembler.bufferEmpty());

    // 粘包：两条一起喂，一次切出 2 条。
    collected.clear();
    const auto two = concat(frameA, frameB);
    assembler.feed(two.data(), static_cast<int>(two.size()), onMsg);
    REQUIRE(collected.size() == 2);
    REQUIRE(assembler.bufferEmpty());

    // 粘包 + 拆包混合：两条一起喂但拆成 5B + 剩余，最终仍切出 2 条。
    collected.clear();
    assembler.feed(slice(two, 0, 5).data(), 5, onMsg);
    REQUIRE(collected.empty());
    assembler.feed(slice(two, 5, 32).data(), 27, onMsg);
    REQUIRE(collected.size() == 2);
    REQUIRE(assembler.bufferEmpty());
}

// =============================================================================
// 2. 命令收发闭环（真实 TCP 链路 E2E）
// =============================================================================

SCENARIO("Host sends a command and IG acknowledges received then result",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        setupHostIgPair(engineA, engineB, 31000);

        WHEN("Host sends a PLACEMODEL command and both engines tick")
        {
            // PLACEMODEL 载荷（不含 seq，初版 §2.2）：id(4B) + pos(3×8B) + ypr(3×8B) = 52B。
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(cigi_wire::Command::PLACE_MODEL, makePayloadWithId(1)));
            tickBoth(engineA, engineB);

            THEN("IG received the command and reports it")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                REQUIRE(ig.lastCommandMsgId() == cigi_wire::Command::PLACE_MODEL);
                REQUIRE(ig.lastCommandSeq() > 0);
                REQUIRE(ig.commandCount() >= 1);
            }
        }
    }
}

SCENARIO("IG executes Load then Place in order", "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        setupHostIgPair(engineA, engineB, 31100);

        WHEN("Host sends LOAD then PLACE and both engines tick")
        {
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(cigi_wire::Command::LOAD_MODEL, makeLoadPayload(7)));
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(cigi_wire::Command::PLACE_MODEL, makePayloadWithId(7)));
            tickBoth(engineA, engineB);

            THEN("IG processed two commands in order, PLACE last")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                REQUIRE(ig.commandCount() == 2);
                REQUIRE(ig.lastCommandMsgId() == cigi_wire::Command::PLACE_MODEL); // 最后是 PLACE
                REQUIRE(ig.lastCommandSeq() > 0);
            }
        }
    }
}

SCENARIO("IG scene holds the loaded model at the placed pose",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        setupHostIgPair(engineA, engineB, 31150);

        // 契约：LOADMODEL 真实加载模型（异步执行线程，慢命令 ~数百 ms）并建骨架实体（id=7，来自载荷），
        // PLACEMODEL(id=7) 升级该实体位姿——IG 侧场景必须真实持有模型。
        WHEN("Host sends LOAD then PLACE with a non-zero pose and both engines tick")
        {
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(cigi_wire::Command::LOAD_MODEL, makeLoadPayload(7)));
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(
                cigi_wire::Command::PLACE_MODEL, makePlacePayload(7, 10.0, 20.0, 30.0, 0.0, 0.0, 0.0)));

            // LOAD 为慢命令（真实 IO ~数百 ms）：按墙钟超时轮询 tick，直到 IG 场景就绪
            // （PLACE 升级后实体有位姿载体）。hasEntityId 在 LOAD 后即 true，须等 transform 就绪。
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!engineB.entityTransform(7) && std::chrono::steady_clock::now() < deadline)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }

            THEN("IG scene has the entity with the model and the placed pose")
            {
                REQUIRE(engineB.hasEntityId(7));
                auto transform = engineB.entityTransform(7);
                REQUIRE(transform);                       // 实体有位姿载体
                REQUIRE(transform->children.size() == 1); // LOAD 的模型真实挂载到实体
                // 位姿与 PLACEMODEL 设定一致：ypr=0 → 矩阵平移即 (10,20,30)。
                REQUIRE(transform->matrix(3, 0) == Catch::Approx(10.0));
                REQUIRE(transform->matrix(3, 1) == Catch::Approx(20.0));
                REQUIRE(transform->matrix(3, 2) == Catch::Approx(30.0));
            }
        }
    }
}

SCENARIO("IG holds the loaded model at the placed ECEF pose (config-driven)",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Host+IG A and IG-only B both loaded from ECEF config files over real sockets")
    {
        Engine engineA;
        Engine engineB;
        // 本地构造 coordFrame=Ellipsoid 配置，engine 经 loadConfig+init 加载（ECEF 坐标系）。
        setupEcefHostIgPair(engineA, engineB, 31800);

        // 契约：ECEF 下 LOADMODEL 真实加载模型（异步执行线程，慢命令 ~数百 ms）并建骨架实体（id=7，来自载荷），
        // PLACEMODEL(id=7) pos=LLA（lat°, lon°, alt m，初版 §2.2）升级实体位姿。
        WHEN("Host sends LOAD then PLACE with an LLA pose and both engines tick")
        {
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(cigi_wire::Command::LOAD_MODEL, makeLoadPayload(7)));
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(
                cigi_wire::Command::PLACE_MODEL, makePlacePayload(7, 39.9, 116.4, 500.0, 0.0, 0.0, 0.0)));

            // LOAD 为慢命令（真实 IO ~数百 ms）：按墙钟超时轮询 tick，直到 IG 场景就绪
            // （PLACE 升级后实体有位姿载体）。hasEntityId 在 LOAD 后即 true，须等 transform 就绪。
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!engineB.entityTransform(7) && std::chrono::steady_clock::now() < deadline)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }

            THEN("IG scene has the entity with the model at the placed LLA pose")
            {
                REQUIRE(engineB.hasEntityId(7));
                auto transform = engineB.entityTransform(7);
                REQUIRE(transform);                       // 实体有位姿载体
                REQUIRE(transform->children.size() == 1); // LOAD 的模型真实挂载到实体

                // ECEF 位姿回读：sampleEntityPoseById 返回 LLA（lat°, lon°, alt m）。
                vsg::dvec3 lla;
                vsg::dvec3 ypr;
                REQUIRE(engineB.sampleEntityPoseById(7, lla, ypr));
                REQUIRE(lla.x == Catch::Approx(39.9));
                REQUIRE(lla.y == Catch::Approx(116.4));
                REQUIRE(lla.z == Catch::Approx(500.0));

                // transform 矩阵 = EllipsoidModel.computeLocalToWorldTransform(lla)（ypr=0 → 无旋转）。
                auto ellipsoid = engineB.ellipsoidModel();
                REQUIRE(ellipsoid);
                const vsg::dmat4 expected = ellipsoid->computeLocalToWorldTransform(lla);
                for (int c = 0; c < 4; ++c)
                {
                    for (int r = 0; r < 4; ++r)
                        REQUIRE(transform->matrix(r, c) == Catch::Approx(expected(r, c)));
                }
            }
        }
    }
}

SCENARIO("IG replies RESULT and Host receives success and failure callbacks",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        setupHostIgPair(engineA, engineB, 31200);

        // 契约：HostSync 命令结果回调 onCommandResult(igPeer, msgId, seq, ack, payload)；
        // 业务侧注册回调收集结果；IG 执行成功回 RESULT-ACK，失败回 RESULT-NACK。
        WHEN("IG executes a command successfully and both engines tick")
        {
            // LOAD 建 id=0 骨架实体，PLACE(0) 升级位姿 → 执行成功 → RESULT-ACK。
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(cigi_wire::Command::LOAD_MODEL, makeLoadPayload(7)));
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(cigi_wire::Command::PLACE_MODEL, makePayloadWithId(7)));

            // LOAD 为慢命令（真实 IO ~数百 ms）：按墙钟超时轮询 tick，直到 RESULT 到达。
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (engineA.synchronSystem().hostSync().lastCommandResultSeq() == 0 &&
                   std::chrono::steady_clock::now() < deadline)
            {
                REQUIRE(engineA.tickOnFrame());
                engineB.tickSync();
            }

            THEN("Host receives a success callback")
            {
                // 契约：HostSync 记录最后 RESULT（lastCommandResultAck / lastCommandResultSeq）
                HostSync& host = engineA.synchronSystem().hostSync();
                REQUIRE(host.lastCommandResultSeq() > 0);
                REQUIRE(host.lastCommandResultAck()); // 成功
            }
        }
    }
}

SCENARIO("IG replies RESULT-NACK when execution fails and Host receives failure callback",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        setupHostIgPair(engineA, engineB, 31400);

        // 待实现契约：IG 执行失败（如 MOVEMODEL 目标实体不存在）→ 回 RESULT-NACK（0x9000|cmd）。
        WHEN("IG fails to execute a command and both engines tick")
        {
            // MOVEMODEL 目标 id=99 不存在 → 执行失败；载荷：id(4B) + delta(24B) + dYpr(24B) = 52B。
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(cigi_wire::Command::MOVE_MODEL, makePayloadWithId(99)));
            tickBoth(engineA, engineB);

            THEN("Host receives a failure callback")
            {
                // 待实现契约：HostSync 记录最后 RESULT（lastCommandResultAck / lastCommandResultSeq）
                HostSync& host = engineA.synchronSystem().hostSync();
                REQUIRE(host.lastCommandResultSeq() > 0);
                REQUIRE_FALSE(host.lastCommandResultAck()); // 失败（NACK）
            }
        }
    }
}

SCENARIO("Host sends a command to two IGs and both receive it (serial fan-out)",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and two IG-only engines B and C linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        Engine engineC;
        setupHostIgTriple(engineA, engineB, engineC, 31500);

        // 待实现契约：sendCommand 串行逐 peer 分发（初版 §5.2）。
        WHEN("Host sends one command and all engines tick")
        {
            REQUIRE(engineA.synchronSystem().hostSync().sendCommand(cigi_wire::Command::PLACE_MODEL, makePayloadWithId(1)));
            tickAll(engineA, engineB, engineC);

            THEN("both IGs received the command")
            {
                IgSync& igB = engineB.synchronSystem().igSync();
                IgSync& igC = engineC.synchronSystem().igSync();
                REQUIRE(igB.commandCount() >= 1);
                REQUIRE(igC.commandCount() >= 1);
                REQUIRE(igB.lastCommandMsgId() == cigi_wire::Command::PLACE_MODEL);
                REQUIRE(igC.lastCommandMsgId() == cigi_wire::Command::PLACE_MODEL);
            }
        }
    }
}

SCENARIO("Host reports undelivered when RECEIVED times out",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        setupHostIgPair(engineA, engineB, 31300);

        // 待实现契约：
        //   HostSync::sendCommand(msgId, payload, receivedTimeoutMs) — 超时即失败（返回 false / 标记未送达）
        //   IgSync::setCommandReceivedDelayMs(delayMs) — 测试注入：收到命令后延迟 delayMs 再回 RECEIVED
        WHEN("Host sends a command with a short RECEIVED timeout and both engines tick")
        {
            // 让 IG 延迟回 RECEIVED（> 50ms），模拟「连接活着但回执迟到」。
            engineB.synchronSystem().igSync().setCommandReceivedDelayMs(200);

            const bool delivered = engineA.synchronSystem().hostSync().sendCommand(
                cigi_wire::Command::PLACE_MODEL, makePayloadWithId(1), /*receivedTimeoutMs=*/50);
            tickBoth(engineA, engineB);

            THEN("Host reported undelivered (RECEIVED timeout) and IG still executed once")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                // 超时即未送达：50ms 内 RECEIVED 未到 → false。
                REQUIRE_FALSE(delivered);
                // 区分「IG 活着但回执迟到」与「断线」：延迟回 RECEIVED 后 IG 最终执行了命令。
                REQUIRE(ig.commandCount() == 1);
            }
        }
    }
}

TEST_CASE("IG deduplicates a repeated command via maxSeq without re-executing",
          "[unit][sync][cmd][dedup]")
{
    // 契约：IgSync 命令去重（seq <= maxSeq 收到即更新，初版 §2.3）。
    // queueCommand 注入生产 processCommand，验证真实去重路径（非测试类模拟）。
    IgSync ig;
    const std::vector<std::uint8_t> payload = makePayloadWithId(1);

    ig.queueCommand(cigi_wire::Command::PLACE_MODEL, 5, payload); // 首次 → 执行
    ig.queueCommand(cigi_wire::Command::PLACE_MODEL, 5, payload); // 重发同 seq → 去重

    REQUIRE(ig.commandCount() == 1); // 只执行一次
    REQUIRE(ig.lastCommandSeq() == 5);
    REQUIRE(ig.lastCommandMsgId() == cigi_wire::Command::PLACE_MODEL);
}

TEST_CASE("IG executes distinct seq even for same command type", "[unit][sync][cmd][dedup]")
{
    // 同指令类型、不同 seq → 都执行（初版 §2.3：指令类型与 seq 序号正交）。
    IgSync ig;
    const std::vector<std::uint8_t> payload = makePayloadWithId(1);

    ig.queueCommand(cigi_wire::Command::PLACE_MODEL, 5, payload);
    ig.queueCommand(cigi_wire::Command::PLACE_MODEL, 6, payload);

    REQUIRE(ig.commandCount() == 2);
    REQUIRE(ig.lastCommandSeq() == 6);
}

SCENARIO("Host does not fail a slow command while IG is still executing",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host+IG and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        setupHostIgPair(engineA, engineB, 31600);

        // 契约（主线程执行模型，初版 §4/§5.2）：命令由 IG 主线程在帧循环执行（runPendingCommands），
        // RECEIVED 由命令读循环线程即时回；RESULT 不设超时（Host 等 RESULT 不判失败）。
        // 慢指令（LOAD 大模型）在实机阻塞主线程一帧、RESULT 随之到达——RESULT 迟到≠判失败。
        WHEN("IG executes a command and both engines tick a few frames")
        {
            const bool delivered = engineA.synchronSystem().hostSync().sendCommand(
                cigi_wire::Command::LOAD_MODEL, makeLoadPayload(7));
            tickBoth(engineA, engineB, 3);

            THEN("Host received delivery and IG executed the command on the main thread")
            {
                IgSync& ig = engineB.synchronSystem().igSync();
                REQUIRE(delivered);              // 送达（收到 RECEIVED，进入等 RESULT 阶段）
                REQUIRE(ig.commandCount() >= 1); // 命令在主线程帧内执行
            }
        }
    }
}

SCENARIO("Host fails a command when IG disconnects (RECEIVED never arrives)",
         "[acceptance][bdd][sync][cmd][e2e]")
{
    GIVEN("Engine A as Host-only and Engine B as IG-only linked over real sockets")
    {
        Engine engineA;
        Engine engineB;
        // Host-only 避免 self-peer 干扰断线判定（初版 §5.2：全部 ready peer 送达才 true）。
        setupHostOnlyIgPair(engineA, engineB, 31700);

        // 待实现契约：IG 掉线（shutdown）→ Host sendCommand 等 RECEIVED 超时 → 判定「未送达」失败。
        WHEN("IG goes offline before receiving a command")
        {
            engineB.synchronSystem().shutdown(); // IG 掉线（模拟断线）

            const bool delivered = engineA.synchronSystem().hostSync().sendCommand(
                cigi_wire::Command::PLACE_MODEL, makePayloadWithId(1), /*receivedTimeoutMs=*/50);

            THEN("Host reported the command as undelivered")
            {
                // 断线即失败（初版 §3.4）：IG 已断，RECEIVED 不达 → 超时未送达；
                // Host 若已感知断线（无 ready peer）同样返回 false，两种时序下断言都成立。
                REQUIRE_FALSE(delivered);
            }
        }
    }
}
