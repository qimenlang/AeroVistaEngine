#pragma once

#include "function/sync/SynchronSystem.h"

#include <vsg/nodes/Node.h>
#include <vsg/ui/KeyEvent.h>

/// 实机命令触发：Host 窗口 F3 → LOAD+PLACE(id=7, teapot)；F4 → MOVEMODEL(id=7, +Y 0.1m)，到所有 ready IG。
/// sendCommand 阻塞等 RECEIVED（IG 在线时毫秒级返回），低频命令可接受。
/// 状态同步设计初版.md §11：实机命令触发入口。
class CommandTriggerHandler : public vsg::Inherit<vsg::Visitor, CommandTriggerHandler>
{
public:
    SynchronSystem* synchronSystem = nullptr;

    void apply(vsg::KeyPressEvent& keyPress) override;
};
