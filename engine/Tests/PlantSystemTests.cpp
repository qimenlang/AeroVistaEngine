#include <catch2/catch_test_macros.hpp>

#include "function/plants/PlantGroup.h"

#include <map>
#include <vector>

namespace
{

    aerovista::PlantInstance standInPlant(int index)
    {
        const int column = index % 10;
        const int row = index / 10;
        aerovista::PlantInstance plant;
        plant.type = 0;
        plant.transform.position = {static_cast<float>((column - 4.5) * 10.0), static_cast<float>((row - 4.5) * 10.0), 0.0f};
        return plant;
    }

    const std::vector<aerovista::PlantTransform>* posesAt(const std::map<aerovista::CellKey, std::vector<aerovista::PlantTransform>>& tiles,
                                                          const aerovista::CellKey& key)
    {
        const auto found = tiles.find(key);
        if (found == tiles.end())
            return nullptr;
        return &found->second;
    }

} // namespace

// =============================================================================
// 同一物种在 256 米边界两侧用不同的片号，边界内的两棵共用 {0, 0, 0}
// （lod分层实例.md §2、§8，码 PLANT-cell-bin）。
// 组内顺序与输入一致：先 origin，后 inside。后一棵的旋转和缩放原样留在该键上。
// 不验收 map 的遍历顺序。
// =============================================================================
TEST_CASE("plants of one type share a tile only inside the same 256 m square", "[unit][plants][PLANT-cell-bin]")
{
    const vsg::vec3 west{-0.1f, 0.0f, 0.0f};
    const vsg::vec3 origin{0.0f, 0.0f, 0.0f};
    const vsg::vec3 inside{255.9f, 0.0f, 0.0f};
    const vsg::vec3 next{256.0f, 0.0f, 0.0f};
    const vsg::quat insideRotation{0.0f, 0.0f, 1.0f, 0.0f};
    const vsg::vec3 insideScale{2.0f, 3.0f, 4.0f};
    const aerovista::CellKey westKey{0, -1, 0};
    const aerovista::CellKey originKey{0, 0, 0};
    const aerovista::CellKey nextKey{0, 1, 0};

    std::vector<aerovista::PlantInstance> plants(4);
    plants[0].transform.position = west;
    plants[1].transform.position = origin;
    plants[2].transform.position = inside;
    plants[2].transform.rotation = insideRotation;
    plants[2].transform.scale = insideScale;
    plants[3].transform.position = next;

    auto tiles = aerovista::splitPlantTiles(plants);
    auto atWest = posesAt(tiles, westKey);
    auto atOrigin = posesAt(tiles, originKey);
    auto atNext = posesAt(tiles, nextKey);

    REQUIRE(tiles.size() == 3);
    REQUIRE(atWest);
    REQUIRE(atOrigin);
    REQUIRE(atNext);
    REQUIRE(atWest->size() == 1);
    REQUIRE(atWest->front().position == west);
    REQUIRE(atNext->size() == 1);
    REQUIRE(atNext->front().position == next);
    REQUIRE(atOrigin->size() == 2);
    REQUIRE(atOrigin->front().position == origin);
    REQUIRE(atOrigin->back().position == inside);
    REQUIRE(atOrigin->back().rotation == insideRotation);
    REQUIRE(atOrigin->back().scale == insideScale);
}

// =============================================================================
// 植被系统设计 §4 的 100 棵同一种替身分成四个片号，每片 25 棵
// （lod分层实例.md §2、§8，码 PLANT-cell-bin）。
// 东、北都从 -45 米到 45 米，在 0 两侧各五棵，所以是四片而不是一片。
// 不验收 map 的遍历顺序。
// =============================================================================
TEST_CASE("the 100-tree stand-in becomes four tiles of 25", "[unit][plants][PLANT-cell-bin]")
{
    const aerovista::CellKey keys[] = {
        {0, -1, -1},
        {0, -1, 0},
        {0, 0, -1},
        {0, 0, 0},
    };

    std::vector<aerovista::PlantInstance> plants;
    plants.reserve(100);
    for (int index = 0; index < 100; ++index)
        plants.push_back(standInPlant(index));

    auto tiles = aerovista::splitPlantTiles(plants);

    REQUIRE(tiles.size() == 4);
    for (const auto& key : keys)
    {
        auto poses = posesAt(tiles, key);
        REQUIRE(poses);
        REQUIRE(poses->size() == 25);
    }
}

// =============================================================================
// 不同物种即使站在同一点也分成两个片号
// （lod分层实例.md §2、§8，码 PLANT-cell-bin）。
// 不验收 map 的遍历顺序。
// =============================================================================
TEST_CASE("two species at the same point become two tiles", "[unit][plants][PLANT-cell-bin]")
{
    const aerovista::CellKey alderKey{0, 0, 0};
    const aerovista::CellKey birchKey{1, 0, 0};

    std::vector<aerovista::PlantInstance> plants(2);
    plants[1].type = 1;

    auto tiles = aerovista::splitPlantTiles(plants);
    auto alder = posesAt(tiles, alderKey);
    auto birch = posesAt(tiles, birchKey);

    REQUIRE(tiles.size() == 2);
    REQUIRE(alder);
    REQUIRE(birch);
    REQUIRE(alder->size() == 1);
    REQUIRE(birch->size() == 1);
}
