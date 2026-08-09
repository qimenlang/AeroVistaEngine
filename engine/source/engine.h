#pragma once

#include <vsg/all.h>

#include "function/config/EngineConfig.h"
#include "function/sync/SyncConfig.h"
#include "function/sync/SynchronSystem.h"
#include "vsg/core/ref_ptr.h"

#include <cstddef>
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
        bool hasLocalPose = false;
        bool hasEllipsoidPose = false;
        vsg::dvec3 localPosition{};
        vsg::dvec3 localYpr{};
        vsg::dvec3 ellipsoidLla{};
        vsg::dvec3 ellipsoidYpr{};
        vsg::ref_ptr<vsg::Node> node;
        vsg::ref_ptr<vsg::MatrixTransform> transform;
    };

    Engine();
    ~Engine();

    EngineChannelConfig config{};

    VkExtent2D extent{1920, 1080};
    VkFormat imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    bool showWindow = true;

    /// Resolve config path from argv: `-c path` or default `RESOURCE_DIR/config/default.json`.
    static std::string resolveConfigPath(int argc, char** argv);

    bool loadConfig(const std::string& path);

    /// Initialize from current `config` (sync + graphics).
    bool init();
    bool init(const vsg::Path& modelPath);
    bool init(const vsg::Path& modelPath, const SyncRoleConfig& syncRole);

    /// Sync plane only (no Vulkan). For multi-IG tests when device count is limited.
    bool initSync(const SyncRoleConfig& syncRole, bool requireIgConnect = true);
    /// Load/inject EllipsoidModel for sync mode checks without creating a Vulkan Device.
    bool initSceneMode(const vsg::Path& modelPath);
    bool initGraphics(const vsg::Path& modelPath);
    /// Scene EllipsoidModel if present (lla §2 / §4.5); null when Local without model ellipsoid.
    vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel() const;

    /// One frame: preFrame → update → render → postFrame.
    bool tickOnFrame();
    /// preFrame + postFrame without rendering (sync-only engines).
    void tickSync();
    bool captureToFile(const vsg::Path& outputPngPath);
    void run();

    SynchronSystem& synchronSystem();
    bool hasGraphics() const { return static_cast<bool>(_viewer); }

    /// On-screen vsg window (null when showWindow=false / offscreen).
    vsg::ref_ptr<vsg::Window> mainWindow() const;

    /// Scene root node (for tests to compute AABB, see 位姿配置设计.md §4).
    vsg::ref_ptr<vsg::Node> mainScene() const { return _scene; }

    /// World / channel camera used by Trackball and Host→IG pose sync (not HUD).
    vsg::ref_ptr<vsg::Camera> mainCamera() const;
    /// Set LookAt from world position + Euler YPR degrees (yaw,pitch,roll; Y-forward, Z-up).
    bool setCameraPose(const vsg::dvec3& position, const vsg::dvec3& eulerYprDeg);
    /// Set LookAt from LLA (lat°, lon°, alt m) + local ENU YPR degrees. Requires scene EllipsoidModel.
    bool setCameraPoseLla(const vsg::dvec3& lla, const vsg::dvec3& eulerYprDeg);

    bool sampleEntityPoseById(int id, vsg::dvec3& positionOrLla, vsg::dvec3& eulerYprDeg) const;
    std::size_t entitySize() const;
    bool hasEntityId(int id) const;
    bool entityName(int id, std::string& outName) const;
    vsg::ref_ptr<vsg::MatrixTransform> entityTransform(int id) const;

private:
    void applyConfigToEngine();
    bool initGraphicsFromEntities();
    bool assembleEntitiesScene();
    bool ensureEllipsoidModelForFrame();
    void applyCameraPoseFromConfig();
    vsg::dmat4 makeEntityMatrix(const EntityConfig& cfg, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid) const;
    bool finishGraphicsAfterScene(vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel);

    /// Frame phases: orchestrate subsystems and viewer in fixed order.
    void preFrame();
    bool update();
    void render();
    void postFrame();

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

    VkExtent2D _currentExtent{};
    bool _hasRenderedFrame = false;
    bool _reportFrameStats = false;
    double _lastFrameSeconds = 0.0;
    /// 模拟时间轴（时钟同步方案.md §5 方案 B）：基于 steady_clock 连续推进，
    /// 语义 = _simStartMs + (steady_clock::now() - _simStartTime)。不随渲染卡顿滞后。
    std::chrono::steady_clock::time_point _simStartTime{};
    double _simStartMs = 0.0;

    // AABB bounds for initial camera and projection adjustment (位姿配置设计.md §4)
    vsg::dvec3 _aabbCentre{0.0, 0.0, 0.0};
    double _aabbRadius = 0.0;

    vsg::ref_ptr<SynchronSystem> _synchronSystem;
    std::unordered_map<int, Entity> _entitiesById;
};
