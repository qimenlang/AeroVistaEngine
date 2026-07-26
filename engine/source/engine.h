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

    /// Resolve config path from argv: `-c path` or default `RESOURCE_DIR/config/main.json`.
    static std::string resolveConfigPath(int argc, char** argv);

    bool loadConfig(const std::string& path);

    /// Initialize from current `config` (sync + graphics).
    bool init();
    bool init(const vsg::Path& modelPath);
    bool init(const vsg::Path& modelPath, const SyncRoleConfig& syncRole);

    /// Sync plane only (no Vulkan). For multi-IG tests when device count is limited.
    bool initSync(const SyncRoleConfig& syncRole, bool requireIgConnect = true);
    bool initGraphics(const vsg::Path& modelPath);

    /// One frame: preFrame → update → render → postFrame.
    bool tickOnFrame();
    /// preFrame + postFrame without rendering (sync-only engines).
    void tickSync();
    bool CaptureToFile(const vsg::Path& outputPngPath);
    void run();

    SynchronSystem& synchronSystem();
    bool hasGraphics() const { return static_cast<bool>(viewer); }

    /// On-screen vsg window (null when showWindow=false / offscreen).
    vsg::ref_ptr<vsg::Window> mainWindow() const;

    /// World / channel camera used by Trackball and Host→IG pose sync (not HUD).
    vsg::ref_ptr<vsg::Camera> mainCamera() const;
    /// Set LookAt from world position + Euler YPR in degrees (yaw,pitch,roll; Y-forward, Z-up).
    bool setCameraPose(const vsg::dvec3& position, const vsg::dvec3& eulerYPR_deg);

private:
    void applyConfigToEngine();

    /// Frame phases: orchestrate subsystems and viewer in fixed order.
    void preFrame();
    bool update();
    void render();
    void postFrame();

    vsg::ref_ptr<vsg::Options> options;
    vsg::ref_ptr<vsg::Node> scene;
    vsg::ref_ptr<vsg::Device> device;
    vsg::ref_ptr<vsg::Viewer> viewer;
    vsg::ref_ptr<vsg::Image> copiedColorBuffer;
    vsg::ref_ptr<vsg::Window> window;
    vsg::ref_ptr<vsg::Camera> _mainCamera;

    vsg::ref_ptr<vsg::Text> frameStatsText;
    vsg::ref_ptr<vsg::stringValue> frameStatsLabel;
    vsg::ref_ptr<vsg::Switch> frameStatsSwitch;

    VkExtent2D currentExtent{};
    bool hasRenderedFrame = false;
    bool reportFrameStats = false;
    double lastFrameSeconds = 0.0;
    double _syncSimTimeMs = 0.0;

    vsg::ref_ptr<SynchronSystem> _synchronSystem;
};
