#pragma once

#include <vsg/all.h>

#include "function/config/EngineConfig.h"
#include "function/sync/SyncConfig.h"
#include "function/sync/SynchronSystem.h"
#include "vsg/core/ref_ptr.h"

#include <string>

class Engine
{
public:
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
    /// True after initSceneMode/initGraphics if scene has EllipsoidModel (lla §2 / §4.5).
    bool sceneHasEllipsoidModel() const { return _sceneHasEllipsoidModel; }
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

    /// World / channel camera used by Trackball and Host→IG pose sync (not HUD).
    vsg::ref_ptr<vsg::Camera> mainCamera() const;
    /// Set LookAt from world position + Euler YPR degrees (yaw,pitch,roll; Y-forward, Z-up).
    bool setCameraPose(const vsg::dvec3& position, const vsg::dvec3& eulerYprDeg);
    /// Set LookAt from LLA (lat°, lon°, alt m) + local ENU YPR degrees. Requires scene EllipsoidModel.
    bool setCameraPoseLla(const vsg::dvec3& lla, const vsg::dvec3& eulerYprDeg);

private:
    void applyConfigToEngine();

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
    bool _sceneHasEllipsoidModel = false;
    double _lastFrameSeconds = 0.0;
    double _syncSimTimeMs = 0.0;

    vsg::ref_ptr<SynchronSystem> _synchronSystem;
};
