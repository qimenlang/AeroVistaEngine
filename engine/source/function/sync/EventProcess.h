#pragma once
#include "CigiBaseEventProcessor.h"
#include "CigiIGCtrlV4.h"

class IGCtrl : public CigiBaseEventProcessor
{
public:
    IGCtrl() {};
    virtual ~IGCtrl() {};

    virtual void OnPacketReceived(CigiBasePacket* packet);

    void SetOrigPckt(CigiIGCtrlV4* packetIn) { _packet = packetIn; }

protected:
    CigiIGCtrlV4* _packet;
};