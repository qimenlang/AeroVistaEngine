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
        UdpSyncAck = 4
    };

#pragma pack(push, 1)
    struct WireMsg
    {
        uint32_t magic = kMagic;
        uint32_t type = 0;
        uint32_t udpRecvPort = 0;
    };
#pragma pack(pop)

    static_assert(sizeof(WireMsg) == 12, "WireMsg size");
} // namespace sync_proto
