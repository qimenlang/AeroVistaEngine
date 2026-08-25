#pragma once

#include <aerovista/sync/HostSync.h>

#include <vsg/nodes/Node.h>
#include <vsg/ui/KeyEvent.h>

/// 实机命令触发：Host 窗口 F9 → 文本指令 `load teapot`；F10 → 文本指令 `move 7 …`（id=7 +Y 0.1m），
/// 经新契约命令面（beginWithIgCtrl << SymbolTextDefV4 → flushTcp，fire-and-forget）扇出到所有 ready IG。
/// 状态同步设计初版.md §4.1 / §11：实机命令触发入口。
class CommandTriggerHandler : public vsg::Inherit<vsg::Visitor, CommandTriggerHandler>
{
public:
    aerovista::sync::HostSync* host = nullptr;

    void apply(vsg::KeyPressEvent& keyPress) override;
};
