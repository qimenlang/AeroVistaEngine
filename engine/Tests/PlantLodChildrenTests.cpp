#include <catch2/catch_test_macros.hpp>

#include "function/plants/PlantGroup.h"

#include <vsg/nodes/InstanceNode.h>
#include <vsg/nodes/LOD.h>

#include <cstdint>

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
// 网格没有包围盒时仍建 LOD。本用例不验收包围球。
// 不验收各档是否共用一份 BufferInfo。另一片要由调用方再调一次。
// =============================================================================
TEST_CASE("one tile hangs the collected meshes from fine to coarse", "[unit][plants][PLANT-lod-children]")
{
    const vsg::vec3 origin{0.0f, 0.0f, 0.0f};
    const vsg::vec3 beside{10.0f, 0.0f, 0.0f};
    auto fine = vsg::Node::create();
    auto middle = vsg::Node::create();
    auto coarse = vsg::Node::create();

    aerovista::PlantTile tile;
    tile.poses.resize(2);
    tile.poses[0].position = origin;
    tile.poses[1].position = beside;

    auto lod = aerovista::createPlantLods(tile, {fine, middle, coarse});

    REQUIRE(lod);
    REQUIRE(holdsPosition(*lod->children.front().node.cast<vsg::InstanceNode>(), beside));
    requireChild(*lod, 0, fine, 2);
    requireChild(*lod, 1, middle, 2);
    requireChild(*lod, 2, coarse, 2);
}

// =============================================================================
// 每种植物单独调用，各用自己传入的网格列表
// （lod分层实例.md §3、§8，码 PLANT-lod-children）。
// 网格没有包围盒时仍建 LOD。本用例不验收包围球。
// 不验收占比。
// =============================================================================
TEST_CASE("each species is mounted with the mesh list it was given", "[unit][plants][PLANT-lod-children]")
{
    auto alderFine = vsg::Node::create();
    auto alderMiddle = vsg::Node::create();
    auto alderCoarse = vsg::Node::create();
    auto birch = vsg::Node::create();

    aerovista::PlantTile alderTile;
    aerovista::PlantTile birchTile;
    alderTile.poses.resize(1);
    birchTile.poses.resize(1);

    auto alderLod = aerovista::createPlantLods(alderTile, {alderFine, alderMiddle, alderCoarse});
    auto birchLod = aerovista::createPlantLods(birchTile, {birch});

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
    aerovista::PlantTile tile;
    tile.poses.resize(1);

    auto lod = aerovista::createPlantLods(tile, {});

    REQUIRE_FALSE(lod);
}

// =============================================================================
// 同一个 LOD 的各档共用一份位姿 BufferInfo，第二棵的位姿写在这一份里
// （lod分层实例.md §3、§8，码 PLANT-lod-share）。
// 比的是 BufferInfo 对象本身。setTranslations 每次都会新建一层，只共用底层数组不算通过。
// 网格没有包围盒时仍建 LOD。本用例不验收包围球。
// 不验收占比、firstInstance、instanceCount、colors。不经过分片。
// =============================================================================
TEST_CASE("one LOD's levels share a single pose buffer", "[unit][plants][PLANT-lod-share]")
{
    const vsg::vec3 beside{10.0f, 0.0f, 0.0f};
    const vsg::quat besideRotation{0.0f, 0.0f, 1.0f, 0.0f};
    const vsg::vec3 besideScale{2.0f, 3.0f, 4.0f};

    aerovista::PlantTile tile;
    tile.poses.resize(2);
    tile.poses[1].position = beside;
    tile.poses[1].rotation = besideRotation;
    tile.poses[1].scale = besideScale;

    auto lod = aerovista::createPlantLods(tile, {vsg::Node::create(), vsg::Node::create()});

    REQUIRE(lod);
    REQUIRE(lod->children.size() == 2);
    auto fineInstance = lod->children[0].node.cast<vsg::InstanceNode>();
    auto coarseInstance = lod->children[1].node.cast<vsg::InstanceNode>();
    REQUIRE(fineInstance);
    REQUIRE(coarseInstance);
    REQUIRE(fineInstance->translations == coarseInstance->translations);
    REQUIRE(fineInstance->rotations == coarseInstance->rotations);
    REQUIRE(fineInstance->scales == coarseInstance->scales);

    auto translations = fineInstance->getTranslations();
    auto rotations = fineInstance->getRotations();
    auto scales = fineInstance->getScales();
    REQUIRE(translations);
    REQUIRE(rotations);
    REQUIRE(scales);
    REQUIRE(translations->at(1) == beside);
    REQUIRE(rotations->at(1) == besideRotation);
    REQUIRE(scales->at(1) == besideScale);
}
