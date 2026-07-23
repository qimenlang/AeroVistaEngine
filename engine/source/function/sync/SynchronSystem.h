#pragma once
#include "EventProcess.h"
#include "Network.h"
#include <CigiIGSession.h>
#include <CigiIncomingMsg.h>
#include <CigiOutgoingMsg.h>
#include <CigiSOFV4.h>
#include <cstdint>
#include <string>
#include <vsg/all.h>

class SynchronSystem : public vsg::Inherit<vsg::Object, SynchronSystem>
{
private:
    Network _network;
    CigiIGSession* _igSession;
    CigiOutgoingMsg* _outgoingMsg;
    CigiIncomingMsg* _incomingMsg;

    CigiSOFV4* _sof;

    std::string addr;
    int portSend;
    int portRecv;

#define RECV_BUFFER_SIZE 32768
    unsigned char _incomingBuffer[RECV_BUFFER_SIZE];
    unsigned char* _outgoingBuffer;

    int _incomingBufferSize;
    int _outgoingBufferSize;

    IGCtrl _igCtrl;

    LARGE_INTEGER _timerFreq;
    LARGE_INTEGER _timeStampStart;
    LARGE_INTEGER _timeStampEnd;

    unsigned long _frameCounter = 0;
    float _timeDelayLimit = 0.0167f;

    void waitUntilBeginningOfFrame();

public:
    void Initialize();
    void Update();
    void Shutdown();
};