#pragma once

#include "EventProcess.h"
#include "Network.h"
#include "SyncConfig.h"

#include <CigiHostSession.h>
#include <CigiIGCtrlV4.h>
#include <CigiIGSession.h>
#include <CigiIncomingMsg.h>
#include <CigiOutgoingMsg.h>
#include <CigiSOFV4.h>
#include <CigiSession.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vsg/all.h>

// Legacy role tag used by older Initialize paths; new code uses HostSync / IgSync.
enum class HostIGType
{
    HOST,
    IG
};

struct HostIGConfig
{
    HostIGType type;
    std::string addr;
    int portSend = 0;
    int portRecv = 0;
};

/// Engine-facing sync facade (loop preFrame/update). Owns IgSync; may temporarily own
/// HostSync for feeding broadcast data. See doc/多通道同步模块设计.md.
class SynchronSystem : public vsg::Inherit<vsg::Object, SynchronSystem>
{
public:
    SynchronSystem();
    ~SynchronSystem() override;

    bool Initialize(const HostIGConfig& config);
    bool Connect();
    void Update();
    void Shutdown();

private:
    static constexpr int RECV_BUFFER_SIZE = 32768;
    static constexpr int ConnectTimeoutMs = 500;

    bool initializeHost();
    bool initializeIG();
    void startHostBeacon();
    void stopHostBeacon();
    void hostBeaconLoop();
    bool sendHostIgCtrl();
    bool waitForHostPacket(int timeoutMs);

    Network _network;
    HostIGConfig _config{};
    CigiSession* _session = nullptr;
    CigiOutgoingMsg* _outgoingMsg = nullptr;
    CigiIncomingMsg* _incomingMsg = nullptr;

    CigiIGCtrlV4 _igCtrlPacket;
    CigiSOFV4 _sofPacket;

    unsigned char _incomingBuffer[RECV_BUFFER_SIZE]{};
    unsigned char* _outgoingBuffer = nullptr;
    int _incomingBufferSize = 0;
    int _outgoingBufferSize = 0;

    IGCtrl _igCtrlProcessor;
    SofProcessor _sofProcessor;

    LARGE_INTEGER _timerFreq{};
    LARGE_INTEGER _timeStampStart{};
    LARGE_INTEGER _timeStampEnd{};

    unsigned long _frameCounter = 0;
    float _timeDelayLimit = 0.0167f;

    std::atomic<bool> _initialized{false};
    std::atomic<bool> _connected{false};
    std::atomic<bool> _stopHostBeacon{false};
    std::thread _hostBeaconThread;

    void waitUntilBeginningOfFrame();
};
