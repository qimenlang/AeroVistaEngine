#include "EventProcess.h"
#include <iostream>

void IGCtrl::OnPacketReceived(CigiBasePacket* packet)
{

    CigiIGCtrlV4* inPacket = (CigiIGCtrlV4*)packet;

    std::cout << "===> IGCtrl <===" << std::endl;
    std::cout << "Version ==> " << inPacket->GetVersion() << std::endl;
    std::cout << "MinorVersion ==> " << inPacket->GetMinorVersion() << std::endl;
    std::cout << "DatabaseID ==> " << inPacket->GetDatabaseID() << std::endl;
    std::cout << "IGMode ==> " << inPacket->GetIGMode() << std::endl;
    std::cout << "TimestampValid ==> " << inPacket->GetTimeStampValid() << std::endl;
    std::cout << "FrameCntr ==> " << inPacket->GetFrameCntr() << std::endl;
    std::cout << "TimeStampV4 ==> " << inPacket->GetTimeStamp() << std::endl;

    std::cout << "Last Recd IG Frame  ==> " << inPacket->GetLastRcvdIGFrame() << std::endl;

    if (inPacket->GetSmoothingEn())
        std::cout << "Smoothing Enabled" << std::endl;
    else
        std::cout << "Smoothing Disabled" << std::endl;
}
