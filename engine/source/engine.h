#pragma once

#include <vsg/all.h>

#include "function/sync/SynchronSystem.h"
#include "vsg/core/ref_ptr.h"

class Engine
{
public:
    Engine();
    ~Engine() { _synchronSystem->Shutdown(); }
    VkExtent2D extent{1920, 1080};
    VkFormat imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    bool showWindow = true;

    bool init(const vsg::Path& modelPath);
    bool renderOneTick();
    bool CaptureToFile(const vsg::Path& outputPngPath);
    void run();

private:
    vsg::ref_ptr<vsg::Options> options;
    vsg::ref_ptr<vsg::Node> scene;
    vsg::ref_ptr<vsg::Device> device;
    vsg::ref_ptr<vsg::Viewer> viewer;
    vsg::ref_ptr<vsg::Image> copiedColorBuffer;
    vsg::ref_ptr<vsg::Window> window;

    vsg::ref_ptr<vsg::Text> frameStatsText;
    vsg::ref_ptr<vsg::stringValue> frameStatsLabel;
    vsg::ref_ptr<vsg::Switch> frameStatsSwitch;

    VkExtent2D currentExtent{};
    bool hasRenderedFrame = false;
    bool reportFrameStats = false;
    double lastFrameSeconds = 0.0;

    vsg::ref_ptr<SynchronSystem> _synchronSystem;
};
