#pragma once

#include <aerovista/sync/IgSync.h>

#include <vsg/nodes/Node.h>
#include <vsg/ui/KeyEvent.h>

#include <string>

/// 报文自检（IG 侧）：
/// - 接收：`bindRecvProbes` 订阅 Host→IG 全量报文，只记类名供 HUD「recv:」行；
/// - 发送：F9 → 随机一条 TCP 上行（IG→Host，16 类响应/通知）；F10 → SOF（UDP 上行）。
/// 与 viewhost testtcp/testudp（Host→IG 下行）对称。
class PacketProbeHandler : public vsg::Inherit<vsg::Visitor, PacketProbeHandler>
{
public:
    aerovista::sync::IgSync* ig = nullptr;
    std::string* lastSentName = nullptr;

    /// 订阅全部 Host→IG 报文，仅记录类名。与业务回调多播并存，不替代显隐/眼点处理。
    static void bindRecvProbes(aerovista::sync::IgSync& ig, std::string& lastReceivedName);

    void apply(vsg::KeyPressEvent& keyPress) override;
};
