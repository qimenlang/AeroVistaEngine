#include "RenderingEngine.h"

#include <vsgXchange/all.h>

#include <cstring>
#include <iostream>

namespace
{

vsg::ref_ptr<vsg::RenderPass> createOffscreenRenderPass(vsg::Device* device, VkFormat imageFormat, VkFormat depthFormat, bool requiresDepthRead)
{
    auto colorAttachment = vsg::defaultColorAttachment(imageFormat);
    auto depthAttachment = vsg::defaultDepthAttachment(depthFormat);

    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    if (requiresDepthRead)
    {
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }

    vsg::RenderPass::Attachments attachments{colorAttachment, depthAttachment};

    vsg::AttachmentReference colorAttachmentRef = {};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    vsg::AttachmentReference depthAttachmentRef = {};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    vsg::SubpassDescription subpass = {};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachments.emplace_back(colorAttachmentRef);
    subpass.depthStencilAttachments.emplace_back(depthAttachmentRef);

    vsg::RenderPass::Subpasses subpasses{subpass};

    vsg::SubpassDependency colorDependency = {};
    colorDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    colorDependency.dstSubpass = 0;
    colorDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    colorDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    colorDependency.srcAccessMask = 0;
    colorDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    colorDependency.dependencyFlags = 0;

    vsg::SubpassDependency depthDependency = {};
    depthDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    depthDependency.dstSubpass = 0;
    depthDependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    depthDependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    depthDependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthDependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depthDependency.dependencyFlags = 0;

    vsg::RenderPass::Dependencies dependencies{colorDependency, depthDependency};

    return vsg::RenderPass::create(device, attachments, subpasses, dependencies);
}

vsg::ref_ptr<vsg::RenderPass> createOffscreenMultisampledRenderPass(vsg::Device* device, VkFormat imageFormat, VkFormat depthFormat, VkSampleCountFlagBits samples, bool requiresDepthRead)
{
    if (samples == VK_SAMPLE_COUNT_1_BIT)
    {
        return createOffscreenRenderPass(device, imageFormat, depthFormat, requiresDepthRead);
    }

    vsg::AttachmentDescription colorAttachment = {};
    colorAttachment.format = imageFormat;
    colorAttachment.samples = samples;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    vsg::AttachmentDescription resolveAttachment = {};
    resolveAttachment.format = imageFormat;
    resolveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    resolveAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolveAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    resolveAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    resolveAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    resolveAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    resolveAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    vsg::AttachmentDescription depthAttachment = {};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = samples;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    vsg::RenderPass::Attachments attachments{colorAttachment, resolveAttachment, depthAttachment};

    if (requiresDepthRead)
    {
        vsg::AttachmentDescription depthResolveAttachment = {};
        depthResolveAttachment.format = depthFormat;
        depthResolveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthResolveAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthResolveAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthResolveAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthResolveAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthResolveAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthResolveAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachments.push_back(depthResolveAttachment);
    }

    vsg::AttachmentReference colorAttachmentRef = {};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    vsg::AttachmentReference resolveAttachmentRef = {};
    resolveAttachmentRef.attachment = 1;
    resolveAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    vsg::AttachmentReference depthAttachmentRef = {};
    depthAttachmentRef.attachment = 2;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    vsg::SubpassDescription subpass;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachments.emplace_back(colorAttachmentRef);
    subpass.resolveAttachments.emplace_back(resolveAttachmentRef);
    subpass.depthStencilAttachments.emplace_back(depthAttachmentRef);

    if (requiresDepthRead)
    {
        vsg::AttachmentReference depthResolveAttachmentRef = {};
        depthResolveAttachmentRef.attachment = 3;
        depthResolveAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        subpass.depthResolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        subpass.stencilResolveMode = VK_RESOLVE_MODE_NONE;
        subpass.depthStencilResolveAttachments.emplace_back(depthResolveAttachmentRef);
    }

    vsg::RenderPass::Subpasses subpasses{subpass};

    vsg::SubpassDependency dependency = {};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependency.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    vsg::SubpassDependency dependency2 = {};
    dependency2.srcSubpass = 0;
    dependency2.dstSubpass = VK_SUBPASS_EXTERNAL;
    dependency2.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependency2.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    dependency2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    dependency2.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    vsg::RenderPass::Dependencies dependencies{dependency, dependency2};

    return vsg::RenderPass::create(device, attachments, subpasses, dependencies);
}

vsg::ref_ptr<vsg::ImageView> createColorImageView(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& renderExtent, VkFormat format, VkSampleCountFlagBits sampleCount)
{
    auto colorImage = vsg::Image::create();
    colorImage->imageType = VK_IMAGE_TYPE_2D;
    colorImage->format = format;
    colorImage->extent = VkExtent3D{renderExtent.width, renderExtent.height, 1};
    colorImage->mipLevels = 1;
    colorImage->arrayLayers = 1;
    colorImage->samples = sampleCount;
    colorImage->tiling = VK_IMAGE_TILING_OPTIMAL;
    colorImage->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    colorImage->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorImage->flags = 0;
    colorImage->sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    return vsg::createImageView(device, colorImage, VK_IMAGE_ASPECT_COLOR_BIT);
}

vsg::ref_ptr<vsg::ImageView> createDepthImageView(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& renderExtent, VkFormat format, VkSampleCountFlagBits sampleCount)
{
    auto depthImage = vsg::Image::create();
    depthImage->imageType = VK_IMAGE_TYPE_2D;
    depthImage->extent = VkExtent3D{renderExtent.width, renderExtent.height, 1};
    depthImage->mipLevels = 1;
    depthImage->arrayLayers = 1;
    depthImage->samples = sampleCount;
    depthImage->format = format;
    depthImage->tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImage->usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    depthImage->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthImage->flags = 0;
    depthImage->sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    return vsg::createImageView(device, depthImage, vsg::computeAspectFlagsForFormat(format));
}

std::pair<vsg::ref_ptr<vsg::Commands>, vsg::ref_ptr<vsg::Image>> createColorCapture(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& renderExtent, vsg::ref_ptr<vsg::Image> sourceImage, VkFormat sourceImageFormat)
{
    auto width = renderExtent.width;
    auto height = renderExtent.height;

    auto physicalDevice = device->getPhysicalDevice();

    VkFormat targetImageFormat = sourceImageFormat;

    VkFormatProperties srcFormatProperties;
    vkGetPhysicalDeviceFormatProperties(*(physicalDevice), sourceImageFormat, &srcFormatProperties);

    VkFormatProperties destFormatProperties;
    vkGetPhysicalDeviceFormatProperties(*(physicalDevice), VK_FORMAT_R8G8B8A8_UNORM, &destFormatProperties);

    bool supportsBlit = ((srcFormatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0) &&
                        ((destFormatProperties.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0);

    if (supportsBlit)
    {
        targetImageFormat = VK_FORMAT_R8G8B8A8_UNORM;
    }

    auto destinationImage = vsg::Image::create();
    destinationImage->imageType = VK_IMAGE_TYPE_2D;
    destinationImage->format = targetImageFormat;
    destinationImage->extent.width = width;
    destinationImage->extent.height = height;
    destinationImage->extent.depth = 1;
    destinationImage->arrayLayers = 1;
    destinationImage->mipLevels = 1;
    destinationImage->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    destinationImage->samples = VK_SAMPLE_COUNT_1_BIT;
    destinationImage->tiling = VK_IMAGE_TILING_LINEAR;
    destinationImage->usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    destinationImage->compile(device);

    auto deviceMemory = vsg::DeviceMemory::create(device, destinationImage->getMemoryRequirements(device->deviceID), VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

    destinationImage->bind(deviceMemory, 0);

    auto commands = vsg::Commands::create();

    auto transitionDestinationImageToDestinationLayoutBarrier = vsg::ImageMemoryBarrier::create(
        0,
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        destinationImage,
        VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

    auto transitionSourceImageToTransferSourceLayoutBarrier = vsg::ImageMemoryBarrier::create(
        VK_ACCESS_MEMORY_READ_BIT,
        VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        sourceImage,
        VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

    auto cmd_transitionForTransferBarrier = vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        transitionDestinationImageToDestinationLayoutBarrier,
        transitionSourceImageToTransferSourceLayoutBarrier);

    commands->addChild(cmd_transitionForTransferBarrier);

    if (supportsBlit)
    {
        VkImageBlit region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.srcOffsets[0] = VkOffset3D{0, 0, 0};
        region.srcOffsets[1] = VkOffset3D{static_cast<int32_t>(width), static_cast<int32_t>(height), 1};
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.dstOffsets[0] = VkOffset3D{0, 0, 0};
        region.dstOffsets[1] = VkOffset3D{static_cast<int32_t>(width), static_cast<int32_t>(height), 1};

        auto blitImage = vsg::BlitImage::create();
        blitImage->srcImage = sourceImage;
        blitImage->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        blitImage->dstImage = destinationImage;
        blitImage->dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        blitImage->regions.push_back(region);
        blitImage->filter = VK_FILTER_NEAREST;

        commands->addChild(blitImage);
    }
    else
    {
        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent.width = width;
        region.extent.height = height;
        region.extent.depth = 1;

        auto copyImage = vsg::CopyImage::create();
        copyImage->srcImage = sourceImage;
        copyImage->srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copyImage->dstImage = destinationImage;
        copyImage->dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        copyImage->regions.push_back(region);

        commands->addChild(copyImage);
    }

    auto transitionDestinationImageToMemoryReadBarrier = vsg::ImageMemoryBarrier::create(
        VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_IMAGE_LAYOUT_GENERAL,
        VK_QUEUE_FAMILY_IGNORED,
        VK_QUEUE_FAMILY_IGNORED,
        destinationImage,
        VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

    auto cmd_transitionFromTransferBarrier = vsg::PipelineBarrier::create(
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        transitionDestinationImageToMemoryReadBarrier);

    commands->addChild(cmd_transitionFromTransferBarrier);

    return {commands, destinationImage};
}

} // namespace

bool RenderingEngine::init(const vsg::Path& modelPath)
{
    currentExtent = extent;
    std::cout << "RenderingEngine::init: " <<__LINE__  << std::endl;

    options = vsg::Options::create();
    options->fileCache = vsg::getEnv("VSG_FILE_CACHE");
    options->paths = vsg::getEnvPaths("VSG_FILE_PATH");
    options->add(vsgXchange::all::create());

    scene = vsg::read_cast<vsg::Node>(modelPath, options);
    if (!scene)
    {
        std::cerr << "Failed to load model: " << modelPath << std::endl;
        return false;
    }

    std::cout << "RenderingEngine::init: " <<__LINE__  << std::endl;

    uint32_t vulkanVersion = VK_API_VERSION_1_0;
    if (samples != VK_SAMPLE_COUNT_1_BIT) vulkanVersion = VK_API_VERSION_1_2;

    auto instance = vsg::Instance::create(vsg::Names{}, vsg::Names{}, vulkanVersion);
    //queueFamily:硬件GPU 图形队列族的索引，包括渲染、计算、传输、显示等队列；
    auto [physicalDevice, queueFamily] = instance->getPhysicalDeviceAndQueueFamily(VK_QUEUE_GRAPHICS_BIT);
    if (!physicalDevice || queueFamily < 0)
    {
        std::cerr << "Could not create PhysicalDevice" << std::endl;
        vsg::Logger::instance()->error("Could not create PhysicalDevice");
        return false;
    }

    std::cout << "RenderingEngine::init: " <<__LINE__  << std::endl;

    vsg::QueueSettings queueSettings{vsg::QueueSetting{queueFamily, {1.0}}};

    //显式开启GPU特性
    auto deviceFeatures = vsg::DeviceFeatures::create();
    deviceFeatures->get().samplerAnisotropy = VK_TRUE;
    deviceFeatures->get().geometryShader = enableGeometryShader;

    device = vsg::Device::create(physicalDevice, queueSettings, vsg::Names{}, vsg::Names{}, deviceFeatures);

    vsg::ComputeBounds computeBounds;
    scene->accept(computeBounds);
    vsg::dvec3 centre = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
    double radius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.6;
    double nearFarRatio = 0.001;

    auto lookAt = above ? vsg::LookAt::create(centre + vsg::dvec3(0.0, 0.0, radius * 1.5), centre, vsg::dvec3(0.0, 1.0, 0.0))
                        : vsg::LookAt::create(centre + vsg::dvec3(0.0, -radius * 1.5, 0.0), centre, vsg::dvec3(0.0, 0.0, 1.0));

    vsg::ref_ptr<vsg::ProjectionMatrix> perspective;
    if (auto ellipsoidModel = scene->getRefObject<vsg::EllipsoidModel>("EllipsoidModel"))
    {
        perspective = vsg::EllipsoidPerspective::create(lookAt, ellipsoidModel, 30.0, static_cast<double>(currentExtent.width) / static_cast<double>(currentExtent.height), nearFarRatio, 0.0);
    }
    else
    {
        perspective = vsg::Perspective::create(30.0, static_cast<double>(currentExtent.width) / static_cast<double>(currentExtent.height), nearFarRatio * radius, radius * 4.5);
    }

    auto camera = vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(currentExtent));
    std::cout << "RenderingEngine::init: " <<__LINE__  << std::endl;

    vsg::ref_ptr<vsg::Framebuffer> framebuffer;
    vsg::ref_ptr<vsg::ImageView> colorImageView;
    vsg::ref_ptr<vsg::ImageView> depthImageView;
    vsg::ref_ptr<vsg::Commands> colorBufferCapture;

    if (useDepthBuffer)
    {
        colorImageView = createColorImageView(device, currentExtent, imageFormat, VK_SAMPLE_COUNT_1_BIT);
        depthImageView = createDepthImageView(device, currentExtent, depthFormat, VK_SAMPLE_COUNT_1_BIT);
        if (samples == VK_SAMPLE_COUNT_1_BIT)
        {
            auto renderPass = createOffscreenRenderPass(device, imageFormat, depthFormat, false);
            framebuffer = vsg::Framebuffer::create(renderPass, vsg::ImageViews{colorImageView, depthImageView}, currentExtent.width, currentExtent.height, 1);
        }
        else
        {
            auto msaa_colorImageView = createColorImageView(device, currentExtent, imageFormat, samples);
            auto msaa_depthImageView = createDepthImageView(device, currentExtent, depthFormat, samples);

            auto renderPass = createOffscreenMultisampledRenderPass(device, imageFormat, depthFormat, samples, false);
            framebuffer = vsg::Framebuffer::create(renderPass, vsg::ImageViews{msaa_colorImageView, colorImageView, msaa_depthImageView, depthImageView}, currentExtent.width, currentExtent.height, 1);
        }

        std::tie(colorBufferCapture, copiedColorBuffer) = createColorCapture(device, currentExtent, colorImageView->image, imageFormat);
    }
    else
    {
        colorImageView = createColorImageView(device, currentExtent, imageFormat, VK_SAMPLE_COUNT_1_BIT);
        if (samples == VK_SAMPLE_COUNT_1_BIT)
        {
            auto renderPass = vsg::createRenderPass(device, imageFormat);
            framebuffer = vsg::Framebuffer::create(renderPass, vsg::ImageViews{colorImageView}, currentExtent.width, currentExtent.height, 1);
        }
        else
        {
            auto msaa_colorImageView = createColorImageView(device, currentExtent, imageFormat, samples);

            auto renderPass = vsg::createMultisampledRenderPass(device, imageFormat, samples);
            framebuffer = vsg::Framebuffer::create(renderPass, vsg::ImageViews{msaa_colorImageView, colorImageView}, currentExtent.width, currentExtent.height, 1);
        }

        std::tie(colorBufferCapture, copiedColorBuffer) = createColorCapture(device, currentExtent, colorImageView->image, imageFormat);
    }
    std::cout << "RenderingEngine::init: " <<__LINE__  << std::endl;


    auto renderGraph = vsg::RenderGraph::create();
    renderGraph->framebuffer = framebuffer;
    renderGraph->renderArea.offset = {0, 0};
    renderGraph->renderArea.extent = currentExtent;
    renderGraph->setClearValues({{1.0f, 1.0f, 0.0f, 0.0f}}, VkClearDepthStencilValue{0.0f, 0});

    auto view = vsg::View::create(camera, scene);
    view->addChild(vsg::createHeadlight());
    renderGraph->addChild(view);

    auto commandGraph = vsg::CommandGraph::create(device, queueFamily);
    commandGraph->addChild(renderGraph);
    if (colorBufferCapture) commandGraph->addChild(colorBufferCapture);

    viewer = vsg::Viewer::create();
    viewer->assignRecordAndSubmitTaskAndPresentation({commandGraph});
    viewer->compile();

    std::cout << "RenderingEngine::init: " <<__LINE__  << std::endl;

    return true;
}

bool RenderingEngine::run(const vsg::Path& outputPngPath)
{
    vsg::info("RenderingEngine::run:", __LINE__," line");

    if (!viewer || !copiedColorBuffer)
    {
        std::cerr << "Engine not initialized" << std::endl;
        return false;
    }

    vsg::info("RenderingEngine::run:", __LINE__," line");


    constexpr uint64_t waitTimeout = 1999999999;

    if (!viewer->advanceToNextFrame())
    {
        std::cerr << "Failed to advance to next frame" << std::endl;
        return false;
    }

    vsg::info("RenderingEngine::run:", __LINE__," line");

    viewer->handleEvents();
    viewer->update();
    viewer->recordAndSubmit();

    viewer->waitForFences(0, waitTimeout);

    VkImageSubresource subResource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout subResourceLayout;
    vkGetImageSubresourceLayout(*device, copiedColorBuffer->vk(device->deviceID), &subResource, &subResourceLayout);

    auto deviceMemory = copiedColorBuffer->getDeviceMemory(device->deviceID);

    
    size_t destRowWidth = currentExtent.width * sizeof(vsg::ubvec4);
    vsg::ref_ptr<vsg::Data> imageData;
    if (destRowWidth == subResourceLayout.rowPitch)
    {
        imageData = vsg::MappedData<vsg::ubvec4Array2D>::create(deviceMemory, subResourceLayout.offset, 0, vsg::Data::Properties{imageFormat}, currentExtent.width, currentExtent.height);
    }
    else
    {
        auto mappedData = vsg::MappedData<vsg::ubyteArray>::create(deviceMemory, subResourceLayout.offset, 0, vsg::Data::Properties{imageFormat}, subResourceLayout.rowPitch * currentExtent.height);
        imageData = vsg::ubvec4Array2D::create(currentExtent.width, currentExtent.height, vsg::Data::Properties{imageFormat});
        for (uint32_t row = 0; row < currentExtent.height; ++row)
        {
            std::memcpy(imageData->dataPointer(row * currentExtent.width), mappedData->dataPointer(row * subResourceLayout.rowPitch), destRowWidth);
        }
    }

    vsg::info("RenderingEngine::run:", __LINE__," line");

    if (!vsg::write(imageData, outputPngPath, options))
    {
        std::cerr << "Failed to write PNG: " << outputPngPath << std::endl;
        return false;
    }
    vsg::info("RenderingEngine::run:", __LINE__," line");


    return true;
}
