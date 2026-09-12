#pragma once

#include <vsg/all.h>

#include "function/config/EngineConfig.h"
#include "function/driver/CameraDriver.h"
#include "function/entity/Entity.h"
#include "vsg/core/ref_ptr.h"
#include <aerovista/sync/SyncConfig.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

class CigiEntityCtrlV4;
class CigiEntityPositionCtrlV4;

namespace aerovista::sync
{
    class SynchronSystem;
}

class Engine
{
public:
    // ===== 门面 =====

    Engine();
    ~Engine();

    EngineChannelConfig config{};

    VkExtent2D extent{1920, 1080};
    VkFormat imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    bool showWindow = true;

    /// 从 argv 解析配置路径：`-c path` 或默认 `RESOURCE_DIR/config/default.json`。
    static std::string resolveConfigPath(int argc, char** argv);

    bool loadConfig(const std::string& path);

    /// 从当前 `config` 初始化（同步 + 图形）。
    bool init();
    bool init(const vsg::Path& modelPath);
    bool init(const vsg::Path& modelPath, const std::optional<aerovista::sync::IgConfig>& igConfig);

    bool initGraphics(const vsg::Path& modelPath);
    /// 场景 EllipsoidModel（lla位姿传输设计.md §2 / §4.5）；本地模式且模型无椭球时为空。
    vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel() const;

    /// 一帧：preFrame → update → render → postFrame。
    bool tickOnFrame();
    bool captureToFile(const vsg::Path& outputPngPath);
    void run();

    aerovista::sync::SynchronSystem& synchronSystem();
    bool hasGraphics() const { return static_cast<bool>(_viewer); }

    /// 屏上 vsg 窗口（showWindow=false / offscreen 时为空）。
    vsg::ref_ptr<vsg::Window> mainWindow() const;

    /// 场景根节点（测试计算 AABB 用，见 位姿配置设计.md §4）。
    vsg::ref_ptr<vsg::Node> mainScene() const { return _scene; }

    /// Trackball 与 Host→IG 位姿同步共用的世界 / 通道相机（非 HUD）。
    vsg::ref_ptr<vsg::Camera> mainCamera() const;
    /// 从世界位置 + 欧拉 YPR 度写 LookAt（yaw,pitch,roll；Y-forward, Z-up）。
    bool setCameraPose(const vsg::dvec3& position, const vsg::dvec3& eulerYprDeg);
    /// 从 LLA（纬度°、经度°、海拔 米）+ 当地 ENU YPR 度写 LookAt。要求场景 EllipsoidModel。
    bool setCameraPoseLla(const vsg::dvec3& lla, const vsg::dvec3& eulerYprDeg);

    /// 已预建实体数（启动表大小）。
    std::size_t entitySize() const;
    /// 按 id 查表；不在表内为空。
    Entity* findEntity(int id);
    const Entity* findEntity(int id) const;

    /// 应用 EntityPositionCtrlV4 命令实体摆放（EntityID≠0，实体与运动控制设计.md §4.2）。
    /// CCL 订阅与测试注入共用；眼点（EntityID==0）由卫语句过滤（走 CameraDriver）。
    void onEntityPose(const CigiEntityPositionCtrlV4& pose);
    /// 应用 EntityCtrlV4 切显隐 + 套属性（实体与运动控制设计.md §9）；不建实例、不加载、不编译。
    /// CCL 订阅与测试注入共用。
    void onEntityCtrl(const CigiEntityCtrlV4& ctrl);

    // ===== 测试接口 =====

    /// 仅同步平面（无 Vulkan）。设备数受限的多 IG 测试用。`igConfig` 空 = 不启 IG。
    bool initSync(const std::optional<aerovista::sync::IgConfig>& igConfig, bool requireConnectedIg = true);
    /// 仅同步平面，装配配置完整传入（含 channelId / offsetDeg / requireConnectedIg）。
    bool initSync(const std::optional<aerovista::sync::IgConfig>& igConfig,
                  const aerovista::sync::SyncSystemConfig& syncSystem);
    /// 加载场景并按配置注入 EllipsoidModel（不创建 Vulkan Device）。
    bool initSceneMode(const vsg::Path& modelPath);
    /// preFrame → stepSync → postFrame，不渲染（仅同步引擎）。
    void tickSync();
    /// 一步同步（不含采样/render）：把 CameraDriver 末次合成位姿应用到相机。
    /// 测试与 tickSync 使用；真实帧循环在 update() 内完成同样的应用。
    void stepSync();

    /// 眼点相机驱动器（收包即合成，结果由 update/stepSync 写相机）。
    /// Engine 值成员；眼点回调由 registerIgCallbacks 转发。
    CameraDriver& cameraDriver() { return _cameraDriver; }
    const CameraDriver& cameraDriver() const { return _cameraDriver; }

private:
    void applyConfigToEngine();
    /// 注册 IG 业务回调（眼点 / 命令实体）；报文自检订阅走 PacketProbeHandler::bindRecvProbes。
    void registerIgCallbacks();
    bool initSceneFromEntities(const std::vector<EntityConfig>& entities);
    bool ensureEllipsoidModel();
    void applyCameraPoseFromConfig();
    bool finishGraphicsAfterScene(vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel);

    /// 有末次合成眼点且已建图形时写入主相机（恒 LLA）。
    void applyLastHostEye();

    /// 帧相位：按固定顺序编排子系统与 viewer。
    void preFrame();
    bool update();
    void render();
    void postFrame();

    /// Frame-stat line "IGCtrl: <frame>:<sec>,<ms>,<us>"（或 "---" 未连接）。
    std::string frameStatsIgCtrlLine() const;

    void resetGraphicsResources();
    bool createVulkanDevice(int& queueFamily);
    vsg::ref_ptr<vsg::LookAt> createInitialLookAt(vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel,
                                                  const vsg::dvec3& centre, double radius) const;
    vsg::ref_ptr<vsg::ProjectionMatrix> createInitialProjection(
        vsg::ref_ptr<vsg::LookAt> lookAt, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel, double radius,
        double nearFarRatio) const;
    vsg::ref_ptr<vsg::CommandGraph> buildCommandGraph(vsg::ref_ptr<vsg::Camera> camera,
                                                      vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel,
                                                      vsg::ref_ptr<vsg::RenderGraph> offscreenRenderGraph,
                                                      vsg::ref_ptr<vsg::Commands> colorBufferCapture,
                                                      int queueFamily);

    vsg::ref_ptr<vsg::Options> _options;
    vsg::ref_ptr<vsg::Node> _scene;
    vsg::ref_ptr<vsg::Device> _device;
    vsg::ref_ptr<vsg::Viewer> _viewer;
    vsg::ref_ptr<vsg::Image> _copiedColorBuffer;
    vsg::ref_ptr<vsg::Window> _window;
    vsg::ref_ptr<vsg::Camera> _mainCamera;

    vsg::ref_ptr<vsg::Text> _frameStatsText;
    vsg::ref_ptr<vsg::stringValue> _frameStatsLabel;
    vsg::ref_ptr<vsg::Switch> _frameStatsSwitch;

    /// 最近收到的 Host→IG 测试报文类名（viewhost testtcp/testudp 报文自检，HUD 显示）。
    std::string _lastReceivedPacketName;
    /// 最近发送的 IG→Host 测试报文类名（F9/F10 报文自检，HUD「send」行显示）。
    std::string _lastSentPacketName;

    VkExtent2D _currentExtent{};
    bool _hasRenderedFrame = false;
    bool _reportFrameStats = false;
    double _lastFrameSeconds = 0.0;

    // 初始相机与投影调整用的 AABB 边界（位姿配置设计.md §4）
    vsg::dvec3 _aabbCentre{0.0, 0.0, 0.0};
    double _aabbRadius = 0.0;

    std::unique_ptr<aerovista::sync::SynchronSystem> _synchronSystem;
    CameraDriver _cameraDriver;
    /// 实体表：id → Entity（启动预建与运行期 EntityCtrl / EntityPositionCtrl 共用）。
    std::unordered_map<int, Entity> _entityMap;
};
