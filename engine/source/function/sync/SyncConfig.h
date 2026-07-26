#pragma once

#include <string>

struct AddressConfig
{
    std::string addr;
    int udpPortSend = 0;
    int udpPortRecv = 0;
    int tcpPort = 0;
};

enum class HostStatus
{
    Idle,
    Running
};

enum class IgStatus
{
    Idle,
    Running
};

enum class SendPace
{
    FreeRun
};

enum class FrameGate
{
    FreeRun,
    Barrier
};

struct SyncPaceConfig
{
    SendPace igCtrlSendPace = SendPace::FreeRun;
    FrameGate frameGate = FrameGate::FreeRun;
    double targetFps = 60.0;
    int barrierTimeoutMs = 8;
};

struct SyncRoleConfig
{
    bool enableHost = false;
    bool enableIg = false;
    AddressConfig hostLocal{};
    AddressConfig igLocal{};
    AddressConfig hostEndpoint{};
};
