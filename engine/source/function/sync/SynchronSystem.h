#pragma once
#include "Network.h"
#include <string>
#include <vsg/all.h>

class SynchronSystem : public vsg::Inherit<vsg::Object, SynchronSystem>
{
private:
    Network network;

    std::string addr;
    int portSend;
    int portRecv;

public:
    void Initialize();
    void Update();
    void Shutdown();
};