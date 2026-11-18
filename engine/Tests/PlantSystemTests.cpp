#include <catch2/catch_test_macros.hpp>

#include "function/plants/PlantGroup.h"

#include <vsg/nodes/InstanceNode.h>
#include <vsg/nodes/LOD.h>

#include <vector>

namespace
{

    vsg::ref_ptr<vsg::InstanceNode> soleInstance(vsg::ref_ptr<vsg::Node> child)
    {
        auto lod = child.cast<vsg::LOD>();
        if (!lod || lod->children.size() != 1)
            return {};
        return lod->children.front().node.cast<vsg::InstanceNode>();
    }

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

    vsg::ref_ptr<vsg::InstanceNode> instanceHolding(vsg::ref_ptr<vsg::Group> group, const vsg::vec3& position)
    {
        if (!group)
            return {};
        for (auto& child : group->children)
        {
            auto instance = soleInstance(child);
            if (instance && holdsPosition(*instance, position))
                return instance;
        }
        return {};
    }

    aerovista::PlantInstance standInPlant(int index)
    {
        const int column = index % 10;
        const int row = index / 10;
        aerovista::PlantInstance plant;
        plant.type = 0;
        plant.position = {static_cast<float>((column - 4.5) * 10.0), static_cast<float>((row - 4.5) * 10.0), 0.0f};
        return plant;
    }

} // namespace

// =============================================================================
// 同一物种在 256 米边界两侧分成不同的 LOD，边界内的两棵进同一个
// （lod分层实例.md §2、§9，码 PLANT-cell-bin）。
// origin 与 inside 划进同一片
// =============================================================================
TEST_CASE("plants of one type share an LOD only inside the same 256 m square",
          "[unit][plants][PLANT-cell-bin]")
{
    const vsg::vec3 west{-0.1f, 0.0f, 0.0f};
    const vsg::vec3 origin{0.0f, 0.0f, 0.0f};
    const vsg::vec3 inside{255.9f, 0.0f, 0.0f};
    const vsg::vec3 next{256.0f, 0.0f, 0.0f};
    auto mesh = vsg::Node::create();

    std::vector<aerovista::PlantInstance> plants(4);
    plants[0].position = west;
    plants[1].position = origin;
    plants[2].position = inside;
    plants[3].position = next;

    auto group = aerovista::createPlantsGroup(plants, mesh);
    auto atOrigin = instanceHolding(group, origin);
    auto atWest = instanceHolding(group, west);
    auto atInside = instanceHolding(group, inside);
    auto atNext = instanceHolding(group, next);

    REQUIRE(atOrigin);
    REQUIRE(atWest);
    REQUIRE(atInside);
    REQUIRE(atNext);
    REQUIRE(atOrigin == atInside);
    REQUIRE(atOrigin != atWest);
    REQUIRE(atOrigin != atNext);
    REQUIRE(atOrigin->child == mesh);
}

// =============================================================================
// 植被系统设计 §4 的 100 棵同一种替身分成 4 个 LOD，每个 25 棵
// （lod分层实例.md §2、§9，码 PLANT-cell-bin）。
// 东、北都从 -45 米到 45 米，在 0 两侧各五棵，所以是四片而不是一片。
// =============================================================================
TEST_CASE("the 100-tree stand-in becomes four LODs of 25", "[unit][plants][PLANT-cell-bin]")
{
    std::vector<aerovista::PlantInstance> plants;
    plants.reserve(100);
    for (int index = 0; index < 100; ++index)
        plants.push_back(standInPlant(index));

    auto group = aerovista::createPlantsGroup(plants, vsg::Node::create());
    REQUIRE(group);
    REQUIRE(group->children.size() == 4);
    for (auto& child : group->children)
    {
        auto instance = soleInstance(child);
        REQUIRE(instance);
        REQUIRE(instance->instanceCount == 25);
    }
}
