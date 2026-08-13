// 最小接入示例：另一个 vsg 项目如何复用 aerovistaSync。
//
// 宿主只需做三件事：
//   1. 创建 SynchronSystem 并 initialize（传 SyncRoleConfig）；
//   2. 每帧调用 update()，用 takePendingCameraPose() 取位姿驱动自己的相机；
//   3. Host 引擎在 update 前把当前相机 LookAt 喂给 captureAuthorityEye()。
//
// 本示例不启动真实渲染循环，只演示 sync 库的数据流接入契约。
// 构建：add_subdirectory(aerovistaSync) 后链接。

#include <vsg/all.h>

#include "SyncConfig.h"
#include "SynchronSystem.h"

int main()
{
    // 1) 相机（宿主自己的场景对象）
    vsg::ref_ptr<vsg::Camera> camera = vsg::Camera::create();
    camera->viewMatrix = vsg::LookAt::create(
        vsg::dvec3(0.0, 0.0, 10.0), vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 0.0, 1.0));

    // 2) sync 门面：作为 IG 连接 Host（或 enableHost 做 Host）
    SyncRoleConfig role;
    role.enableIg = true;
    role.igConfig.bindAddr = "127.0.0.1";
    role.igConfig.udpPortSend = 0;
    role.igConfig.udpPortRecv = 8003;
    role.igConfig.targetAddr = "127.0.0.1";
    role.igConfig.targetTcpPort = 8100;
    role.igConfig.targetUdpPortRecv = 8001;

    auto sync = SynchronSystem::create();
    if (!sync->initialize(role, /*requireIgConnect=*/false))
        return 1;

    // 场景模式注入（本地模式传 false、空椭球；椭球模式传 true + EllipsoidModel）。
    sync->setSceneIsEllipsoid(false);
    sync->setEllipsoidModel({});
    sync->setChannelId(1);

    // 3) 帧驱动：采样(仅 Host) → update → 应用位姿 → postFrame
    for (int frame = 0; frame < 100; ++frame)
    {
        if (sync->hasHost())
        {
            // Host 引擎：覆盖前把当前相机 LookAt 喂给采样（防回声由门面处理）。
            if (auto lookAt = camera->viewMatrix.cast<vsg::LookAt>())
                sync->captureAuthorityEye(*lookAt);
        }

        sync->update();

        // 取本帧应写相机的位姿，宿主自己驱动相机（每帧一次）。
        if (auto pose = sync->takePendingCameraPose())
        {
            if (pose->frame == HostEyeCoordFrame::LLA)
            {
                // 需要椭球模型 → ECEF LookAt（此处省略，见 SynchronSystem 语义）。
                (void)pose;
            }
            else
            {
                const vsg::dvec3 forward =
                    vsg::dquat(vsg::radians(pose->eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0)) *
                    vsg::dquat(vsg::radians(pose->eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) *
                    vsg::dquat(vsg::radians(pose->eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0)) *
                    vsg::dvec3(0.0, 1.0, 0.0);
                camera->viewMatrix = vsg::LookAt::create(
                    pose->position, pose->position + forward, vsg::dvec3(0.0, 0.0, 1.0));
            }
        }

        sync->postFrame(frame * 16.667);
    }

    sync->shutdown();
    return 0;
}
