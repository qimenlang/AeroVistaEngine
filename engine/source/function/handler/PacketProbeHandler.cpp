#include "PacketProbeHandler.h"

// IG→Host 方向可达报文头（cigi梳理.md 链路矩阵；HostSync 侧对应注册，状态同步设计初版.md §8.1）。
#include "CigiAerosolRespV4.h"
#include "CigiAnimationStopV4.h"
#include "CigiCollDetSegRespV4.h"
#include "CigiCollDetVolRespV4.h"
#include "CigiEventNotificationV4.h"
#include "CigiHatHotRespV4.h"
#include "CigiHatHotXRespV4.h"
#include "CigiIGMsgV4.h"
#include "CigiLosRespV4.h"
#include "CigiLosXRespV4.h"
#include "CigiMaritimeSurfaceRespV4.h"
#include "CigiPositionRespV4.h"
#include "CigiSensorRespV4.h"
#include "CigiSensorXRespV4.h"
#include "CigiTerrestrialSurfaceRespV4.h"
#include "CigiWeatherCondRespV4.h"

#include <cstddef>
#include <random>

namespace
{
    /// 一条测试报文：类名（HUD 对照显示）+ 构造并塞入出站消息的发送器。
    struct PacketProbe
    {
        const char* name;
        void (*send)(CigiOutgoingMsg&);
    };

    /// 生成报文探测项：默认构造 `PacketT`，可选宏变参做字段填充。
#define PACKET_PROBE(PacketT, ...)            \
    {                                         \
        #PacketT, [](CigiOutgoingMsg& omsg) { \
            PacketT packet;                   \
            __VA_ARGS__;                      \
            omsg << packet;                   \
        }}

    /// 命令面（TCP）IG→Host 响应/通知类（HostSync registerTcpProcessors 已注册，可订阅）。
    const PacketProbe kTcpProbes[] = {
        PACKET_PROBE(CigiIGMsgV4, packet.SetMsgID(0x1001); packet.SetMsg("ig-probe")),
        PACKET_PROBE(CigiEventNotificationV4),
        PACKET_PROBE(CigiAnimationStopV4),
        PACKET_PROBE(CigiHatHotRespV4),
        PACKET_PROBE(CigiHatHotXRespV4),
        PACKET_PROBE(CigiLosRespV4),
        PACKET_PROBE(CigiLosXRespV4),
        PACKET_PROBE(CigiSensorRespV4),
        PACKET_PROBE(CigiSensorXRespV4),
        PACKET_PROBE(CigiPositionRespV4),
        PACKET_PROBE(CigiWeatherCondRespV4),
        PACKET_PROBE(CigiAerosolRespV4),
        PACKET_PROBE(CigiMaritimeSurfaceRespV4),
        PACKET_PROBE(CigiTerrestrialSurfaceRespV4),
        PACKET_PROBE(CigiCollDetSegRespV4),
        PACKET_PROBE(CigiCollDetVolRespV4),
    };

#undef PACKET_PROBE

    template<std::size_t probeCount>
    const PacketProbe& pickRandomProbe(const PacketProbe (&probes)[probeCount])
    {
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<std::size_t> dist(0, probeCount - 1);
        return probes[dist(rng)];
    }

    template<typename PacketT>
    void bindRecvName(aerovista::sync::IgSync& ig, std::string& out, const char* name)
    {
        ig.addCallback<PacketT>([&out, name](const PacketT&) { out = name; });
    }
} // namespace

void PacketProbeHandler::bindRecvProbes(aerovista::sync::IgSync& ig, std::string& lastReceivedName)
{
    bindRecvName<CigiConfClampEntityCtrlV4>(ig, lastReceivedName, "CigiConfClampEntityCtrlV4");
    bindRecvName<CigiVelocityCtrlV4>(ig, lastReceivedName, "CigiVelocityCtrlV4");
    bindRecvName<CigiAccelerationCtrlV4>(ig, lastReceivedName, "CigiAccelerationCtrlV4");
    bindRecvName<CigiViewCtrlV4>(ig, lastReceivedName, "CigiViewCtrlV4");
    bindRecvName<CigiEntityCtrlV4>(ig, lastReceivedName, "CigiEntityCtrlV4");
    bindRecvName<CigiArtPartCtrlV4>(ig, lastReceivedName, "CigiArtPartCtrlV4");
    bindRecvName<CigiShortArtPartCtrlV4>(ig, lastReceivedName, "CigiShortArtPartCtrlV4");
    bindRecvName<CigiCompCtrlV4>(ig, lastReceivedName, "CigiCompCtrlV4");
    bindRecvName<CigiShortCompCtrlV4>(ig, lastReceivedName, "CigiShortCompCtrlV4");
    bindRecvName<CigiAnimationCtrlV4>(ig, lastReceivedName, "CigiAnimationCtrlV4");
    bindRecvName<CigiViewDefV4>(ig, lastReceivedName, "CigiViewDefV4");
    bindRecvName<CigiSensorCtrlV4>(ig, lastReceivedName, "CigiSensorCtrlV4");
    bindRecvName<CigiMotionTrackCtrlV4>(ig, lastReceivedName, "CigiMotionTrackCtrlV4");
    bindRecvName<CigiAtmosCtrlV4>(ig, lastReceivedName, "CigiAtmosCtrlV4");
    bindRecvName<CigiCelestialCtrlV4>(ig, lastReceivedName, "CigiCelestialCtrlV4");
    bindRecvName<CigiEnvRgnCtrlV4>(ig, lastReceivedName, "CigiEnvRgnCtrlV4");
    bindRecvName<CigiWeatherCtrlV4>(ig, lastReceivedName, "CigiWeatherCtrlV4");
    bindRecvName<CigiMaritimeSurfaceCtrlV4>(ig, lastReceivedName, "CigiMaritimeSurfaceCtrlV4");
    bindRecvName<CigiTerrestrialSurfaceCtrlV4>(ig, lastReceivedName, "CigiTerrestrialSurfaceCtrlV4");
    bindRecvName<CigiWaveCtrlV4>(ig, lastReceivedName, "CigiWaveCtrlV4");
    bindRecvName<CigiEarthModelDefV4>(ig, lastReceivedName, "CigiEarthModelDefV4");
    bindRecvName<CigiCollDetSegDefV4>(ig, lastReceivedName, "CigiCollDetSegDefV4");
    bindRecvName<CigiCollDetVolDefV4>(ig, lastReceivedName, "CigiCollDetVolDefV4");
    bindRecvName<CigiHatHotReqV4>(ig, lastReceivedName, "CigiHatHotReqV4");
    bindRecvName<CigiLosSegReqV4>(ig, lastReceivedName, "CigiLosSegReqV4");
    bindRecvName<CigiLosVectReqV4>(ig, lastReceivedName, "CigiLosVectReqV4");
    bindRecvName<CigiPositionReqV4>(ig, lastReceivedName, "CigiPositionReqV4");
    bindRecvName<CigiEnvCondReqV4>(ig, lastReceivedName, "CigiEnvCondReqV4");
    bindRecvName<CigiSymbolCtrlV4>(ig, lastReceivedName, "CigiSymbolCtrlV4");
    bindRecvName<CigiShortSymbolCtrlV4>(ig, lastReceivedName, "CigiShortSymbolCtrlV4");
    bindRecvName<CigiSymbolSurfaceDefV4>(ig, lastReceivedName, "CigiSymbolSurfaceDefV4");
    bindRecvName<CigiSymbolTextDefV4>(ig, lastReceivedName, "CigiSymbolTextDefV4");
    bindRecvName<CigiSymbolCircleDefV4>(ig, lastReceivedName, "CigiSymbolCircleDefV4");
    bindRecvName<CigiSymbolPolygonDefV4>(ig, lastReceivedName, "CigiSymbolPolygonDefV4");
    bindRecvName<CigiSymbolTexturedCircleDefV4>(ig, lastReceivedName, "CigiSymbolTexturedCircleDefV4");
    bindRecvName<CigiSymbolTexturedPolygonDefV4>(ig, lastReceivedName, "CigiSymbolTexturedPolygonDefV4");
    bindRecvName<CigiSymbolCloneV4>(ig, lastReceivedName, "CigiSymbolCloneV4");
}

void PacketProbeHandler::apply(vsg::KeyPressEvent& keyPress)
{
    if (!ig || !lastSentName)
        return;

    if (keyPress.keyBase == vsg::KEY_F9)
    {
        const PacketProbe& probe = pickRandomProbe(kTcpProbes);
        auto& omsg = ig->outMsgWithSofTcp();
        probe.send(omsg);
        ig->flushTcp();
        *lastSentName = probe.name;
        return;
    }

    if (keyPress.keyBase == vsg::KEY_F10)
    {
        // IG→Host UDP 仅 SOF 一种（cigi梳理.md 链路矩阵）：outMsgWithSofUdp 自动前置 SOF，flushUdp 发出。
        ig->outMsgWithSofUdp();
        ig->flushUdp();
        *lastSentName = "CigiSOFV4";
    }
}
