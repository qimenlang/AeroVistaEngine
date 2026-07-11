#pragma once

#include <vsg/all.h>

class RenderingEngine
{
public:
    VkExtent2D extent{2048, 1024};
    VkFormat imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depthFormat = VK_FORMAT_D32_SFLOAT;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool useDepthBuffer = true;
    bool above = false;
    bool enableGeometryShader = false;

    bool init(const vsg::Path& modelPath);
    bool run(const vsg::Path& outputPngPath);

private:
    vsg::ref_ptr<vsg::Options> options;
    vsg::ref_ptr<vsg::Node> scene;
    vsg::ref_ptr<vsg::Device> device;
    vsg::ref_ptr<vsg::Viewer> viewer;
    vsg::ref_ptr<vsg::Image> copiedColorBuffer;

    VkExtent2D currentExtent{};
};
