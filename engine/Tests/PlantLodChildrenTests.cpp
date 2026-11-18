#include <catch2/catch_test_macros.hpp>

#include "function/plants/PlantGroup.h"

#include <vsg/nodes/InstanceNode.h>
#include <vsg/nodes/LOD.h>

#include <cstdint>
#include <vector>

namespace
{

    bool holdsPosition(const vsg::InstanceNode& instance, const vsg::vec3& position)
    {
        auto translations = instance.getTranslations();
        if (!translations)
            return false;
        for (const auto& value : *translations)
        {
            if (value == position)
                return true;
        }
        return false;
    }

    void requireChild(const vsg::LOD& lod, std::size_t index, vsg::ref_ptr<vsg::Node> mesh, std::uint32_t instanceCount)
    {
        REQUIRE(lod.children.size() > index);
        auto instance = lod.children[index].node.cast<vsg::InstanceNode>();
        REQUIRE(instance);
        REQUIRE(instance->child == mesh);
        REQUIRE(instance->firstInstance == 0);
        REQUIRE(instance->instanceCount == instanceCount);
    }

} // namespace

// =============================================================================
// 已经分好的一片按收集顺序挂上全部网格
// （lod分层实例.md §3、§8，码 PLANT-lod-children）。
// 这一片的两棵树都出现在每一档上。不验收占比，占比是临时值。
// 不验收各档是否共用一份 BufferInfo。另一片要由调用方再调一次。
// =============================================================================
TEST_CASE("one tile hangs the collected meshes from fine to coarse", "[unit][plants][PLANT-lod-children]")
{
    const vsg::vec3 origin{0.0f, 0.0f, 0.0f};
    const vsg::vec3 beside{10.0f, 0.0f, 0.0f};
    auto fine = vsg::Node::create();
    auto middle = vsg::Node::create();
    auto coarse = vsg::Node::create();

    std::vector<aerovista::PlantTransform> poses(2);
    poses[0].position = origin;
    poses[1].position = beside;

    auto lod = aerovista::createPlantLods(poses, {fine, middle, coarse});

    REQUIRE(lod);
    REQUIRE(holdsPosition(*lod->children.front().node.cast<vsg::InstanceNode>(), beside));
    requireChild(*lod, 0, fine, 2);
    requireChild(*lod, 1, middle, 2);
    requireChild(*lod, 2, coarse, 2);
}

// =============================================================================
// 每种植物单独调用，各用自己传入的网格列表
// （lod分层实例.md §3、§8，码 PLANT-lod-children）。
// 不验收占比。
// =============================================================================
TEST_CASE("each species is mounted with the mesh list it was given", "[unit][plants][PLANT-lod-children]")
{
    auto alderFine = vsg::Node::create();
    auto alderMiddle = vsg::Node::create();
    auto alderCoarse = vsg::Node::create();
    auto birch = vsg::Node::create();

    const aerovista::PlantTransform alderPose;
    const aerovista::PlantTransform birchPose;

    auto alderLod = aerovista::createPlantLods({alderPose}, {alderFine, alderMiddle, alderCoarse});
    auto birchLod = aerovista::createPlantLods({birchPose}, {birch});

    REQUIRE(alderLod);
    REQUIRE(birchLod);
    requireChild(*alderLod, 0, alderFine, 1);
    requireChild(*alderLod, 1, alderMiddle, 1);
    requireChild(*alderLod, 2, alderCoarse, 1);
    requireChild(*birchLod, 0, birch, 1);
}

// =============================================================================
// 网格列表为空时不建 LOD
// （lod分层实例.md §3、§8，码 PLANT-lod-children）。
// 空列表就是收集结果为空。不验收日志。
// =============================================================================
TEST_CASE("no meshes produces no LOD", "[unit][plants][PLANT-lod-children]")
{
    const aerovista::PlantTransform pose;

    auto lod = aerovista::createPlantLods({pose}, {});

    REQUIRE_FALSE(lod);
}
