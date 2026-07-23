#include "SynchronSystem.h"
#include <CigiException.h>
#include <iostream>

void SynchronSystem::Initialize()
{
    addr = "127.0.0.1";
    portSend = 8001;
    portRecv = 8000;

    bool netstatus = _network.openSocket(addr.c_str(), portSend, portRecv);

    if (!netstatus)
    {
        std::cerr << "could not connect to CIGI host server" << std::endl;
    }
    else
    {
        std::cout << "successfully connected to CIGI host server" << std::endl;
    }

    _igSession = new CigiIGSession(1, 32768, 2, 32768);
    _igSession->SetCigiVersion(4, 0);
    _igSession->SetSynchronous(true);

    _incomingMsg = &_igSession->GetIncomingMsgMgr();
    _incomingMsg->SetReaderCigiVersion(4, 0);
    _incomingMsg->UsingIteration(false);
    _incomingMsg->RegisterEventProcessor(CIGI_IG_CTRL_PACKET_ID_V4, static_cast<CigiBaseEventProcessor*>(&_igCtrl));

    _outgoingMsg = &_igSession->GetOutgoingMsgMgr();
    _outgoingMsg->BeginMsg();

    _sof = new CigiSOFV4();
    _sof->SetDatabaseID(0);
    _sof->SetIGStatus(0);
    _sof->SetIGMode(CigiBaseSOF::Operate);
    _sof->SetTimeStampValid(false);
    _sof->SetEarthRefModel(CigiBaseSOF::WGS84);
    _sof->SetTimeStamp(0);
    _sof->SetFrameCntr(0);

    BOOL perfTimerFlag = QueryPerformanceFrequency(&_timerFreq);
    if (perfTimerFlag)
    {
        QueryPerformanceCounter(&_timeStampStart);
    }
}

void SynchronSystem::Update()
{
    /* process incoming CIGI message - this could be long */
    if (_incomingBufferSize > 0)
    {
        try
        {
            _incomingMsg->ProcessIncomingMsg((unsigned char*)_incomingBuffer, _incomingBufferSize);
        }
        catch (CigiException& theException)
        {
            std::cout << "getNetMessages - Exception: " << theException.what() << std::endl;
        }
    }

    // set frame counter and time stamp
    _sof->SetFrameCntr(_frameCounter++);
    QueryPerformanceCounter(&_timeStampEnd);
    _sof->SetTimeStamp(static_cast<unsigned long>((_timeStampEnd.QuadPart - _timeStampStart.QuadPart) * 1000 / _timerFreq.QuadPart));
    _sof->SetTimeStampValid(true);

    // send SOF Control
    *_outgoingMsg << *_sof;

    waitUntilBeginningOfFrame();

    // Package msg
    try
    {
        _outgoingMsg->PackageMsg(&_outgoingBuffer, _outgoingBufferSize);
    }
    catch (CigiException& theException)
    {
        std::cout << "getNetMessages - Exception: " << theException.what() << std::endl;
    }

    // Update SOF Frame IDs
    _outgoingMsg->UpdateSOF(_outgoingBuffer, _incomingBuffer);

    // send SOF message
    int sentBytes = _network.send(_outgoingBuffer, _outgoingBufferSize);

    // Frees the buffer containing the message that was just sent
    _outgoingMsg->FreeMsg();

    // wait for Host message
    time_t HoldTime;
    bool RcvrProc = false;
    time_t CheckTime = time(&HoldTime);
    while (!RcvrProc)
    {
        if ((_incomingBufferSize = _network.recv(_incomingBuffer, RECV_BUFFER_SIZE)) > 0)
            RcvrProc = true;
        else
        {
            time_t TstTime = time(&HoldTime);
            if ((TstTime - CheckTime) > 1)
            {
                static unsigned int c = 0;
                cout << "Did not receive IG Control ";
                cout << c++;
                cout << "\n";
                RcvrProc = true;
                _incomingBufferSize = 0;
            }
        }
    }
}

void SynchronSystem::Shutdown()
{
    _network.closeSocket();
    delete _igSession;
    delete _sof;
}

void SynchronSystem::waitUntilBeginningOfFrame(void)
{
    static DWORD t1 = 0;
    static DWORD t2 = 0;
    static bool firsttimethrough = true;

    if (firsttimethrough)
    {
        t1 = GetTickCount();
        firsttimethrough = false;
    }

    do {
        t2 = GetTickCount(); // number of milliseconds
    } while ((t2 - t1) < _timeDelayLimit);

    t1 = t2;
}