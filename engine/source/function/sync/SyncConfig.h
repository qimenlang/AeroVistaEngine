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
    IDLE,
    RUNNING
};

enum class IgStatus
{
    IDLE,
    RUNNING
};

enum class SendPace
{
    FREE_RUN
};

enum class FrameGate
{
    FREE_RUN,
    BARRIER
};

struct SyncPaceConfig
{
    SendPace igCtrlSendPace = SendPace::FREE_RUN;
    FrameGate frameGate = FrameGate::FREE_RUN;
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
