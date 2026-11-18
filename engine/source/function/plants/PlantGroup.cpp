#include "function/plants/PlantGroup.h"

#include <vsg/core/Array.h>
#include <vsg/nodes/InstanceNode.h>
#include <vsg/state/BufferInfo.h>

#include <cmath>
#include <cstdint>

namespace
{

    constexpr double cellSizeMeters = 256.0;

    // 负向要用 floor。整数除法会向 0 截断，-0.1 米会进错片。
    int cellIndex(float meters)
    {
        return static_cast<int>(std::floor(static_cast<double>(meters) / cellSizeMeters));
    }

    aerovista::CellKey keyOf(const aerovista::PlantInstance& plant)
    {
        const auto& position = plant.transform.position;
        return {plant.type, cellIndex(position.x), cellIndex(position.y)};
    }

    struct SharedPose
    {
        vsg::ref_ptr<vsg::BufferInfo> translations;
        vsg::ref_ptr<vsg::BufferInfo> rotations;
        vsg::ref_ptr<vsg::BufferInfo> scales;
        std::uint32_t count = 0;
    };

    // setTranslations 每次都会新建 BufferInfo。各档要共用同一份，只能直接挂这三份对象。
    SharedPose createSharedPose(const std::vector<aerovista::PlantTransform>& poses)
    {
        const auto count = static_cast<std::uint32_t>(poses.size());
        auto translations = vsg::vec3Array::create(count);
        auto rotations = vsg::quatArray::create(count);
        auto scales = vsg::vec3Array::create(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            translations->at(index) = poses[index].position;
            rotations->at(index) = poses[index].rotation;
            scales->at(index) = poses[index].scale;
        }

        return {vsg::BufferInfo::create(translations), vsg::BufferInfo::create(rotations), vsg::BufferInfo::create(scales), count};
    }

    vsg::ref_ptr<vsg::InstanceNode> createInstance(const SharedPose& pose, vsg::ref_ptr<vsg::Node> mesh)
    {
        auto instance = vsg::InstanceNode::create();
        instance->translations = pose.translations;
        instance->rotations = pose.rotations;
        instance->scales = pose.scales;
        instance->firstInstance = 0;
        instance->instanceCount = pose.count;
        instance->child = mesh;
        return instance;
    }

    // 验收不锁占比。JSON 配置之前，先按档序号写成递减分数，最后一档为 0。
    double temporaryRatio(std::size_t index, std::size_t levelCount)
    {
        return static_cast<double>(levelCount - 1 - index) / static_cast<double>(levelCount);
    }

} // namespace

namespace aerovista
{

    std::map<CellKey, std::vector<PlantTransform>> splitPlantTiles(const std::vector<PlantInstance>& plants)
    {
        std::map<CellKey, std::vector<PlantTransform>> tiles;
        for (const auto& plant : plants)
            tiles[keyOf(plant)].push_back(plant.transform);
        return tiles;
    }

    vsg::ref_ptr<vsg::LOD> createPlantLods(const std::vector<PlantTransform>& poses, const std::vector<vsg::ref_ptr<vsg::Node>>& meshes)
    {
        if (meshes.empty())
            return {};

        const auto pose = createSharedPose(poses);
        auto lod = vsg::LOD::create();
        for (std::size_t index = 0; index < meshes.size(); ++index)
            lod->addChild({temporaryRatio(index, meshes.size()), createInstance(pose, meshes[index])});
        return lod;
    }

} // namespace aerovista
