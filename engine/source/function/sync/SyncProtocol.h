#pragma once

#include <cstdint>

namespace sync_proto
{
    constexpr uint32_t kMagic = 0x41565359u; // 'AVSY'

    enum class MsgType : uint32_t
    {
        Hello = 1,
        HelloAck = 2,
        UdpSync = 3,
        UdpSyncAck = 4,
        IgCtrl = 5,
        Sof = 6
    };

#pragma pack(push, 1)
    struct WireMsg
    {
        uint32_t magic = kMagic;
        uint32_t type = 0;
        uint32_t udpRecvPort = 0;
    };

    /// IGCtrl + optional Host eye (EntityPosition semantics) in one datagram.
    struct IgCtrlMsg
    {
        uint32_t magic = kMagic;
        uint32_t type = static_cast<uint32_t>(MsgType::IgCtrl);
        uint32_t frameCntr = 0;
        double simTimeMs = 0.0;
        uint32_t hasEye = 0; // 1 → pos/euler valid (world XYZ + YPR degrees)
        double posX = 0.0;
        double posY = 0.0;
        double posZ = 0.0;
        double yawDeg = 0.0;
        double pitchDeg = 0.0;
        double rollDeg = 0.0;
    };

    struct SofMsg
    {
        uint32_t magic = kMagic;
        uint32_t type = static_cast<uint32_t>(MsgType::Sof);
        uint32_t frameCntr = 0;
    };
#pragma pack(pop)

    static_assert(sizeof(WireMsg) == 12, "WireMsg size");
    static_assert(sizeof(IgCtrlMsg) == 72, "IgCtrlMsg size");
    static_assert(sizeof(SofMsg) == 12, "SofMsg size");
} // namespace sync_proto
