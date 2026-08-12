#include "FrameStatsHandler.h"

#include <iostream>

void FrameStatsHandler::apply(vsg::KeyPressEvent& keyPress)
{
    if (!enabled || keyPress.keyBase != vsg::KEY_F2)
        return;

    *enabled = !*enabled;
    std::cout << "[FrameStats] " << (*enabled ? "ON" : "OFF") << std::endl;
}
