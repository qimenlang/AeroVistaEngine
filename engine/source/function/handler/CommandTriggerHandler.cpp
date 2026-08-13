#include "CommandTriggerHandler.h"

#include "function/sync/HostSync.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using aerovista::sync::HostSync;
namespace cigi_wire = aerovista::sync::cigi_wire;

namespace
{
    // 命令面载荷编码（状态同步设计初版.md §2.2，本机字节序 LE）：
    // LOADMODEL = [id(4B)][path…]；PLACEMODEL = [id(4B)][pos(3×8B)][ypr(3×8B)]。
    void appendLeU32(std::vector<std::uint8_t>& p, std::uint32_t value)
    {
        p.push_back(static_cast<std::uint8_t>(value & 0xFF));
        p.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        p.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
        p.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    }

    void appendLeF64(std::vector<std::uint8_t>& p, double value)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int i = 0; i < 8; ++i)
            p.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    }

    std::vector<std::uint8_t> makeLoadModelPayload(std::uint32_t id, const std::string& path)
    {
        std::vector<std::uint8_t> p;
        appendLeU32(p, id);
        p.insert(p.end(), path.begin(), path.end());
        return p;
    }

    std::vector<std::uint8_t> makePlaceModelPayload(std::uint32_t id, double x, double y, double z,
                                                    double yawDeg, double pitchDeg, double rollDeg)
    {
        std::vector<std::uint8_t> p;
        appendLeU32(p, id);
        appendLeF64(p, x);
        appendLeF64(p, y);
        appendLeF64(p, z);
        appendLeF64(p, yawDeg);
        appendLeF64(p, pitchDeg);
        appendLeF64(p, rollDeg);
        return p;
    }

    // MOVEMODEL 载荷（初版 §2.2）：[id(4B)][deltaPos(3×8B)][deltaYpr(3×8B)]，与 PLACE 布局同构、语义为增量。
    std::vector<std::uint8_t> makeMoveModelPayload(std::uint32_t id, double dx, double dy, double dz,
                                                   double dyawDeg, double dpitchDeg, double drollDeg)
    {
        return makePlaceModelPayload(id, dx, dy, dz, dyawDeg, dpitchDeg, drollDeg);
    }
} // namespace

void CommandTriggerHandler::apply(vsg::KeyPressEvent& keyPress)
{
    if (!synchronSystem || !synchronSystem->hasHost())
        return;
    HostSync& host = synchronSystem->hostSync();

    if (keyPress.keyBase == vsg::KEY_F9)
    {
        const bool loadOk = host.sendCommand(cigi_wire::Command::LOAD_MODEL, makeLoadModelPayload(7, "teapot"));
        const bool placeOk = host.sendCommand(cigi_wire::Command::PLACE_MODEL,
                                              makePlaceModelPayload(7, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0));
        std::cout << "[Cmd] F3 LOAD+PLACE id=7 teapot: delivered=" << (loadOk && placeOk) << std::endl;
        return;
    }

    if (keyPress.keyBase == vsg::KEY_F10)
    {
        // MOVEMODEL：向上移动 0.1 m（Y+，本坐标系 Z-up 时向上为 Y+；增量无姿态变化）。
        const bool moveOk = host.sendCommand(cigi_wire::Command::MOVE_MODEL,
                                             makeMoveModelPayload(7, 0.0, 0.1, 0.0, 0.0, 0.0, 0.0));
        std::cout << "[Cmd] F4 MOVE id=7 +Y 0.1m: delivered=" << moveOk << std::endl;
    }
}
