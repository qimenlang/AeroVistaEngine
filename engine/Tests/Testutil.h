#pragma once

// 测试辅助：临时配置、端口装配、一对已握手的 Host 与 IG。

#include <catch2/catch_test_macros.hpp>

#include <aerovista/config/EngineChannelConfig.h>
#include <aerovista/sync/CigiWire.h>
#include <aerovista/sync/HostSync.h>
#include <aerovista/sync/IgSync.h>
#include <aerovista/sync/SyncConfig.h>

#include "CigiBaseEntityCtrl.h"
#include "CigiBaseEntityPositionCtrl.h"
#include "CigiBaseEventProcessor.h"
#include "CigiCollDetSegDefV4.h"
#include "CigiCollDetSegRespV4.h"
#include "CigiEntityCtrlV4.h"
#include "CigiEntityPositionCtrlV4.h"
#include "CigiIGCtrlV4.h"
#include "CigiIGMsgV4.h"
#include "CigiSymbolTextDefV4.h"
#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using aerovista::config::ViewDefinition;
using aerovista::sync::HostConfig;
using aerovista::sync::HostSync;
using aerovista::sync::IgConfig;
using aerovista::sync::IgSync;

class TempConfigFile
{
public:
    explicit TempConfigFile(const std::string& jsonBody)
    {
        _path = (std::filesystem::temp_directory_path() /
                 ("ave_engine_cfg_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".json"))
                    .string();
        std::ofstream out(_path, std::ios::binary);
        out << jsonBody;
    }

    ~TempConfigFile()
    {
        std::error_code ec;
        std::filesystem::remove(_path, ec);
    }

    const std::string& path() const { return _path; }

private:
    std::string _path;
};

/// 写独立 entities.json + 指向它的 engine 通道配置（实体目录抽取后，测试装配用）。
/// 两个临时文件随对象析构清理；路径反斜杠转正斜杠嵌入 JSON，避免转义。
/// `extraFields` 为额外对象成员（不含前导逗号），例如 `"igConfig": { ... }`。
struct EntitiesConfig
{
    EntitiesConfig(const std::string& entitiesArrayBody, const std::string& cameraObject = {},
                   bool injectEllipsoidIfMissing = false, const std::string& extraFields = {})
    {
        entitiesFile = std::make_unique<TempConfigFile>(std::string(R"({ "entities": )") + entitiesArrayBody + " }");

        std::string cfg = "{";
        if (injectEllipsoidIfMissing)
            cfg += R"("injectEllipsoidIfMissing": true,)";
        std::string entitiesPath = entitiesFile->path();
        std::replace(entitiesPath.begin(), entitiesPath.end(), '\\', '/');
        cfg += R"("entitiesFilePath": ")" + entitiesPath + "\"";
        if (!cameraObject.empty())
            cfg += R"(, "camera": )" + cameraObject;
        if (!extraFields.empty())
            cfg += ", " + extraFields;
        cfg += R"(, "viewDefinition": { "vfov": 30.0, "near": 1.0, "far": 1000.0, "window": { "posX": 0, "posY": 0, "width": 640, "height": 480 } } })";
        cfgFile = std::make_unique<TempConfigFile>(cfg);
    }

    std::unique_ptr<TempConfigFile> entitiesFile;
    std::unique_ptr<TempConfigFile> cfgFile;
};

// =============================================================================
// 测试共用：Host/IG 网络角色辅助（E2E 用）。
// 各测试文件调用时传不同 base 隔离端口，避免并行冲突。
// 命名 makeTest*，避免与 HostIGTests 握手期专用（无 base）的 makeHostLocal 冲突。
// =============================================================================

inline HostConfig makeTestHostConfig(int base)
{
    return HostConfig{base, base + 100};
}

inline IgConfig makeTestIgConfig(int udpRecvPort, int base)
{
    return IgConfig{udpRecvPort, "127.0.0.1", base + 100, base};
}

inline ViewDefinition makeTestViewDefinition(int viewID, bool requireConnectedIg = true)
{
    ViewDefinition viewDefinition;
    viewDefinition.viewID = viewID;
    viewDefinition.requireConnectedIg = requireConnectedIg;
    return viewDefinition;
}

// 独立 Host 端点 + 一台 IG-only。HostSync 由测试直接持有，不经 Engine。
// IG 的 UDP 口是 base+1。要加载实体目录或改口偏移的用例不能走这里。
inline void setupHostIgPair(HostSync& host, Engine& engine, int base)
{
    REQUIRE(host.initialize(makeTestHostConfig(base)));
    REQUIRE(engine.initSync(makeTestIgConfig(base + 1, base)));
    REQUIRE(host.readyIgCount() == 1);
}

// 独立 Host 端点 + 两台 IG-only（viewID 0 与 1）。HostSync 由测试直接持有，不经 Engine。
inline void setupHostDoubleIgPair(HostSync& host, Engine& engineA, Engine& engineB, int base)
{
    REQUIRE(host.initialize(makeTestHostConfig(base)));
    REQUIRE(engineA.initSync(makeTestIgConfig(base + 1, base), makeTestViewDefinition(0)));
    REQUIRE(engineB.initSync(makeTestIgConfig(base + 3, base), makeTestViewDefinition(1)));
    REQUIRE(host.readyIgCount() == 2);
}

// 初始化 Host+IG 并完成握手。返回 true 表示流程走完；调用方若需要 IG 已 ready，自行断言。
inline bool linkHostIg(HostSync& host, IgSync& ig, int base)
{
    if (!host.initialize(makeTestHostConfig(base)))
        return false;
    const IgConfig igConfig = makeTestIgConfig(base + 1, base);
    if (!ig.initialize(igConfig.udpRecvPort) || !ig.connect(igConfig))
        return false;
    return true;
}

// 独立 Host 端点（测试用）：持 HostSync。端口语义 = makeTestHostConfig。
struct TestHost
{
    HostSync sync;
    bool init(int base)
    {
        if (!sync.initialize(makeTestHostConfig(base)))
            return false;
        return true;
    }
};

inline std::string makeIgConfigExtra(int base)
{
    return std::string(R"("igConfig": { "udpRecvPort": )") + std::to_string(base + 1) +
           R"(, "hostAddr": "127.0.0.1", "hostTcpPort": )" + std::to_string(base + 100) +
           R"(, "hostUdpRecvPort": )" + std::to_string(base) + " }";
}

// 条件成立即返回。短轮询只盖住 Host 就绪表晚于 IG connect 返回的缝，不空等固定超时。
inline void connectUntil(const std::function<bool()>& linked)
{
    for (int i = 0; i < 10 && !linked(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

// 条件成立即返回。1ms 一拍，最多 10 拍，盖住回环晚到的报文。
inline void tickIgUntil(Engine& engine, const std::function<bool()>& done)
{
    for (int i = 0; i < 10 && !done(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        engine.tickSync();
    }
}

inline void tickIgUntil(Engine& engineA, Engine& engineB, const std::function<bool()>& done)
{
    for (int i = 0; i < 10 && !done(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        engineA.tickSync();
        engineB.tickSync();
    }
}

// 1ms 一拍，最多 10 拍。与 40×2ms 的等待不是同一个词。
inline void drainHostUntil(HostSync& host, const std::function<bool()>& done)
{
    for (int i = 0; i < 10 && !done(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        host.drainIncoming();
    }
}

// UDP 可丢：actual 落在 [expected-slack, expected]。
inline bool approxAtMost(std::uint32_t actual, int expected, int slack)
{
    const auto exp = static_cast<std::uint32_t>(expected);
    const auto minOk = exp > static_cast<std::uint32_t>(slack) ? exp - static_cast<std::uint32_t>(slack) : 0u;
    return actual >= minOk && actual <= exp;
}

inline Entity& requireEntity(Engine& engine, int id)
{
    Entity* entity = engine.findEntity(id);
    REQUIRE(entity);
    return *entity;
}

// ENU 方向按 LocalToWorld 的正交列转到 ECEF（lla设计 §3.3）。
inline vsg::dvec3 rotateEnuToEcef(const vsg::dmat4& localToWorld, const vsg::dvec3& enuDir)
{
    const vsg::dvec3 east = vsg::normalize(vsg::dvec3(localToWorld(0, 0), localToWorld(0, 1), localToWorld(0, 2)));
    const vsg::dvec3 north = vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
    const vsg::dvec3 upAxis = vsg::normalize(vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
    return enuDir.x * east + enuDir.y * north + enuDir.z * upAxis;
}

// IGCtrl 由 outMsgWithIgCtrlUdp() 自动前置；这里只发无眼点帧。
inline void hostSendFrame(HostSync& host)
{
    host.outMsgWithIgCtrlUdp();
    host.flushUdp();
}

inline void hostSendEyePose(HostSync& host, const aerovista::sync::cigi_wire::EyePose& eye)
{
    auto& omsg = host.outMsgWithIgCtrlUdp();
    aerovista::sync::cigi_wire::appendEye(omsg, &eye);
    host.flushUdp();
}

// 业务侧扇出一帧 IGCtrl + 眼点。
inline void hostSendEyeFrame(HostSync& host, const ChannelEye& eye)
{
    aerovista::sync::cigi_wire::EyePose wire{};
    wire.x = eye.lla.x;
    wire.y = eye.lla.y;
    wire.z = eye.lla.z;
    wire.yawDeg = eye.eulerYprDeg.x;
    wire.pitchDeg = eye.eulerYprDeg.y;
    wire.rollDeg = eye.eulerYprDeg.z;
    hostSendEyePose(host, wire);
}

struct OwnshipEyeCapture
{
    bool got = false;
    std::uint16_t entityId = 0;
    std::uint16_t parentId = 0;
    CigiBaseEntityPositionCtrl::AttachStateGrp attachState = CigiBaseEntityPositionCtrl::Detach;
    double lat = 0.0;
    double lon = 0.0;
    double alt = 0.0;
    double yawDeg = 0.0;
    double pitchDeg = 0.0;
    double rollDeg = 0.0;
};

inline void captureOwnship(IgSync& ig, OwnshipEyeCapture& cap)
{
    ig.addCallback<CigiEntityPositionCtrlV4>([&](const CigiEntityPositionCtrlV4& pose) {
        if (pose.GetEntityID() != 0)
            return;
        cap.got = true;
        cap.entityId = pose.GetEntityID();
        cap.parentId = pose.GetParentID();
        cap.attachState = pose.GetAttachState();
        cap.lat = pose.GetLat();
        cap.lon = pose.GetLon();
        cap.alt = pose.GetAlt();
        cap.yawDeg = pose.GetYaw();
        cap.pitchDeg = pose.GetPitch();
        cap.rollDeg = pose.GetRoll();
    });
}

inline void pumpOwnshipEyeFrames(HostSync& host, IgSync& ig, const aerovista::sync::cigi_wire::EyePose& eye, int frames = 5)
{
    for (int i = 0; i < frames; ++i)
    {
        hostSendEyePose(host, eye);
        ig.drainIncoming();
        ig.update();
    }
}

template<typename PacketT, typename Proc>
void subscribe(IgSync& igSync, const std::shared_ptr<Proc>& proc)
{
    igSync.addCallback<PacketT>([proc](const PacketT& packet) {
        proc->OnPacketReceived(const_cast<PacketT*>(&packet));
    });
}

template<typename PacketT, typename Proc>
void subscribe(HostSync& host, const std::shared_ptr<Proc>& proc)
{
    host.addCallback<PacketT>([proc](const PacketT& packet) {
        proc->OnPacketReceived(const_cast<PacketT*>(&packet));
    });
}

inline void sendHostEntityCtrl(HostSync& host, Engine& engine, Cigi_uint16 entityId,
                               CigiBaseEntityCtrl::EntityStateGrp state, Cigi_uint8 alpha = 255)
{
    auto& tcp = host.outMsgWithIgCtrlTcp();
    CigiEntityCtrlV4 ent;
    ent.SetEntityID(entityId);
    ent.SetEntityState(state);
    ent.SetAlpha(alpha);
    tcp << ent;
    host.flushTcp();
    engine.tickSync();
}

// ownship（EntityID==0）是数据面眼点保留实体，业务 processor 须过滤。
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

// CCL 把 IGMsg.Msg 按 8 字节对齐（含 NUL 填充），按 C 字符串语义取文本。
class TestIgMsgProcessor : public CigiBaseEventProcessor
{
public:
    void OnPacketReceived(CigiBasePacket* packet) override
    {
        auto* igmsg = dynamic_cast<CigiIGMsgV4*>(packet);
        if (!igmsg)
            return;
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
