#include "engine.h"

#include <vsgXchange/all.h>

#include <cstring>
#include <iostream>

namespace
{

vsg::ref_ptr<vsg::Node> createTextureQuad(vsg::ref_ptr<vsg::Data> sourceData, vsg::ref_ptr<vsg::Options> options)
{
    auto builder = vsg::Builder::create();
    builder->options = options;

    vsg::StateInfo state;
    state.image = sourceData;
    state.lighting = false;

    vsg::GeometryInfo geom;
    geom.dx.set(static_cast<float>(sourceData->width()), 0.0f, 0.0f);
    geom.dy.set(0.0f, 0.0f, static_cast<float>(sourceData->height()));
    geom.dz.set(0.0f, -1.0f, 0.0f);

    return builder->createQuad(geom, state);
}

vsg::ref_ptr<vsg::RenderPass> createOffscreenRenderPass(vsg::Device* device, VkFormat imageFormat, VkFormat depthFormat)
{
    auto colorAttachment = vsg::defaultColorAttachment(imageFormat);
    auto depthAttachment = vsg::defaultDepthAttachment(depthFormat);

    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

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

vsg::ref_ptr<vsg::ImageView> createColorImageView(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& renderExtent, VkFormat format)
{
    auto colorImage = vsg::Image::create();
    colorImage->imageType = VK_IMAGE_TYPE_2D;
    colorImage->format = format;
    colorImage->extent = VkExtent3D{renderExtent.width, renderExtent.height, 1};
    colorImage->mipLevels = 1;
    colorImage->arrayLayers = 1;
    colorImage->samples = VK_SAMPLE_COUNT_1_BIT;
    colorImage->tiling = VK_IMAGE_TILING_OPTIMAL;
    colorImage->usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    colorImage->initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorImage->flags = 0;
    colorImage->sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    return vsg::createImageView(device, colorImage, VK_IMAGE_ASPECT_COLOR_BIT);
}

vsg::ref_ptr<vsg::ImageView> createDepthImageView(vsg::ref_ptr<vsg::Device> device, const VkExtent2D& renderExtent, VkFormat format)
{
    auto depthImage = vsg::Image::create();
    depthImage->imageType = VK_IMAGE_TYPE_2D;
    depthImage->extent = VkExtent3D{renderExtent.width, renderExtent.height, 1};
    depthImage->mipLevels = 1;
    depthImage->arrayLayers = 1;
    depthImage->samples = VK_SAMPLE_COUNT_1_BIT;
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

bool loadScene(const vsg::Path& modelPath, vsg::ref_ptr<vsg::Options> options, vsg::ref_ptr<vsg::Node>& outScene)
{
    auto object = vsg::read(modelPath, options);
    if (auto node = object.cast<vsg::Node>())
    {
        outScene = node;
        return true;
    }

    if (auto data = object.cast<vsg::Data>())
    {
        outScene = createTextureQuad(data, options);
        return static_cast<bool>(outScene);
    }

    if (object)
        std::cerr << "Unable to view object of type " << object->className() << std::endl;
    else
        std::cerr << "Unable to load file " << modelPath << std::endl;

    return false;
}

} // namespace

bool Engine::init(const vsg::Path& modelPath)
{
    try
    {
        currentExtent = extent;
        hasRenderedFrame = false;
        window = {};

        options = vsg::Options::create();
        options->sharedObjects = vsg::SharedObjects::create();
        options->fileCache = vsg::getEnv("VSG_FILE_CACHE");
        options->paths = vsg::getEnvPaths("VSG_FILE_PATH");
        options->add(vsgXchange::all::create());

        if (!loadScene(modelPath, options, scene))
            return false;

        viewer = vsg::Viewer::create();

        int queueFamily = -1;
        if (showWindow)
        {
            auto windowTraits = vsg::WindowTraits::create(extent.width, extent.height, "AeroVistaEngine");
            windowTraits->hdpi = false;
            window = vsg::Window::create(windowTraits);
            if (!window)
            {
                std::cerr << "Could not create window." << std::endl;
                return false;
            }
            viewer->addWindow(window);
            device = window->getOrCreateDevice();
            currentExtent = window->extent2D();
        }
        else
        {
            auto instance = vsg::Instance::create(vsg::Names{}, vsg::Names{}, VK_API_VERSION_1_0);
            auto [physicalDevice, graphicsFamily] = instance->getPhysicalDeviceAndQueueFamily(VK_QUEUE_GRAPHICS_BIT);
            if (!physicalDevice || graphicsFamily < 0)
            {
                std::cerr << "Could not create PhysicalDevice." << std::endl;
                return false;
            }

            queueFamily = graphicsFamily;
            vsg::QueueSettings queueSettings{vsg::QueueSetting{queueFamily, {1.0}}};
            auto deviceFeatures = vsg::DeviceFeatures::create();
            deviceFeatures->get().samplerAnisotropy = VK_TRUE;
            device = vsg::Device::create(physicalDevice, queueSettings, vsg::Names{}, vsg::Names{}, deviceFeatures);
        }

        // Camera matches RenderingEngine defaults so golden regression images remain valid.
        vsg::ComputeBounds computeBounds;
        scene->accept(computeBounds);
        vsg::dvec3 centre = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
        double radius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.6;
        constexpr double nearFarRatio = 0.001;

        auto lookAt = vsg::LookAt::create(centre + vsg::dvec3(0.0, -radius * 1.5, 0.0), centre, vsg::dvec3(0.0, 0.0, 1.0));

        auto ellipsoidModel = scene->getRefObject<vsg::EllipsoidModel>("EllipsoidModel");
        vsg::ref_ptr<vsg::ProjectionMatrix> perspective;
        if (ellipsoidModel)
        {
            perspective = vsg::EllipsoidPerspective::create(lookAt, ellipsoidModel, 30.0,
                                                           static_cast<double>(currentExtent.width) / static_cast<double>(currentExtent.height),
                                                           nearFarRatio, 0.0);
        }
        else
        {
            perspective = vsg::Perspective::create(30.0,
                                                   static_cast<double>(currentExtent.width) / static_cast<double>(currentExtent.height),
                                                   nearFarRatio * radius, radius * 4.5);
        }

        auto camera = vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(currentExtent));

        auto colorImageView = createColorImageView(device, currentExtent, imageFormat);
        auto depthImageView = createDepthImageView(device, currentExtent, depthFormat);
        auto renderPass = createOffscreenRenderPass(device, imageFormat, depthFormat);
        auto framebuffer = vsg::Framebuffer::create(renderPass, vsg::ImageViews{colorImageView, depthImageView}, currentExtent.width, currentExtent.height, 1);

        vsg::ref_ptr<vsg::Commands> colorBufferCapture;
        std::tie(colorBufferCapture, copiedColorBuffer) = createColorCapture(device, currentExtent, colorImageView->image, imageFormat);

        auto offscreenRenderGraph = vsg::RenderGraph::create();
        offscreenRenderGraph->framebuffer = framebuffer;
        offscreenRenderGraph->renderArea.offset = {0, 0};
        offscreenRenderGraph->renderArea.extent = currentExtent;
        offscreenRenderGraph->setClearValues({{0.0f, 0.0f, 0.0f, 1.0f}}, VkClearDepthStencilValue{0.0f, 0});

        auto offscreenView = vsg::View::create(camera, scene);
        offscreenView->addChild(vsg::createHeadlight());
        offscreenRenderGraph->addChild(offscreenView);

        vsg::ref_ptr<vsg::CommandGraph> commandGraph;
        if (window)
        {
            viewer->addEventHandler(vsg::CloseHandler::create(viewer));
            viewer->addEventHandler(vsg::Trackball::create(camera, ellipsoidModel));

            auto windowView = vsg::View::create(camera);
            windowView->addChild(vsg::createHeadlight());
            windowView->addChild(scene);

            auto windowRenderGraph = vsg::RenderGraph::create(window, windowView);
            windowRenderGraph->setClearValues({{0.0f, 0.0f, 0.0f, 1.0f}}, VkClearDepthStencilValue{0.0f, 0});

            commandGraph = vsg::CommandGraph::create(window);
            commandGraph->addChild(windowRenderGraph);
            commandGraph->addChild(offscreenRenderGraph);
            if (colorBufferCapture) commandGraph->addChild(colorBufferCapture);
        }
        else
        {
            commandGraph = vsg::CommandGraph::create(device, queueFamily);
            commandGraph->addChild(offscreenRenderGraph);
            if (colorBufferCapture) commandGraph->addChild(colorBufferCapture);
        }

        viewer->assignRecordAndSubmitTaskAndPresentation({commandGraph});
        viewer->compile();
        viewer->start_point() = vsg::clock::now();
        return true;
    }
    catch (const vsg::Exception& ve)
    {
        std::cerr << "[Exception] - " << ve.message << " result = " << ve.result << std::endl;
        return false;
    }
}

bool Engine::renderOneTick()
{
    if (!viewer)
    {
        std::cerr << "Engine not initialized." << std::endl;
        return false;
    }

    try
    {
        constexpr uint64_t waitTimeout = 1999999999;

        if (!viewer->advanceToNextFrame())
            return false;

        viewer->handleEvents();
        viewer->update();
        viewer->recordAndSubmit();
        if (window)
            viewer->present();
        viewer->waitForFences(0, waitTimeout);

        hasRenderedFrame = true;
        return true;
    }
    catch (const vsg::Exception& ve)
    {
        std::cerr << "[Exception] - " << ve.message << " result = " << ve.result << std::endl;
        return false;
    }
}

bool Engine::CaptureToFile(const vsg::Path& outputPngPath)
{
    if (!viewer || !device || !copiedColorBuffer || !options)
    {
        std::cerr << "Engine not initialized." << std::endl;
        return false;
    }

    if (!hasRenderedFrame)
    {
        std::cerr << "No frame rendered yet; call renderOneTick() before CaptureToFile()." << std::endl;
        return false;
    }

    try
    {
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

        if (!vsg::write(imageData, outputPngPath, options))
        {
            std::cerr << "Failed to write PNG: " << outputPngPath << std::endl;
            return false;
        }

        return true;
    }
    catch (const vsg::Exception& ve)
    {
        std::cerr << "[Exception] - " << ve.message << " result = " << ve.result << std::endl;
        return false;
    }
}

void Engine::run()
{
    while (renderOneTick())
    {
    }
}
