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

#include <cstring>
#include <memory>
#include <mutex>

namespace cigi_wire
{
    namespace
    {
        // CCL is not thread-safe; Host udpLoop and the engine thread both touch it.
        std::mutex gCigiMutex;

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
                eye.x = ent->GetXoff();
                eye.y = ent->GetYoff();
                eye.z = ent->GetZoff();
                eye.yawDeg = ent->GetYaw();
                eye.pitchDeg = ent->GetPitch();
                eye.rollDeg = ent->GetRoll();
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
        if (eye)
        {
            // Local world XYZ: Attach offsets from synthetic parent (see design §8).
            ent.SetEntityID(0);
            ent.SetParentID(1);
            ent.SetAttachState(CigiBaseEntityPositionCtrl::Attach);
            ent.SetXoff(eye->x);
            ent.SetYoff(eye->y);
            ent.SetZoff(eye->z);
            ent.SetYaw(static_cast<float>(eye->yawDeg), false);
            ent.SetPitch(static_cast<float>(eye->pitchDeg), false);
            ent.SetRoll(static_cast<float>(eye->rollDeg), false);
        }

        CigiOutgoingMsg& omsg = rt.host.GetOutgoingMsgMgr();
        omsg.BeginMsg();
        omsg << igCtrl;
        if (eye)
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
            outFrame.eye = rt.entityPosProc.eye;
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
