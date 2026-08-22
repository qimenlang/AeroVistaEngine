#include "CommandTriggerHandler.h"

#include <aerovista/sync/HostSync.h>

#include "CigiSymbolTextDefV4.h"

#include <iostream>

using aerovista::sync::HostSync;

void CommandTriggerHandler::apply(vsg::KeyPressEvent& keyPress)
{
    if (!host)
        return;

    if (keyPress.keyBase == vsg::KEY_F9)
    {
        // 新契约命令面：通用文本指令（SymbolTextDefV4，fire-and-forget），IG 按首 token 分发。
        auto& tcp = host->tcpOutgoing();
        CigiSymbolTextDefV4 load("load teapot");
        tcp << load;
        host->flushTcp();
        std::cout << "[Cmd] F9 load teapot (text command, fire-and-forget)" << std::endl;
        return;
    }

    if (keyPress.keyBase == vsg::KEY_F10)
    {
        auto& tcp = host->tcpOutgoing();
        CigiSymbolTextDefV4 move("move 7 0.0 0.1 0.0 0.0 0.0 0.0"); // id=7 +Y 0.1m
        tcp << move;
        host->flushTcp();
        std::cout << "[Cmd] F10 move 7 +Y 0.1m (text command, fire-and-forget)" << std::endl;
    }
}
