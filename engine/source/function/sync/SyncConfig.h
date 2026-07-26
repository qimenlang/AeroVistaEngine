#pragma once

#include <string>

struct AddressConfig
{
    std::string addr;
    int udpPortSend = 0;
    int udpPortRecv = 0;
    int tcpPort = 0;
};
