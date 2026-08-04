#pragma once

#include <cstdint>
#include <optional>
#include <vector>

/// CIGI V4 data-plane pack/unpack for Host↔IG sync.
/// Handshake (HELLO / UDP_SYNC) stays on sync_proto::WireMsg — see SyncProtocol.h.
namespace cigi_wire
{
    /// Wire position semantics from EntityPosition AttachState (lla设计 §5).
    enum class EyeFrame : std::uint8_t
    {
        WORLD_LOCAL = 0, ///< Attach + X/Y/Z off
        LLA = 1          ///< Detach + Lat/Lon/Alt
    };

    struct EyePose
    {
        double x = 0.0; ///< WORLD_LOCAL: X off m; LLA: lat°
        double y = 0.0; ///< WORLD_LOCAL: Y off m; LLA: lon°
        double z = 0.0; ///< WORLD_LOCAL: Z off m; LLA: alt m
        double yawDeg = 0.0;
        double pitchDeg = 0.0;
        double rollDeg = 0.0;
        EyeFrame frame = EyeFrame::WORLD_LOCAL;
        std::uint16_t entityId = 0;
        std::uint16_t parentId = 0;
    };

    struct HostFrame
    {
        std::uint32_t frameCntr = 0;
        std::uint32_t timeStamp = 0;
        bool timeStampValid = false;
        std::optional<EyePose> eye;
    };

    /// True if buffer starts with sync_proto AVSY magic (handshake plane).
    bool isAvsyMagic(const unsigned char* data, int n);

    /// Count of LLA eyes dropped for lat/pitch out of range (lla设计 §5).
    std::uint64_t eyePoseRejectedByRange();

    /// Pack Host→IG: IGCtrlV4 [+ EntityPositionCtrlV4 when eye != nullptr].
    /// WorldLocal → Attach+XYZ ParentID=1; Lla → Detach+LLA ParentID=0.
    bool packHostFrame(std::uint32_t frameCntr, double simTimeMs, const EyePose* eye,
                       std::vector<unsigned char>& out);

    /// Pack IG→Host: SOFV4 with FrameCntr echo.
    bool packSof(std::uint32_t frameCntr, std::vector<unsigned char>& out);

    /// Unpack Host→IG datagram. Requires IGCtrl; eye optional.
    bool unpackHostFrame(const unsigned char* data, int n, HostFrame& out);

    /// Unpack IG→Host SOF datagram.
    bool unpackSof(const unsigned char* data, int n, std::uint32_t& frameCntrOut);

    /// simTimeMs → CIGI TimeStamp (10 µs steps).
    inline std::uint32_t simTimeMsToTimeStamp(double simTimeMs)
    {
        if (simTimeMs <= 0.0)
            return 0;
        const double ticks = simTimeMs * 100.0; // ms → 10 µs
        if (ticks >= 4294967295.0)
            return 0xffffffffu;
        return static_cast<std::uint32_t>(ticks);
    }
} // namespace cigi_wire
