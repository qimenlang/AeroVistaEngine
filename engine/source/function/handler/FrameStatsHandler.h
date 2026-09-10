#pragma once

#include <vsg/nodes/Node.h>
#include <vsg/ui/KeyEvent.h>

/// 窗口热键处理器：F2 切换 HUD 帧统计显示。
class FrameStatsHandler : public vsg::Inherit<vsg::Visitor, FrameStatsHandler>
{
public:
    bool* enabled = nullptr;

    void apply(vsg::KeyPressEvent& keyPress) override;
};
