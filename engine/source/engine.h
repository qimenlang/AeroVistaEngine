#pragma once

#include <vsg/all.h>

#include "function/config/EngineConfig.h"
#include "function/driver/CameraDriver.h"
#include "vsg/core/ref_ptr.h"
#include <aerovista/sync/SyncConfig.h>
#include <aerovista/sync/SynchronSystem.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

class Engine
{
public:
    struct Entity
    {
        int id = 0;
        std::string name;
        std::string path;
        /// true：`positionOrLla` 为 LLA、`eulerYprDeg` 为当地 ENU；false：本地笛卡尔。与场景有无椭球一致。
        bool ellipsoid = false;
        vsg::dvec3 positionOrLla{};
        vsg::dvec3 eulerYprDeg{};
        vsg::ref_ptr<vsg::Node> node;
        vsg::ref_ptr<vsg::MatrixTransform> transform;
        vsg::ref_ptr<vsg::Switch> visibility;
        /// CIGI Alpha：0 透明 .. 255 不透明；缺省不透明（报文未到前）。
        std::uint8_t alpha = 255;
        bool inheritAlpha = false;
        bool collisionDetectEn = false;
        bool smoothingEn = false;
    };

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

    /// 仅同步平面（无 Vulkan）。设备数受限的多 IG 测试用。`igConfig` 空 = 不启 IG。
    bool initSync(const std::optional<aerovista::sync::IgConfig>& igConfig, bool requireConnectedIg = true);
    /// 仅同步平面，装配配置完整传入（含 channelId / offsetDeg / hostEyeStalePolicy / requireConnectedIg）。
    bool initSync(const std::optional<aerovista::sync::IgConfig>& igConfig,
                  const aerovista::sync::SyncSystemConfig& syncSystem);
    /// 加载/注入 EllipsoidModel 以做同步模式检查（不创建 Vulkan Device）。
    bool initSceneMode(const vsg::Path& modelPath);
    bool initGraphics(const vsg::Path& modelPath);
    /// 场景 EllipsoidModel（lla §2 / §4.5）；本地模式且模型无椭球时为空。
    vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel() const;

    /// 一帧：preFrame → update → render → postFrame。
    bool tickOnFrame();
    /// preFrame + postFrame，不渲染（仅同步引擎）。
    void tickSync();
    /// 一步同步（不含采样/render）：Engine 帧级眼点决策后把本帧位姿应用到相机。
    /// 测试与 tickSync 使用；真实帧循环在 update() 内完成采样 + 应用。
    void stepSync();
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

    bool sampleEntityPoseById(int id, vsg::dvec3& positionOrLla, vsg::dvec3& eulerYprDeg) const;
    std::size_t entitySize() const;
    bool hasEntityId(int id) const;
    bool entityName(int id, std::string& outName) const;
    vsg::ref_ptr<vsg::MatrixTransform> entityTransform(int id) const;
    /// 显隐（读 Switch mask）；不在表内或无 Switch 为 false。启动初值与运行期 `EntityCtrl` 共用。
    bool entityVisible(int id) const;
    /// 运行时透明度（0 透明..255 不透明）；不在表内为空。
    std::optional<std::uint8_t> entityAlpha(int id) const;
    /// 实体几何 node（共享几何验收：同 model 两实体指针相同）。
    vsg::ref_ptr<vsg::Node> entityNode(int id) const;

    /// 眼点相机驱动器（2026-09 从 Engine 抽出；眼点→相机的业务策略：offset 合成 / stale / 断线决策）。
    /// Engine 持有并在 update/stepSync 每帧调 update()；眼点回调由 registerIgCallbacks 转发。
    CameraDriver& cameraDriver();

    /// 更新指定实体的位姿（命令面 Host→IG 摆放，恒 LLA）。回调主线程解包时调用，直接写 entityMap（主线程安全，§6）。
    void updateEntityPose(int id, const aerovista::sync::DVec3& lla, const aerovista::sync::DVec3& eulerYprDeg);

    /// 订阅回调：EntityPositionCtrlV4 命令实体摆放（EntityID≠0，§4.2）——同步层只 LLA，
    /// 走 updateEntityPose。addCallback 多播到 UDP/TCP 两条链路的通用捕获，眼点报文
    /// （EntityID==0）由本回调卫语句过滤（眼点走 CameraDriver，经 registerIgCallbacks 转发）。
    void onEntityPose(const CigiEntityPositionCtrlV4& pose);
    /// 订阅回调：EntityCtrlV4 切显隐 + 套属性（实体与运动控制设计.md §9）；不建实例、不加载、不编译。
    void onEntityCtrl(const CigiEntityCtrlV4& ctrl);

private:
    void applyConfigToEngine();
    /// 注册 IG 回调（眼点/命令实体分流 + 全量 Host→IG 报文自检订阅）；hasIg 时调用。
    void registerIgCallbacks();
    bool initSceneFromEntities(const std::vector<EntityConfig>& entities);
    void assembleEntity(vsg::Group& root, const EntityConfig& cfg, vsg::ref_ptr<vsg::Node> geometry,
                        vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid);
    bool ensureEllipsoidModel();
    void applyCameraPoseFromConfig();
    vsg::dmat4 makeEntityMatrix(const EntityConfig& cfg, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid) const;
    bool finishGraphicsAfterScene(vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel);

    /// 实体无 transform 时创建（挂 node，scene 存在则 addChild）。
    void ensureEntityTransform(Entity& entity);
    void recomputeEntityTransform(Entity& entity);

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
    /// 眼点相机驱动器（2026-09 从 Engine 抽出；initSync 创建，传 *this + *_synchronSystem）。
    std::unique_ptr<CameraDriver> _cameraDriver;
    /// 实体表：id → Entity（命令面 LOAD/PLACE 与配置实体共用）。
    std::unordered_map<int, Entity> _entityMap;
};
