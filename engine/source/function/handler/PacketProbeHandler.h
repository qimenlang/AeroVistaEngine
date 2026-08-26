#pragma once

#include <aerovista/sync/IgSync.h>

#include <vsg/nodes/Node.h>
#include <vsg/ui/KeyEvent.h>

#include <string>

/// 报文自检发送（IG 侧）：F9 → 随机发一条 TCP 上行报文（IG→Host，16 类响应/通知类）；
/// F10 → 显式发 SOF（UDP 上行，IG→Host UDP 仅此一种，cigi梳理.md 链路矩阵）。
/// 与 viewhost testtcp/testudp（Host→IG 下行）对称，验证双向链路支持。
/// 发送报文类名写入 lastSentName 供 engine HUD「send: <类名>」显示。
/// 原废弃 CommandTriggerHandler（Host 命令触发，随拆 Host 失效）改造而来，2026-08。
class PacketProbeHandler : public vsg::Inherit<vsg::Visitor, PacketProbeHandler>
{
public:
    aerovista::sync::IgSync* ig = nullptr;
    std::string* lastSentName = nullptr;

    void apply(vsg::KeyPressEvent& keyPress) override;
};
