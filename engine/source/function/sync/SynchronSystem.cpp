#include "SynchronSystem.h"
#include <iostream>

void SynchronSystem::Initialize()
{
    addr = "127.0.0.1";
    portSend = 8001;
    portRecv = 8000;

    bool netstatus = network.openSocket(addr.c_str(), portSend, portRecv);

    if (!netstatus)
    {
        std::cerr << "could not connect to CIGI host server" << std::endl;
    }
    else
    {
        std::cout << "successfully connected to CIGI host server" << std::endl;
    }
}
void SynchronSystem::Update()
{
}
void SynchronSystem::Shutdown()
{
}