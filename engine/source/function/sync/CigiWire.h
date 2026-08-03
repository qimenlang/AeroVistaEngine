#pragma once

#include <cstdint>
#include <optional>
#include <vector>

/// CIGI V4 data-plane pack/unpack for Host↔IG sync.
/// Handshake (HELLO / UDP_SYNC) stays on sync_proto::WireMsg — see SyncProtocol.h.
namespace cigi_wire
{
    struct EyePose
    {
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        double yawDeg = 0.0;
        double pitchDeg = 0.0;
        double rollDeg = 0.0;
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

    /// Pack Host→IG: IGCtrlV4 [+ EntityPositionCtrlV4 when eye != nullptr].
    /// Local XYZ eye uses Attach + X/Y/Z off (temporary until LLA / §4.8).
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
