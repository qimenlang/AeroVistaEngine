#include "SynchronSystem.h"

#include <CigiException.h>

#include <chrono>
#include <iostream>
#include <thread>

SynchronSystem::SynchronSystem() = default;

SynchronSystem::~SynchronSystem()
{
    Shutdown();
}

bool SynchronSystem::Initialize(const HostIGConfig& config)
{
    if (_initialized)
        Shutdown();

    _config = config;
    _connected = false;
    _incomingBufferSize = 0;
    _outgoingBufferSize = 0;
    _outgoingBuffer = nullptr;
    _frameCounter = 0;

    if (!_network.openSocket(_config.addr.c_str(), _config.portSend, _config.portRecv))
    {
        std::cerr << "SynchronSystem: failed to open sockets" << std::endl;
        return false;
    }

    const bool ok = (_config.type == HostIGType::HOST) ? initializeHost() : initializeIG();
    if (!ok)
    {
        _network.closeSocket();
        return false;
    }

    BOOL perfTimerFlag = QueryPerformanceFrequency(&_timerFreq);
    if (perfTimerFlag)
        QueryPerformanceCounter(&_timeStampStart);

    _initialized = true;

    if (_config.type == HostIGType::HOST)
        startHostBeacon();

    return true;
}

bool SynchronSystem::initializeHost()
{
    auto* hostSession = new CigiHostSession(1, 32768, 2, 32768);
    _session = hostSession;
    _session->SetCigiVersion(4, 0);
    _session->SetSynchronous(true);

    _incomingMsg = &_session->GetIncomingMsgMgr();
    _incomingMsg->SetReaderCigiVersion(4, 0);
    _incomingMsg->UsingIteration(false);
    _sofProcessor.SetOrigPckt(&_sofPacket);
    _incomingMsg->RegisterEventProcessor(CIGI_SOF_PACKET_ID_V4, static_cast<CigiBaseEventProcessor*>(&_sofProcessor));

    _outgoingMsg = &_session->GetOutgoingMsgMgr();
    _outgoingMsg->BeginMsg();

    _igCtrlPacket.SetIGMode(CigiBaseIGCtrl::Operate);
    _igCtrlPacket.SetTimeStampValid(false);
    _igCtrlPacket.SetFrameCntr(0);

    return true;
}

bool SynchronSystem::initializeIG()
{
    auto* igSession = new CigiIGSession(1, 32768, 2, 32768);
    _session = igSession;
    _session->SetCigiVersion(4, 0);
    _session->SetSynchronous(true);

    _incomingMsg = &_session->GetIncomingMsgMgr();
    _incomingMsg->SetReaderCigiVersion(4, 0);
    _incomingMsg->UsingIteration(false);
    _igCtrlProcessor.SetOrigPckt(&_igCtrlPacket);
    _incomingMsg->RegisterEventProcessor(CIGI_IG_CTRL_PACKET_ID_V4, static_cast<CigiBaseEventProcessor*>(&_igCtrlProcessor));

    _outgoingMsg = &_session->GetOutgoingMsgMgr();
    _outgoingMsg->BeginMsg();

    _sofPacket.SetDatabaseID(0);
    _sofPacket.SetIGStatus(0);
    _sofPacket.SetIGMode(CigiBaseSOF::Operate);
    _sofPacket.SetTimeStampValid(false);
    _sofPacket.SetEarthRefModel(CigiBaseSOF::WGS84);
    _sofPacket.SetTimeStamp(0);
    _sofPacket.SetFrameCntr(0);

    return true;
}

void SynchronSystem::startHostBeacon()
{
    stopHostBeacon();
    _stopHostBeacon = false;
    _hostBeaconThread = std::thread(&SynchronSystem::hostBeaconLoop, this);
}

void SynchronSystem::stopHostBeacon()
{
    _stopHostBeacon = true;
    if (_hostBeaconThread.joinable())
        _hostBeaconThread.join();
}

void SynchronSystem::hostBeaconLoop()
{
    while (!_stopHostBeacon)
    {
        sendHostIgCtrl();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

bool SynchronSystem::sendHostIgCtrl()
{
    if (!_outgoingMsg)
        return false;

    try
    {
        QueryPerformanceCounter(&_timeStampEnd);
        const LONGLONG freq = (_timerFreq.QuadPart > 0) ? _timerFreq.QuadPart : 1;
        const auto stamp = static_cast<unsigned long>(
            (_timeStampEnd.QuadPart - _timeStampStart.QuadPart) * 1000 / freq);
        _igCtrlPacket.SetTimeStamp(stamp);
        _igCtrlPacket.SetTimeStampValid(true);
        _igCtrlPacket.SetFrameCntr(_frameCounter++);

        *_outgoingMsg << _igCtrlPacket;
        _outgoingMsg->PackageMsg(&_outgoingBuffer, _outgoingBufferSize);
        _outgoingMsg->UpdateIGCtrl(_outgoingBuffer, nullptr);

        const int sentBytes = _network.send(_outgoingBuffer, _outgoingBufferSize);
        _outgoingMsg->FreeMsg();
        return sentBytes > 0;
    }
    catch (CigiException& theException)
    {
        std::cerr << "SynchronSystem host beacon exception: " << theException.what() << std::endl;
        return false;
    }
}

bool SynchronSystem::waitForHostPacket(int timeoutMs)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline)
    {
        _incomingBufferSize = _network.recv(_incomingBuffer, RECV_BUFFER_SIZE);
        if (_incomingBufferSize > 0)
            return true;

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

bool SynchronSystem::Connect()
{
    if (!_initialized || _config.type != HostIGType::IG)
        return false;

    if (_connected)
        return true;

    if (!waitForHostPacket(ConnectTimeoutMs))
        return false;

    try
    {
        _incomingMsg->ProcessIncomingMsg(_incomingBuffer, _incomingBufferSize);
    }
    catch (CigiException& theException)
    {
        std::cerr << "SynchronSystem Connect exception: " << theException.what() << std::endl;
        return false;
    }

    _connected = true;
    return true;
}

void SynchronSystem::Update()
{
    if (!_initialized)
        return;

    if (_config.type == HostIGType::HOST)
    {
        // Host beacon thread owns outbound IGCtrl while connected setup is in progress.
        // Full host frame loop will replace this later.
        return;
    }

    if (_incomingBufferSize > 0)
    {
        try
        {
            _incomingMsg->ProcessIncomingMsg(_incomingBuffer, _incomingBufferSize);
        }
        catch (CigiException& theException)
        {
            std::cout << "getNetMessages - Exception: " << theException.what() << std::endl;
        }
    }

    _sofPacket.SetFrameCntr(_frameCounter++);
    QueryPerformanceCounter(&_timeStampEnd);
    _sofPacket.SetTimeStamp(static_cast<unsigned long>(
        (_timeStampEnd.QuadPart - _timeStampStart.QuadPart) * 1000 / _timerFreq.QuadPart));
    _sofPacket.SetTimeStampValid(true);

    *_outgoingMsg << _sofPacket;

    waitUntilBeginningOfFrame();

    try
    {
        _outgoingMsg->PackageMsg(&_outgoingBuffer, _outgoingBufferSize);
    }
    catch (CigiException& theException)
    {
        std::cout << "getNetMessages - Exception: " << theException.what() << std::endl;
    }

    _outgoingMsg->UpdateSOF(_outgoingBuffer, _incomingBuffer);
    _network.send(_outgoingBuffer, _outgoingBufferSize);
    _outgoingMsg->FreeMsg();

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
                std::cout << "Did not receive IG Control ";
                std::cout << c++;
                std::cout << "\n";
                RcvrProc = true;
                _incomingBufferSize = 0;
            }
        }
    }
}

void SynchronSystem::Shutdown()
{
    stopHostBeacon();

    if (_initialized || _session)
        _network.closeSocket();

    delete _session;
    _session = nullptr;
    _outgoingMsg = nullptr;
    _incomingMsg = nullptr;
    _outgoingBuffer = nullptr;
    _incomingBufferSize = 0;
    _outgoingBufferSize = 0;
    _initialized = false;
    _connected = false;
}

void SynchronSystem::waitUntilBeginningOfFrame()
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
        t2 = GetTickCount();
    } while ((t2 - t1) < static_cast<DWORD>(_timeDelayLimit));

    t1 = t2;
}
