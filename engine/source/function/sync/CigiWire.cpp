#include "CigiIncludes.h"

#include "CigiWire.h"
#include "SyncProtocol.h"

#include "CigiBaseEntityPositionCtrl.h"
#include "CigiBaseEventProcessor.h"
#include "CigiBaseIGCtrl.h"
#include "CigiBaseSOF.h"
#include "CigiEntityPositionCtrlV4.h"
#include "CigiHostSession.h"
#include "CigiIGCtrlV4.h"
#include "CigiIGSession.h"
#include "CigiIncomingMsg.h"
#include "CigiOutgoingMsg.h"
#include "CigiSOFV4.h"

#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>

namespace cigi_wire
{
    namespace
    {
        // CCL is not thread-safe; Host udpLoop and the engine thread both touch it.
        std::mutex gCigiMutex;
        std::uint64_t gEyePoseRejectedByRange = 0;

        /// Normalize longitude to (-180, 180] (lla设计 §5).
        double normalizeLonDeg(double lon)
        {
            double x = std::fmod(lon, 360.0);
            if (x <= -180.0)
                x += 360.0;
            if (x > 180.0)
                x -= 360.0;
            return x;
        }

        bool llaEyeInRange(double lat, double /*lon*/, double pitchDeg)
        {
            return lat >= -90.0 && lat <= 90.0 && pitchDeg >= -90.0 && pitchDeg <= 90.0;
        }

        constexpr int kCigiBufCount = 1;
        constexpr int kCigiBufLen = 4096;

        class CaptureIgCtrlProc : public CigiBaseEventProcessor
        {
        public:
            void OnPacketReceived(CigiBasePacket* packet) override
            {
                auto* ig = dynamic_cast<CigiIGCtrlV4*>(packet);
                if (!ig)
                    return;
                got = true;
                frameCntr = ig->GetFrameCntr();
                timeStamp = ig->GetTimeStamp();
                timeStampValid = ig->GetTimeStampValid();
            }

            void reset()
            {
                got = false;
                frameCntr = 0;
                timeStamp = 0;
                timeStampValid = false;
            }

            bool got = false;
            std::uint32_t frameCntr = 0;
            std::uint32_t timeStamp = 0;
            bool timeStampValid = false;
        };

        class CaptureEntityPosProc : public CigiBaseEventProcessor
        {
        public:
            void OnPacketReceived(CigiBasePacket* packet) override
            {
                auto* ent = dynamic_cast<CigiEntityPositionCtrlV4*>(packet);
                if (!ent)
                    return;
                got = true;
                eye.entityId = ent->GetEntityID();
                eye.parentId = ent->GetParentID();
                eye.yawDeg = ent->GetYaw();
                eye.pitchDeg = ent->GetPitch();
                eye.rollDeg = ent->GetRoll();
                if (ent->GetAttachState() == CigiBaseEntityPositionCtrl::Detach)
                {
                    eye.frame = EyeFrame::LLA;
                    eye.x = ent->GetLat();
                    eye.y = ent->GetLon();
                    eye.z = ent->GetAlt();
                }
                else
                {
                    eye.frame = EyeFrame::WORLD_LOCAL;
                    eye.x = ent->GetXoff();
                    eye.y = ent->GetYoff();
                    eye.z = ent->GetZoff();
                }
            }

            void reset()
            {
                got = false;
                eye = {};
            }

            bool got = false;
            EyePose eye{};
        };

        class CaptureSofProc : public CigiBaseEventProcessor
        {
        public:
            void OnPacketReceived(CigiBasePacket* packet) override
            {
                auto* sof = dynamic_cast<CigiSOFV4*>(packet);
                if (!sof)
                    return;
                got = true;
                frameCntr = sof->GetFrameCntr();
            }

            void reset()
            {
                got = false;
                frameCntr = 0;
            }

            bool got = false;
            std::uint32_t frameCntr = 0;
        };

        /// One-time CCL init. Creating Cigi*Session per call rebuilds the full
        /// outgoing/incoming packet handler tables and dominates test runtime.
        struct CigiRuntime
        {
            CigiRuntime() :
                host(kCigiBufCount, kCigiBufLen, kCigiBufCount, kCigiBufLen), ig(kCigiBufCount, kCigiBufLen, kCigiBufCount, kCigiBufLen)
            {
                host.SetCigiVersion(4, 0);
                host.SetSynchronous(false);
                ig.SetCigiVersion(4, 0);
                ig.SetSynchronous(false);

                // Processors outlive the sessions; register once (push_back).
                ig.GetIncomingMsgMgr().RegisterEventProcessor(CIGI_IG_CTRL_PACKET_ID_V4, &igCtrlProc);
                ig.GetIncomingMsgMgr().RegisterEventProcessor(CIGI_ENTITY_POSITION_CTRL_PACKET_ID_V4,
                                                              &entityPosProc);
                host.GetIncomingMsgMgr().RegisterEventProcessor(CIGI_SOF_PACKET_ID_V4, &sofProc);
            }

            CigiHostSession host;
            CigiIGSession ig;
            CaptureIgCtrlProc igCtrlProc;
            CaptureEntityPosProc entityPosProc;
            CaptureSofProc sofProc;
        };

        CigiRuntime& runtime()
        {
            static CigiRuntime rt;
            return rt;
        }
    } // namespace

    bool isAvsyMagic(const unsigned char* data, int n)
    {
        if (data == nullptr || n < 4)
            return false;
        std::uint32_t magic = 0;
        std::memcpy(&magic, data, sizeof(magic));
        return magic == sync_proto::kMagic;
    }

    std::uint64_t eyePoseRejectedByRange()
    {
        std::lock_guard lock(gCigiMutex);
        return gEyePoseRejectedByRange;
    }

    bool packHostFrame(std::uint32_t frameCntr, double simTimeMs, const EyePose* eye,
                       std::vector<unsigned char>& out)
    {
        out.clear();
        std::lock_guard lock(gCigiMutex);
        CigiRuntime& rt = runtime();

        CigiIGCtrlV4 igCtrl;
        igCtrl.SetFrameCntr(frameCntr);
        igCtrl.SetTimeStamp(simTimeMsToTimeStamp(simTimeMs));
        igCtrl.SetTimeStampValid(true);

        CigiEntityPositionCtrlV4 ent{};
        bool includeEye = static_cast<bool>(eye);
        if (eye)
        {
            ent.SetEntityID(0);
            ent.SetYaw(static_cast<float>(eye->yawDeg), false);
            ent.SetPitch(static_cast<float>(eye->pitchDeg), false);
            ent.SetRoll(static_cast<float>(eye->rollDeg), false);

            if (eye->frame == EyeFrame::LLA)
            {
                const double lon = normalizeLonDeg(eye->y);
                if (!llaEyeInRange(eye->x, lon, eye->pitchDeg))
                {
                    ++gEyePoseRejectedByRange;
                    includeEye = false; // IGCtrl still sent (lla设计 §5)
                }
                else
                {
                    // Ellipsoid: Detach + LLA, ParentID must be 0 (lla设计 §5).
                    ent.SetParentID(0);
                    ent.SetAttachState(CigiBaseEntityPositionCtrl::Detach);
                    ent.SetLat(eye->x, false);
                    ent.SetLon(lon, false);
                    ent.SetAlt(eye->z, false);
                }
            }
            else
            {
                // Local world XYZ: Attach offsets from synthetic parent (lla设计 §5).
                ent.SetParentID(1);
                ent.SetAttachState(CigiBaseEntityPositionCtrl::Attach);
                ent.SetXoff(eye->x);
                ent.SetYoff(eye->y);
                ent.SetZoff(eye->z);
            }
        }

        CigiOutgoingMsg& omsg = rt.host.GetOutgoingMsgMgr();
        omsg.BeginMsg();
        omsg << igCtrl;
        if (includeEye)
            omsg << ent;

        Cigi_uint8* buf = nullptr;
        int len = 0;
        if (omsg.PackageMsg(&buf, len) != CIGI_SUCCESS || buf == nullptr || len <= 0)
        {
            omsg.FreeMsg();
            return false;
        }
        out.assign(buf, buf + len);
        omsg.FreeMsg();
        return !out.empty();
    }

    bool packSof(std::uint32_t frameCntr, std::vector<unsigned char>& out)
    {
        out.clear();
        std::lock_guard lock(gCigiMutex);
        CigiRuntime& rt = runtime();

        CigiSOFV4 sof;
        sof.SetFrameCntr(frameCntr);

        CigiOutgoingMsg& omsg = rt.ig.GetOutgoingMsgMgr();
        omsg.BeginMsg();
        omsg << sof;

        Cigi_uint8* buf = nullptr;
        int len = 0;
        if (omsg.PackageMsg(&buf, len) != CIGI_SUCCESS || buf == nullptr || len <= 0)
        {
            omsg.FreeMsg();
            return false;
        }
        out.assign(buf, buf + len);
        omsg.FreeMsg();
        return !out.empty();
    }

    bool unpackHostFrame(const unsigned char* data, int n, HostFrame& outFrame)
    {
        outFrame = {};
        if (data == nullptr || n <= 0 || isAvsyMagic(data, n))
            return false;

        std::lock_guard lock(gCigiMutex);
        CigiRuntime& rt = runtime();
        rt.igCtrlProc.reset();
        rt.entityPosProc.reset();

        try
        {
            rt.ig.GetIncomingMsgMgr().ProcessIncomingMsg(const_cast<unsigned char*>(data), n);
        }
        catch (...)
        {
            return false;
        }

        if (!rt.igCtrlProc.got)
            return false;

        outFrame.frameCntr = rt.igCtrlProc.frameCntr;
        outFrame.timeStamp = rt.igCtrlProc.timeStamp;
        outFrame.timeStampValid = rt.igCtrlProc.timeStampValid;
        if (rt.entityPosProc.got)
        {
            EyePose eye = rt.entityPosProc.eye;
            // Detach with non-zero ParentID is illegal for our eye slot — drop eye (lla设计 §5).
            if (eye.frame == EyeFrame::LLA && eye.parentId != 0)
                ; // leave outFrame.eye empty
            else
                outFrame.eye = eye;
        }
        return true;
    }

    bool unpackSof(const unsigned char* data, int n, std::uint32_t& frameCntrOut)
    {
        frameCntrOut = 0;
        if (data == nullptr || n <= 0 || isAvsyMagic(data, n))
            return false;

        std::lock_guard lock(gCigiMutex);
        CigiRuntime& rt = runtime();
        rt.sofProc.reset();

        try
        {
            rt.host.GetIncomingMsgMgr().ProcessIncomingMsg(const_cast<unsigned char*>(data), n);
        }
        catch (...)
        {
            return false;
        }

        if (!rt.sofProc.got)
            return false;
        frameCntrOut = rt.sofProc.frameCntr;
        return true;
    }
} // namespace cigi_wire
