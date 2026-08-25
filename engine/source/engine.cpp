#include "engine.h"

#include "InitialCameraConfig.h"
#include "function/handler/CommandTriggerHandler.h"
#include "function/handler/FrameStatsHandler.h"

#include <vsgXchange/all.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using aerovista::sync::HostEyeCoordFrame;
using aerovista::sync::HostEyePose;
using aerovista::sync::HostSync;
using aerovista::sync::SynchronSystem;
using aerovista::sync::SyncRoleConfig;

namespace
{

    /// 用 vsg::EllipsoidModel 实现 sync 库的 EllipsoidTransform 注入接口。
    /// 引擎侧适配器：把 sync 库需要的大地测量学操作桥接到 vsg 实现。
    class VsgEllipsoidTransform final : public aerovista::sync::EllipsoidTransform
    {
    public:
        explicit VsgEllipsoidTransform(vsg::ref_ptr<vsg::EllipsoidModel> model) :
            _model(std::move(model)) {}

        aerovista::sync::DVec3 ecefToLla(const aerovista::sync::DVec3& ecef) const override
        {
            const vsg::dvec3 lla = _model->convertECEFToLatLongAltitude(vsg::dvec3(ecef.x, ecef.y, ecef.z));
            return {lla.x, lla.y, lla.z};
        }

        aerovista::sync::DVec3 llaToEcef(const aerovista::sync::DVec3& lla) const override
        {
            const vsg::dvec3 ecef = _model->convertLatLongAltitudeToECEF(vsg::dvec3(lla.x, lla.y, lla.z));
            return {ecef.x, ecef.y, ecef.z};
        }

        void localToWorldBasis(const aerovista::sync::DVec3& lla, aerovista::sync::DVec3& east,
                               aerovista::sync::DVec3& north, aerovista::sync::DVec3& up) const override
        {
            const vsg::dmat4 l2w = _model->computeLocalToWorldTransform(vsg::dvec3(lla.x, lla.y, lla.z));
            east = {l2w(0, 0), l2w(0, 1), l2w(0, 2)};
            north = {l2w(1, 0), l2w(1, 1), l2w(1, 2)};
            up = {l2w(2, 0), l2w(2, 1), l2w(2, 2)};
        }

    private:
        vsg::ref_ptr<vsg::EllipsoidModel> _model;
    };

    struct FrameStatsHud
    {
        vsg::ref_ptr<vsg::Text> text;
        vsg::ref_ptr<vsg::stringValue> label;
        vsg::ref_ptr<vsg::Switch> visibility;
        vsg::ref_ptr<vsg::Camera> camera;
    };

    FrameStatsHud createFrameStatsHud(const VkExtent2D& extent, vsg::ref_ptr<vsg::Options> options)
    {
        FrameStatsHud hud;

        auto font = vsg::read_cast<vsg::Font>("fonts/times.vsgb", options);
        if (!font)
        {
            std::cerr << "Failed to load fonts/times.vsgb; on-screen frame stats disabled." << std::endl;
            return hud;
        }

        // 关闭深度测试，让 HUD 文本始终绘制在最上层。
        auto shaderSet = options->shaderSets["text"] = vsg::createTextShaderSet(options);
        auto depthStencilState = vsg::DepthStencilState::create();
        depthStencilState->depthTestEnable = VK_FALSE;
        depthStencilState->depthWriteEnable = VK_FALSE;
        shaderSet->defaultGraphicsPipelineStates.push_back(depthStencilState);

        hud.label = vsg::stringValue::create("IGCtrl: ---\nFPS: ---\nframe: --- ms");
        hud.label->properties.dataVariance = vsg::DYNAMIC_DATA;

        auto layout = vsg::StandardLayout::create();
        layout->horizontalAlignment = vsg::StandardLayout::LEFT_ALIGNMENT;
        layout->verticalAlignment = vsg::StandardLayout::TOP_ALIGNMENT;
        layout->position = vsg::vec3(-0.95f, 0.90f, 0.0f);
        layout->horizontal = vsg::vec3(0.035f, 0.0f, 0.0f);
        layout->vertical = vsg::vec3(0.0f, 0.055f, 0.0f);
        layout->color = vsg::vec4(1.0f, 1.0f, 0.2f, 1.0f);
        layout->outlineWidth = 0.1f;

        hud.text = vsg::Text::create();
        hud.text->technique = vsg::GpuLayoutTechnique::create();
        hud.text->font = font;
        hud.text->layout = layout;
        hud.text->text = hud.label;
        hud.text->setup(128, options);

        hud.visibility = vsg::Switch::create();
        hud.visibility->addChild(false, hud.text);

        auto projection = vsg::Orthographic::create(-1.0, 1.0, -1.0, 1.0, -1.0, 1.0);
        auto lookAt = vsg::LookAt::create(vsg::dvec3(0.0, 0.0, 1.0), vsg::dvec3(0.0, 0.0, 0.0), vsg::dvec3(0.0, 1.0, 0.0));
        hud.camera = vsg::Camera::create(projection, lookAt, vsg::ViewportState::create(extent));

        return hud;
    }

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

        auto cmdTransitionForTransferBarrier = vsg::PipelineBarrier::create(
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            transitionDestinationImageToDestinationLayoutBarrier,
            transitionSourceImageToTransferSourceLayoutBarrier);

        commands->addChild(cmdTransitionForTransferBarrier);

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

        auto cmdTransitionFromTransferBarrier = vsg::PipelineBarrier::create(
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            transitionDestinationImageToMemoryReadBarrier);

        commands->addChild(cmdTransitionFromTransferBarrier);

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

    vsg::dvec3 toDVec3(const Vec3Config& v)
    {
        return vsg::dvec3{v.x, v.y, v.z};
    }

    /// R = Rz(yaw)*Rx(pitch)*Ry(roll) 的 3×3（与 setCameraPose / ModelConfigTests 同一轴序）。
    vsg::dmat4 rotationMatrixYpr(const vsg::dvec3& eulerYprDeg)
    {
        const vsg::dquat qRoll(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dquat qPitch(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0));
        const vsg::dquat qYaw(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0));
        const auto rotate = [&](const vsg::dvec3& v) { return qYaw * (qPitch * (qRoll * v)); };
        const vsg::dvec3 x = rotate(vsg::dvec3(1.0, 0.0, 0.0));
        const vsg::dvec3 y = rotate(vsg::dvec3(0.0, 1.0, 0.0));
        const vsg::dvec3 z = rotate(vsg::dvec3(0.0, 0.0, 1.0));
        vsg::dmat4 m = vsg::dmat4(1.0);
        m(0, 0) = x.x;
        m(0, 1) = x.y;
        m(0, 2) = x.z;
        m(1, 0) = y.x;
        m(1, 1) = y.y;
        m(1, 2) = y.z;
        m(2, 0) = z.x;
        m(2, 1) = z.y;
        m(2, 2) = z.z;
        return m;
    }

    bool setupOptions(vsg::ref_ptr<vsg::Options>& options)
    {
        options = vsg::Options::create();
        options->sharedObjects = vsg::SharedObjects::create();
        options->fileCache = vsg::getEnv("VSG_FILE_CACHE");
        options->paths = vsg::getEnvPaths("VSG_FILE_PATH");
        options->add(vsgXchange::all::create());
#ifdef RESOURCE_DIR
        options->paths.push_back(vsg::Path(RESOURCE_DIR));
#endif
        return true;
    }

} // namespace

#ifndef RESOURCE_DIR
#    define RESOURCE_DIR "."
#endif

namespace
{
    std::string defaultConfigPath()
    {
        return std::string(RESOURCE_DIR) + "/config/default.json";
    }

    std::string formatSimTimeUsParts(std::uint64_t totalUs)
    {
        const std::uint64_t sec = totalUs / 1000000;
        const std::uint64_t msec = (totalUs / 1000) % 1000;
        const std::uint64_t usec = totalUs % 1000;

        std::ostringstream oss;
        oss << sec << "," << std::setfill('0') << std::setw(3) << msec << "," << std::setw(3) << usec
            << std::setfill(' ');
        return oss.str();
    }
} // namespace

Engine::Engine()
{
    _synchronSystem = SynchronSystem::create();
    // 默认匹配 default.json（无 -c）：仅图形，同步关闭。
    loadConfig(defaultConfigPath());
}

Engine::~Engine()
{
    if (_synchronSystem)
        _synchronSystem->shutdown();
    // 释放 Vulkan 对象，使含多个 Engine 的 Catch 套件保持在设备数限制内。
    _viewer = {};
    _window = {};
    _mainCamera = {};
    _copiedColorBuffer = {};
    _device = {};
    _scene = {};
    _options = {};
}

std::string Engine::resolveConfigPath(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (argv[i] && std::string(argv[i]) == "-c" && argv[i + 1])
            return std::string(argv[i + 1]);
    }
    return defaultConfigPath();
}

bool Engine::loadConfig(const std::string& path)
{
    EngineChannelConfig loaded;
    std::string error;
    if (!loadEngineChannelConfig(path, loaded, &error))
    {
        std::cerr << "Engine::loadConfig failed: " << error << std::endl;
        return false;
    }
    config = std::move(loaded);
    applyConfigToEngine();
    return true;
}

void Engine::applyConfigToEngine()
{
    extent.width = static_cast<uint32_t>(config.window.width);
    extent.height = static_cast<uint32_t>(config.window.height);
    // 窗口可见性始终是 Engine::showWindow（默认 true）；不来自配置。
}

SynchronSystem& Engine::synchronSystem()
{
    return *_synchronSystem;
}

HostSync& Engine::hostSync()
{
    return *_hostSync;
}

vsg::ref_ptr<vsg::Window> Engine::mainWindow() const
{
    return _window;
}

vsg::ref_ptr<vsg::Camera> Engine::mainCamera() const
{
    return _mainCamera;
}

vsg::ref_ptr<vsg::EllipsoidModel> Engine::ellipsoidModel() const
{
    if (!_scene)
        return {};
    return _scene->getRefObject<vsg::EllipsoidModel>("EllipsoidModel");
}

bool Engine::sampleEntityPoseById(int id, vsg::dvec3& positionOrLla, vsg::dvec3& eulerYprDeg) const
{
    const auto it = _entityMap.find(id);
    if (it == _entityMap.end())
        return false;
    const Entity& entity = it->second;
    if (config.coordFrame == CoordFrameIntent::ELLIPSOID)
    {
        if (!entity.hasEllipsoidPose)
            return false;
        positionOrLla = entity.ellipsoidLla;
        eulerYprDeg = entity.ellipsoidYpr;
        return true;
    }
    if (!entity.hasLocalPose)
        return false;
    positionOrLla = entity.localPosition;
    eulerYprDeg = entity.localYpr;
    return true;
}

std::size_t Engine::entitySize() const
{
    return _entityMap.size();
}

bool Engine::hasEntityId(int id) const
{
    return _entityMap.find(id) != _entityMap.end();
}

bool Engine::entityName(int id, std::string& outName) const
{
    const auto it = _entityMap.find(id);
    if (it == _entityMap.end())
        return false;
    outName = it->second.name;
    return true;
}

vsg::ref_ptr<vsg::MatrixTransform> Engine::entityTransform(int id) const
{
    const auto it = _entityMap.find(id);
    if (it == _entityMap.end())
        return {};
    return it->second.transform;
}

vsg::dmat4 Engine::makeEntityMatrix(const EntityConfig& cfg, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid) const
{
    if (config.coordFrame == CoordFrameIntent::ELLIPSOID)
    {
        const vsg::dvec3 lla = toDVec3(cfg.ellipsoidPose.lla);
        const vsg::dvec3 ypr = toDVec3(cfg.ellipsoidPose.eulerYprDeg);
        return ellipsoid->computeLocalToWorldTransform(lla) * rotationMatrixYpr(ypr);
    }
    const vsg::dvec3 position = toDVec3(cfg.localPose.position);
    const vsg::dvec3 ypr = toDVec3(cfg.localPose.eulerYprDeg);
    return vsg::translate(position) * rotationMatrixYpr(ypr);
}

void Engine::ensureEntityTransform(Entity& entity)
{
    if (entity.transform)
        return;
    auto mt = vsg::MatrixTransform::create();
    if (entity.node)
        mt->addChild(entity.node);
    entity.transform = mt;
    // 主线程执行（drainIncoming）：与渲染遍历天然串行，无需锁。
    // 运行期挂载的模型节点需编译 GPU pipeline：初始场景在 finishGraphicsAfterScene 编译，
    // 新挂载节点的 GraphicsPipeline::_implementation 为空 → record 时 vk() 越界。
    if (auto group = _scene.cast<vsg::Group>())
    {
        group->addChild(mt);
        if (_viewer)
            _viewer->compile();
    }
}

void Engine::recomputeEntityTransform(Entity& entity)
{
    if (!entity.transform)
        return;
    const auto ellipsoid = ellipsoidModel();
    if (entity.hasEllipsoidPose && ellipsoid)
        entity.transform->matrix =
            ellipsoid->computeLocalToWorldTransform(entity.ellipsoidLla) * rotationMatrixYpr(entity.ellipsoidYpr);
    else
        entity.transform->matrix = vsg::translate(entity.localPosition) * rotationMatrixYpr(entity.localYpr);
}

void Engine::applyCameraPoseFromConfig()
{
    if (!config.hasCamera || !config.camera.hasPose)
        return;
    if (config.coordFrame == CoordFrameIntent::ELLIPSOID && config.camera.hasPoseEllipsoid)
    {
        setCameraPoseLla(toDVec3(config.camera.ellipsoidPose.lla), toDVec3(config.camera.ellipsoidPose.eulerYprDeg));
        return;
    }
    if (config.coordFrame == CoordFrameIntent::LOCAL && config.camera.hasPoseLocal)
    {
        setCameraPose(toDVec3(config.camera.localPose.position), toDVec3(config.camera.localPose.eulerYprDeg));

        // 位姿配置设计.md §4.1 D3: 重算 near/far 以避免裁切
        if (_mainCamera)
        {
            auto perspective = _mainCamera->projectionMatrix.cast<vsg::Perspective>();
            if (perspective)
            {
                const double eyeDistance = vsg::length(_mainCamera->viewMatrix.cast<vsg::LookAt>()->eye - _aabbCentre);
                const double minFar = eyeDistance + _aabbRadius;
                const double defaultFar = 4.5 * _aabbRadius;
                const double newFar = std::max(defaultFar, minFar);
                const double newNear = 0.001 * newFar;
                perspective->nearDistance = newNear;
                perspective->farDistance = newFar;
            }
        }
        return;
    }
}

bool Engine::ensureEllipsoidModelForFrame()
{
    auto ellipsoidModel = _scene ? _scene->getRefObject<vsg::EllipsoidModel>("EllipsoidModel") : vsg::ref_ptr<vsg::EllipsoidModel>{};
    const char* ellipsoidSource = "model";
    if (ellipsoidModel)
    {
        if (config.coordFrame == CoordFrameIntent::LOCAL)
        {
            std::cerr << "[WARN] scene has EllipsoidModel but coordFrame is Local; "
                         "runtime stays ellipsoid (lla设计 §2.3)\n";
        }
    }
    else if (config.coordFrame == CoordFrameIntent::ELLIPSOID)
    {
        ellipsoidModel = vsg::EllipsoidModel::create();
        _scene->setObject("EllipsoidModel", ellipsoidModel);
        ellipsoidSource = "inject-WGS84";
    }

    if (ellipsoidModel)
    {
        std::cerr << "[INFO] EllipsoidModel radii equator=" << ellipsoidModel->radiusEquator()
                  << " polar=" << ellipsoidModel->radiusPolar() << " source=" << ellipsoidSource << "\n";
    }
    if (ellipsoidModel)
        _ellipsoidTransform = std::make_unique<VsgEllipsoidTransform>(ellipsoidModel);
    else
        _ellipsoidTransform.reset();
    if (_synchronSystem)
        _synchronSystem->setEllipsoidTransform(_ellipsoidTransform.get());
    return true;
}

bool Engine::assembleEntitiesScene()
{
    _entityMap.clear();
    if (!setupOptions(_options))
        return false;

    auto root = vsg::Group::create();
    _scene = root;
    if (!ensureEllipsoidModelForFrame())
        return false;
    auto ellipsoid = ellipsoidModel();

    for (const EntityConfig& cfg : config.entities)
    {
        const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / cfg.model;
        vsg::ref_ptr<vsg::Node> loaded;
        if (!loadScene(modelPath, _options, loaded))
            return false;

        Entity entity;
        entity.id = cfg.id;
        entity.name = cfg.name;
        entity.path = cfg.model;
        entity.hasLocalPose = cfg.hasPoseLocal;
        entity.hasEllipsoidPose = cfg.hasPoseEllipsoid;
        entity.localPosition = toDVec3(cfg.localPose.position);
        entity.localYpr = toDVec3(cfg.localPose.eulerYprDeg);
        entity.ellipsoidLla = toDVec3(cfg.ellipsoidPose.lla);
        entity.ellipsoidYpr = toDVec3(cfg.ellipsoidPose.eulerYprDeg);
        entity.node = loaded;

        if (cfg.hasPose)
        {
            if (config.coordFrame == CoordFrameIntent::ELLIPSOID && !ellipsoid)
                return false;
            auto mt = vsg::MatrixTransform::create();
            mt->matrix = makeEntityMatrix(cfg, ellipsoid);
            mt->addChild(loaded);
            root->addChild(mt);
            entity.transform = mt;
        }
        else
        {
            root->addChild(loaded);
        }

        _entityMap.emplace(entity.id, std::move(entity));
    }
    return true;
}

bool Engine::setCameraPose(const vsg::dvec3& position, const vsg::dvec3& eulerYprDeg)
{
    if (!_mainCamera)
        return false;

    auto lookAt = _mainCamera->viewMatrix.cast<vsg::LookAt>();
    if (!lookAt)
        return false;

    // R = Rz(yaw)*Rx(pitch)*Ry(roll)。按 roll→pitch→yaw 顺序逐个作用轴四元数
    // （VSG quat*quat 是 reverse-Hamilton；不要直接连乘 Rz*Rx*Ry）。
    constexpr double kLookDistance = 1.0;
    const auto rotateYpr = [&](const vsg::dvec3& v) {
        const vsg::dvec3 afterRoll =
            vsg::dquat(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0)) * v;
        const vsg::dvec3 afterPitch =
            vsg::dquat(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) * afterRoll;
        return vsg::dquat(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0)) * afterPitch;
    };
    const vsg::dvec3 forward = rotateYpr(vsg::dvec3(0.0, 1.0, 0.0));
    const vsg::dvec3 up = rotateYpr(vsg::dvec3(0.0, 0.0, 1.0));

    lookAt->eye = position;
    lookAt->center = position + forward * kLookDistance;
    lookAt->up = up;
    return true;
}

bool Engine::setCameraPoseLla(const vsg::dvec3& lla, const vsg::dvec3& eulerYprDeg)
{
    if (!_mainCamera || !_scene)
        return false;

    auto ellipsoidModel = _scene->getRefObject<vsg::EllipsoidModel>("EllipsoidModel");
    if (!ellipsoidModel)
    {
        // 回退到投影的椭球（装配成功时是同一对象）。
        if (auto ep = _mainCamera->projectionMatrix.cast<vsg::EllipsoidPerspective>())
            ellipsoidModel = ep->ellipsoidModel;
    }
    if (!ellipsoidModel)
        return false;

    auto lookAt = _mainCamera->viewMatrix.cast<vsg::LookAt>();
    if (!lookAt)
        return false;

    // lla设计 §3.3：R_local = Rz*Rx*Ry（ENU 内）；LocalToWorld 3×3 只用于方向。
    constexpr double kLookDistance = 1.0;
    const auto rotateYpr = [&](const vsg::dvec3& v) {
        const vsg::dvec3 afterRoll =
            vsg::dquat(vsg::radians(eulerYprDeg.z), vsg::dvec3(0.0, 1.0, 0.0)) * v;
        const vsg::dvec3 afterPitch =
            vsg::dquat(vsg::radians(eulerYprDeg.y), vsg::dvec3(1.0, 0.0, 0.0)) * afterRoll;
        return vsg::dquat(vsg::radians(eulerYprDeg.x), vsg::dvec3(0.0, 0.0, 1.0)) * afterPitch;
    };
    const vsg::dvec3 forwardEnu = rotateYpr(vsg::dvec3(0.0, 1.0, 0.0));
    const vsg::dvec3 upEnu = rotateYpr(vsg::dvec3(0.0, 0.0, 1.0));
    const vsg::dmat4 localToWorld = ellipsoidModel->computeLocalToWorldTransform(lla);
    // 正交 ENU 基（LocalToWorld 列可能被缩放；归一化使写↔采样互逆）。
    const vsg::dvec3 east = vsg::normalize(vsg::dvec3(localToWorld(0, 0), localToWorld(0, 1), localToWorld(0, 2)));
    const vsg::dvec3 north = vsg::normalize(vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
    const vsg::dvec3 upAxis = vsg::normalize(vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
    const auto rotateEnuToEcef = [&](const vsg::dvec3& enuDir) {
        return enuDir.x * east + enuDir.y * north + enuDir.z * upAxis;
    };

    const vsg::dvec3 eye = ellipsoidModel->convertLatLongAltitudeToECEF(lla);
    const vsg::dvec3 forward = vsg::normalize(rotateEnuToEcef(forwardEnu));
    const vsg::dvec3 up = vsg::normalize(rotateEnuToEcef(upEnu));

    lookAt->eye = eye;
    lookAt->center = eye + forward * kLookDistance;
    lookAt->up = up;
    return true;
}

bool Engine::init()
{
    applyConfigToEngine();
    // 父键 enable；装配配置（channelId / offsetDeg / hostEyeStalePolicy / requireConnectedIg）来自 syncSystem 组。
    if (!initSync(config.toSyncRole(), config.syncSystem))
        return false;

    if (!config.entities.empty())
        return initGraphicsFromEntities();

    // JSON 中的模型路径相对 resources/ 解析。
    const vsg::Path modelPath = vsg::Path(RESOURCE_DIR) / config.model;
    return initGraphics(modelPath);
}

bool Engine::init(const vsg::Path& modelPath)
{
    return init(modelPath, SyncRoleConfig{});
}

bool Engine::initSync(const SyncRoleConfig& syncRole, bool requireConnectedIg)
{
    // 程序化路径（测试）：仅指定 requireConnectedIg，其余装配配置用默认值。
    SyncSystemConfig syncSystem;
    syncSystem.requireConnectedIg = requireConnectedIg;
    return initSync(syncRole, syncSystem);
}

bool Engine::initSync(const SyncRoleConfig& syncRole, const SyncSystemConfig& syncSystem)
{
    // 模拟时间由 HostSync 自计时（initialize 记录 _startTime，beginWithIgCtrlUdp 填 TimeStamp，§7.1）——
    // 时钟同步方案.md §5 方案 B：从 HostSync 初始化时刻起 steady_clock 连续推进。

    // Host：Engine 直接持有 HostSync（直发，不经 SynchronSystem 门面）。
    _hostSync.reset();
    if (syncRole.enableHost)
    {
        _hostSync = std::make_unique<HostSync>();
        if (!_hostSync->initialize(syncRole.hostConfig))
        {
            std::cerr << "Engine: HostSync initialize failed\n";
            return false;
        }
        _hostSync->run();
    }

    // IG：SynchronSystem（IG 决策器）；enableIg=false 时 initialize 仅清空旧 IG。
    if (!_synchronSystem->initialize(syncRole, syncSystem))
        return false;

    return true;
}

bool Engine::init(const vsg::Path& modelPath, const SyncRoleConfig& syncRole)
{
    if (!initSync(syncRole))
        return false;
    return initGraphics(modelPath);
}

bool Engine::initSceneMode(const vsg::Path& modelPath)
{
    try
    {
        if (!setupOptions(_options))
            return false;
        if (!loadScene(modelPath, _options, _scene))
            return false;
        return ensureEllipsoidModelForFrame();
    }
    catch (const vsg::Exception& ve)
    {
        std::cerr << "[Exception] - " << ve.message << " result = " << ve.result << std::endl;
        return false;
    }
}

void Engine::resetGraphicsResources()
{
    // lla §4.3：图形重建时清空眼点缓存（不拆除同步）。
    if (_synchronSystem)
        _synchronSystem->resetEyeCaches();
    _hostPublisher.reset();

    _entityMap.clear();
    _currentExtent = extent;
    _hasRenderedFrame = false;
    // 分配新 Device 前丢弃旧的 Vulkan 图（Catch 多 Engine 套件）。
    _viewer = {};
    _window = {};
    _mainCamera = {};
    _copiedColorBuffer = {};
    _device = {};
    _frameStatsText = {};
    _frameStatsLabel = {};
    _frameStatsSwitch = {};
}

bool Engine::createVulkanDevice(int& queueFamily)
{
    _viewer = vsg::Viewer::create();
    queueFamily = -1;

    if (showWindow)
    {
        auto windowTraits = vsg::WindowTraits::create(
            config.window.x,
            config.window.y,
            static_cast<uint32_t>(config.window.width),
            static_cast<uint32_t>(config.window.height),
            "AeroVistaEngine");
        windowTraits->hdpi = false;
        windowTraits->decoration = false; // 无边框客户区
        _window = vsg::Window::create(windowTraits);
        if (!_window)
        {
            std::cerr << "Could not create window." << std::endl;
            return false;
        }
        _viewer->addWindow(_window);
        _device = _window->getOrCreateDevice();
        _currentExtent = _window->extent2D();
        return true;
    }

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
    _device = vsg::Device::create(physicalDevice, queueSettings, vsg::Names{}, vsg::Names{}, deviceFeatures);
    return true;
}

vsg::ref_ptr<vsg::LookAt> Engine::createInitialLookAt(vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel,
                                                      const vsg::dvec3& centre, double radius) const
{
    // 回退半径阈值（位姿配置设计.md §4）
    if (ellipsoidModel)
    {
        // 椭球：eye = centre_ecef - north·(k_back·radius) + up·(k_up·radius)
        // 位姿配置设计.md §4.2：k_back = 3.5, k_up = 0.3
        constexpr vsg::dvec3 kFallbackLla{39.9, 116.4, 500.0};

        // Fallback 判据：radius < 0.1 或 |centre| < 0.9·radiusPolar（场景中心落入地底）
        const double centreMag = vsg::length(centre);
        const bool useFallback = radius < initial_camera::kRadiusThreshold ||
                                 centreMag < ellipsoidModel->radiusPolar() * 0.9;

        if (useFallback)
        {
            std::cerr << "[WARN] Ellipsoid AABB triggers fallback: radius=" << radius
                      << ", |centre|=" << centreMag << " < " << (ellipsoidModel->radiusPolar() * 0.9)
                      << ", using fallback LLA=(39.9, 116.4, 500)\n";

            const vsg::dmat4 localToWorld = ellipsoidModel->computeLocalToWorldTransform(kFallbackLla);
            const vsg::dvec3 eye = ellipsoidModel->convertLatLongAltitudeToECEF(kFallbackLla);
            const vsg::dvec3 north = vsg::normalize(
                vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
            const vsg::dvec3 up = vsg::normalize(
                vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
            return vsg::LookAt::create(eye, eye + north, up); // YPR=0 朝北
        }

        // 主路径：按 AABB 计算
        const vsg::dvec3 centreLla = ellipsoidModel->convertECEFToLatLongAltitude(centre);
        const vsg::dmat4 localToWorld = ellipsoidModel->computeLocalToWorldTransform(centreLla);
        const vsg::dvec3 north = vsg::normalize(
            vsg::dvec3(localToWorld(1, 0), localToWorld(1, 1), localToWorld(1, 2)));
        const vsg::dvec3 up = vsg::normalize(
            vsg::dvec3(localToWorld(2, 0), localToWorld(2, 1), localToWorld(2, 2)));
        const vsg::dvec3 eye = centre - north * (initial_camera::kEllipsoidBackMultiplier * radius) +
                               up * (initial_camera::kEllipsoidUpMultiplier * radius);
        return vsg::LookAt::create(eye, centre, up);
    }

    // 本地：eye = centre + (0, -k_back·radius, 0)，k_back = 3.5
    // 位姿配置设计.md §4.1
    // 回退：radius < 0.1 → eye=(0,0,10)，center=(0,0,0)，up=(0,0,1)
    if (radius < initial_camera::kRadiusThreshold)
    {
        std::cerr << "[WARN] Local AABB radius=" << radius << " < " << initial_camera::kRadiusThreshold
                  << ", using fallback eye=(0,0,10)\n";
        return vsg::LookAt::create(vsg::dvec3(0.0, 0.0, 10.0), vsg::dvec3(0.0, 0.0, 0.0),
                                   vsg::dvec3(0.0, 0.0, 1.0));
    }

    return vsg::LookAt::create(centre + vsg::dvec3(0.0, -initial_camera::kLocalBackMultiplier * radius, 0.0), centre,
                               vsg::dvec3(0.0, 0.0, 1.0));
}

vsg::ref_ptr<vsg::ProjectionMatrix> Engine::createInitialProjection(
    vsg::ref_ptr<vsg::LookAt> lookAt, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel, double radius,
    double nearFarRatio) const
{
    const double aspect =
        static_cast<double>(_currentExtent.width) / static_cast<double>(_currentExtent.height);

    // 回退半径阈值（位姿配置设计.md §4）
    if (ellipsoidModel)
    {
        // EllipsoidPerspective 动态计算 near/far，horizonMountainHeight=0
        return vsg::EllipsoidPerspective::create(lookAt, ellipsoidModel, initial_camera::kFieldOfViewDegrees,
                                                 aspect, nearFarRatio,
                                                 initial_camera::kEllipsoidHorizonMountainHeight);
    }

    // 本地：near = 0.001·radius，far = 4.5·radius
    // fallback (radius < 0.1): near=0.1, far=100
    if (radius < initial_camera::kRadiusThreshold)
    {
        return vsg::Perspective::create(30.0, aspect, 0.1, 100.0);
    }

    return vsg::Perspective::create(initial_camera::kFieldOfViewDegrees, aspect, nearFarRatio * radius,
                                    radius * initial_camera::kLocalFarMultiplier);
}

vsg::ref_ptr<vsg::CommandGraph> Engine::buildCommandGraph(
    vsg::ref_ptr<vsg::Camera> camera, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel,
    vsg::ref_ptr<vsg::RenderGraph> offscreenRenderGraph, vsg::ref_ptr<vsg::Commands> colorBufferCapture,
    int queueFamily)
{
    _reportFrameStats = false;
    _frameStatsText = {};
    _frameStatsLabel = {};
    _frameStatsSwitch = {};
    _lastFrameSeconds = 0.0;

    if (_window)
    {
        _viewer->addEventHandler(vsg::CloseHandler::create(_viewer));
        _viewer->addEventHandler(vsg::Trackball::create(camera, ellipsoidModel));

        auto frameStatsHandler = FrameStatsHandler::create();
        frameStatsHandler->enabled = &_reportFrameStats;
        _viewer->addEventHandler(frameStatsHandler);

        // 实机命令触发：仅 Host 引擎（有 HostSync）挂 F3 热键。
        if (_hostSync)
        {
            auto commandHandler = CommandTriggerHandler::create();
            commandHandler->host = _hostSync.get();
            _viewer->addEventHandler(commandHandler);
        }

        auto windowView = vsg::View::create(camera);
        windowView->addChild(vsg::createHeadlight());
        windowView->addChild(_scene);

        auto windowRenderGraph = vsg::RenderGraph::create(_window, windowView);
        windowRenderGraph->setClearValues({{0.0f, 0.0f, 0.0f, 1.0f}}, VkClearDepthStencilValue{0.0f, 0});

        auto hud = createFrameStatsHud(_currentExtent, _options);
        if (hud.text && hud.visibility && hud.camera)
        {
            _frameStatsText = hud.text;
            _frameStatsLabel = hud.label;
            _frameStatsSwitch = hud.visibility;
            // HUD 文本管线已禁用深度测试/写入，无需中间通道深度清除。
            windowRenderGraph->addChild(vsg::View::create(hud.camera, hud.visibility));
        }

        auto commandGraph = vsg::CommandGraph::create(_window);
        commandGraph->addChild(windowRenderGraph);
        commandGraph->addChild(offscreenRenderGraph);
        if (colorBufferCapture)
            commandGraph->addChild(colorBufferCapture);
        return commandGraph;
    }

    auto commandGraph = vsg::CommandGraph::create(_device, queueFamily);
    commandGraph->addChild(offscreenRenderGraph);
    if (colorBufferCapture)
        commandGraph->addChild(colorBufferCapture);
    return commandGraph;
}

bool Engine::finishGraphicsAfterScene(vsg::ref_ptr<vsg::EllipsoidModel> ellipsoidModel)
{
    int queueFamily = -1;
    if (!createVulkanDevice(queueFamily))
        return false;

    vsg::ComputeBounds computeBounds;
    _scene->accept(computeBounds);
    const vsg::dvec3 centre = (computeBounds.bounds.min + computeBounds.bounds.max) * 0.5;
    const double radius = vsg::length(computeBounds.bounds.max - computeBounds.bounds.min) * 0.6;
    constexpr double nearFarRatio = initial_camera::kNearFarRatio;

    // 保存供相机位姿调整使用（位姿配置设计.md §4.1 D3）
    _aabbCentre = centre;
    _aabbRadius = radius;

    auto lookAt = createInitialLookAt(ellipsoidModel, centre, radius);
    auto perspective = createInitialProjection(lookAt, ellipsoidModel, radius, nearFarRatio);
    auto camera = vsg::Camera::create(perspective, lookAt, vsg::ViewportState::create(_currentExtent));
    _mainCamera = camera;

    auto colorImageView = createColorImageView(_device, _currentExtent, imageFormat);
    auto depthImageView = createDepthImageView(_device, _currentExtent, depthFormat);
    auto renderPass = createOffscreenRenderPass(_device, imageFormat, depthFormat);
    auto framebuffer = vsg::Framebuffer::create(renderPass, vsg::ImageViews{colorImageView, depthImageView},
                                                _currentExtent.width, _currentExtent.height, 1);

    vsg::ref_ptr<vsg::Commands> colorBufferCapture;
    std::tie(colorBufferCapture, _copiedColorBuffer) =
        createColorCapture(_device, _currentExtent, colorImageView->image, imageFormat);

    auto offscreenRenderGraph = vsg::RenderGraph::create();
    offscreenRenderGraph->framebuffer = framebuffer;
    offscreenRenderGraph->renderArea.offset = {0, 0};
    offscreenRenderGraph->renderArea.extent = _currentExtent;
    offscreenRenderGraph->setClearValues({{0.0f, 0.0f, 0.0f, 1.0f}}, VkClearDepthStencilValue{0.0f, 0});

    auto offscreenView = vsg::View::create(camera, _scene);
    offscreenView->addChild(vsg::createHeadlight());
    offscreenRenderGraph->addChild(offscreenView);

    auto commandGraph =
        buildCommandGraph(camera, ellipsoidModel, offscreenRenderGraph, colorBufferCapture, queueFamily);
    _viewer->assignRecordAndSubmitTaskAndPresentation({commandGraph});
    _viewer->compile();
    _viewer->start_point() = vsg::clock::now();
    applyCameraPoseFromConfig();
    return true;
}

bool Engine::initGraphicsFromEntities()
{
    try
    {
        resetGraphicsResources();
        if (!assembleEntitiesScene())
            return false;
        return finishGraphicsAfterScene(ellipsoidModel());
    }
    catch (const vsg::Exception& ve)
    {
        std::cerr << "[Exception] - " << ve.message << " result = " << ve.result << std::endl;
        return false;
    }
}

bool Engine::initGraphics(const vsg::Path& modelPath)
{
    try
    {
        resetGraphicsResources();
        if (!initSceneMode(modelPath))
            return false;
        return finishGraphicsAfterScene(ellipsoidModel());
    }
    catch (const vsg::Exception& ve)
    {
        std::cerr << "[Exception] - " << ve.message << " result = " << ve.result << std::endl;
        return false;
    }
}

void Engine::preFrame()
{
    // 子系统：_scene 更新前收包 / 应用状态。
    if (_synchronSystem)
        _synchronSystem->preFrame();
}

bool Engine::update()
{
    if (!_viewer->advanceToNextFrame())
        return false;

    _viewer->handleEvents();

    // Host→IG：先采样权威眼（覆盖前）；IG 决策器收包/决策，应用本帧位姿。
    if (_hostSync)
    {
        if (auto camera = mainCamera())
        {
            if (auto lookAt = camera->viewMatrix.cast<vsg::LookAt>())
            {
                _hostPublisher.captureAuthorityEye(
                    *lookAt, ellipsoidModel().get(), _synchronSystem->lastAppliedHostEye());
            }
        }
    }
    if (_synchronSystem)
    {
        _synchronSystem->update();
        if (auto pose = _synchronSystem->takePendingCameraPose())
            applySyncCameraPose(*pose);
    }

    if (_frameStatsSwitch)
        _frameStatsSwitch->setAllChildren(_reportFrameStats);

    if (_reportFrameStats && _frameStatsText && _frameStatsLabel && _lastFrameSeconds > 0.0)
    {
        const double frameMs = _lastFrameSeconds * 1000.0;
        const double instantFps = 1.0 / _lastFrameSeconds;

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(1);
        if (_synchronSystem && _synchronSystem->hasIg() && _synchronSystem->igSync().igCtrlReceivedCount() > 0)
            oss << frameStatsIgCtrlLine();
        else
            oss << "IGCtrl: ---\n";
        oss << "FPS: " << instantFps << "\n"
            << "frame: " << frameMs << " ms";

        _frameStatsLabel->value() = oss.str();
        _frameStatsText->setup(0, _options);
    }

    _viewer->update();
    return true;
}

std::string Engine::frameStatsIgCtrlLine() const
{
    // 调用方已确认 linked（_synchronSystem && hasIg && igCtrlReceivedCount>0）。
    // "IGCtrl: <帧号>：<s>,<ms>,<us>"（ms/us 补零 3 位）。
    std::ostringstream oss;
    oss << "IGCtrl: " << _synchronSystem->igSync().lastIgCtrlFrameCntr() << ":"
        << formatSimTimeUsParts(_synchronSystem->igSync().simTimeUs()) << "\n";
    return oss.str();
}

void Engine::render()
{
    constexpr uint64_t waitTimeout = 1999999999;
    _viewer->recordAndSubmit();
    if (_window)
        _viewer->present();
    _viewer->waitForFences(0, waitTimeout);
}

void Engine::postFrame()
{
    // 子系统：update+render 后读最终状态 / 扇出。
    // 模拟时间由 HostSync 自计时（_startTime = steady_clock::now() 于 initialize，§7.1）：
    // outMsgWithIgCtrlUdp() 自动填 IGCtrl.TimeStamp = simTimeMs×100（10µs 步进）。渲染卡顿时时间戳
    // 跟上真实流逝，IG 外推不因 host 帧节奏波动而放大误差（时钟同步方案.md §5 方案 B）。
    if (_hostSync)
        _hostPublisher.postHostFrame(*_hostSync, ellipsoidModel().get());
}

void Engine::stepSync()
{
    if (_synchronSystem)
    {
        _synchronSystem->update();
        if (auto pose = _synchronSystem->takePendingCameraPose())
            applySyncCameraPose(*pose);
    }
}

void Engine::applySyncCameraPose(const HostEyePose& pose)
{
    if (!hasGraphics())
        return;
    const vsg::dvec3 position(pose.position.x, pose.position.y, pose.position.z);
    const vsg::dvec3 eulerYprDeg(pose.eulerYprDeg.x, pose.eulerYprDeg.y, pose.eulerYprDeg.z);
    if (pose.frame == HostEyeCoordFrame::LLA)
        setCameraPoseLla(position, eulerYprDeg);
    else
        setCameraPose(position, eulerYprDeg);
}

void Engine::tickSync()
{
    preFrame();
    stepSync();
    postFrame();
}

bool Engine::tickOnFrame()
{
    if (!_viewer)
    {
        std::cerr << "Engine not initialized." << std::endl;
        return false;
    }

    try
    {
        const auto frameStart = vsg::clock::now();

        preFrame();
        if (!update())
            return false;
        render();
        postFrame();

        _lastFrameSeconds = std::chrono::duration<double, std::chrono::seconds::period>(vsg::clock::now() - frameStart).count();
        _hasRenderedFrame = true;
        return true;
    }
    catch (const vsg::Exception& ve)
    {
        std::cerr << "[Exception] - " << ve.message << " result = " << ve.result << std::endl;
        return false;
    }
}

bool Engine::captureToFile(const vsg::Path& outputPngPath)
{
    if (!_viewer || !_device || !_copiedColorBuffer || !_options)
    {
        std::cerr << "Engine not initialized." << std::endl;
        return false;
    }

    if (!_hasRenderedFrame)
    {
        std::cerr << "No frame rendered yet; call tickOnFrame() before captureToFile()." << std::endl;
        return false;
    }

    try
    {
        VkImageSubresource subResource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
        VkSubresourceLayout subResourceLayout;
        vkGetImageSubresourceLayout(*_device, _copiedColorBuffer->vk(_device->deviceID), &subResource, &subResourceLayout);

        auto deviceMemory = _copiedColorBuffer->getDeviceMemory(_device->deviceID);

        size_t destRowWidth = _currentExtent.width * sizeof(vsg::ubvec4);
        vsg::ref_ptr<vsg::Data> imageData;
        if (destRowWidth == subResourceLayout.rowPitch)
        {
            imageData = vsg::MappedData<vsg::ubvec4Array2D>::create(deviceMemory, subResourceLayout.offset, 0, vsg::Data::Properties{imageFormat}, _currentExtent.width, _currentExtent.height);
        }
        else
        {
            auto mappedData = vsg::MappedData<vsg::ubyteArray>::create(deviceMemory, subResourceLayout.offset, 0, vsg::Data::Properties{imageFormat}, subResourceLayout.rowPitch * _currentExtent.height);
            imageData = vsg::ubvec4Array2D::create(_currentExtent.width, _currentExtent.height, vsg::Data::Properties{imageFormat});
            for (uint32_t row = 0; row < _currentExtent.height; ++row)
            {
                std::memcpy(imageData->dataPointer(row * _currentExtent.width), mappedData->dataPointer(row * subResourceLayout.rowPitch), destRowWidth);
            }
        }

        if (!vsg::write(imageData, outputPngPath, _options))
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
    while (true)
    {
        if (!tickOnFrame())
        {
            break;
        }
    }
}
