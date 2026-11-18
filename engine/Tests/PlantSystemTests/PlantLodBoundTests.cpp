#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "function/plants/PlantGroup.h"

#include <vsg/core/Array.h>
#include <vsg/nodes/VertexDraw.h>

#include <cmath>

namespace
{

    vsg::ref_ptr<vsg::VertexDraw> meshFromVertices(vsg::ref_ptr<vsg::vec3Array> vertices)
    {
        auto draw = vsg::VertexDraw::create();
        draw->assignArrays({vertices});
        draw->vertexCount = static_cast<std::uint32_t>(vertices->size());
        draw->instanceCount = 1;
        return draw;
    }

    vsg::ref_ptr<vsg::VertexDraw> meshBox()
    {
        auto vertices = vsg::vec3Array::create(2);
        vertices->set(0, {0.0f, 0.0f, 0.0f});
        vertices->set(1, {2.0f, 3.0f, 6.0f});
        return meshFromVertices(vertices);
    }

    vsg::ref_ptr<vsg::VertexDraw> meshPoint()
    {
        auto vertices = vsg::vec3Array::create(1);
        vertices->set(0, {0.0f, 0.0f, 0.0f});
        return meshFromVertices(vertices);
    }

    double squareRadius(double sideLength)
    {
        return sideLength * 0.5 * std::sqrt(2.0);
    }

    double cellHalf()
    {
        return aerovista::cellSizeMeters * 0.5;
    }

    aerovista::PlantTile centeredTile()
    {
        aerovista::PlantTile tile;
        const double half = cellHalf();
        tile.key.center = {half, half, 0.0};
        tile.key.sideLength = aerovista::cellSizeMeters;
        return tile;
    }

} // namespace

// =============================================================================
// 分片写入方格中心和边长。中心是 (片号 + 0.5) × 边长，高度为 0
// （lod分层实例.md §4、§8，码 PLANT-lod-bound）。
// x = -0.1 的东向片号是 -1，所以球心 x 是负的半个边长。
// 不验收 map 的遍历顺序。
// =============================================================================
TEST_CASE("a split tile records the center of its cell square", "[unit][plants][PLANT-lod-bound]")
{
    std::vector<aerovista::PlantInstance> plants(2);
    plants[0].transform.position = {0.0f, 0.0f, 0.0f};
    plants[1].transform.position = {-0.1f, 0.0f, 0.0f};

    auto tiles = aerovista::splitPlantTiles(plants);
    auto origin = tiles.find({0, 0, 0});
    auto west = tiles.find({0, -1, 0});

    REQUIRE(origin != tiles.end());
    REQUIRE(west != tiles.end());
    REQUIRE(origin->second.key.sideLength == Catch::Approx(aerovista::cellSizeMeters));
    REQUIRE(origin->second.key.center.x == Catch::Approx(cellHalf()));
    REQUIRE(origin->second.key.center.y == Catch::Approx(cellHalf()));
    REQUIRE(origin->second.key.center.z == Catch::Approx(0.0));
    REQUIRE(west->second.key.center.x == Catch::Approx(-cellHalf()));
    REQUIRE(west->second.key.center.y == Catch::Approx(cellHalf()));
    REQUIRE(west->second.key.center.z == Catch::Approx(0.0));
}

// =============================================================================
// 片包围球的球心是 PlantTile 上的方格中心，半径加上最细网格的单株半径乘最大缩放
// （lod分层实例.md §4、§8，码 PLANT-lod-bound）。
// 网格两个顶点 (0,0,0) 与 (2,3,6)，对角线长 7，单株半径 3.5。缩放 (2,3,4) 取 4。
// 不验收占比。
// =============================================================================
TEST_CASE("a tile LOD bound uses the cell center and the finest mesh radius", "[unit][plants][PLANT-lod-bound]")
{
    auto tile = centeredTile();
    tile.poses.resize(1);
    tile.poses[0].scale = {2.0f, 3.0f, 4.0f};

    auto lod = aerovista::createPlantLods(tile, {meshBox()});

    REQUIRE(lod);
    REQUIRE(lod->bound.center.x == Catch::Approx(cellHalf()));
    REQUIRE(lod->bound.center.y == Catch::Approx(cellHalf()));
    REQUIRE(lod->bound.center.z == Catch::Approx(0.0));
    REQUIRE(lod->bound.radius == Catch::Approx(squareRadius(aerovista::cellSizeMeters) + 3.5 * 4.0));
}

// =============================================================================
// 最细网格包围盒退化成一个点时，单株半径按 0 写进片包围球
// （lod分层实例.md §4、§8，码 PLANT-lod-bound）。
// 不验收占比。
// =============================================================================
TEST_CASE("a point mesh adds nothing to the tile radius", "[unit][plants][PLANT-lod-bound]")
{
    auto tile = centeredTile();
    tile.poses.resize(1);
    tile.poses[0].scale = {2.0f, 2.0f, 2.0f};

    auto lod = aerovista::createPlantLods(tile, {meshPoint()});

    REQUIRE(lod);
    REQUIRE(lod->bound.radius == Catch::Approx(squareRadius(aerovista::cellSizeMeters)));
}

// =============================================================================
// PlantTile 没有位姿时不建 LOD
// （lod分层实例.md §4、§8，码 PLANT-lod-bound）。
// 网格是有包围盒的，所以空的是位姿，不是网格。
// =============================================================================
TEST_CASE("a tile with no poses produces no LOD", "[unit][plants][PLANT-lod-bound]")
{
    auto tile = centeredTile();

    auto lod = aerovista::createPlantLods(tile, {meshBox()});

    REQUIRE_FALSE(lod);
}

// =============================================================================
// 单株半径取各档包围盒对角线一半的最大值，较粗的一档更大时用它
// （lod分层实例.md §4、§8，码 PLANT-lod-bound）。
// 第一档两个顶点 (0,0,0) 与 (2,0,0)，对角线一半是 1。第二档是 (2,3,6)，一半是 3.5。缩放是 1。
// 不验收占比。
// =============================================================================
TEST_CASE("the tile radius uses the largest mesh bounding box", "[unit][plants][PLANT-lod-bound]")
{
    auto small = vsg::vec3Array::create(2);
    small->set(0, {0.0f, 0.0f, 0.0f});
    small->set(1, {2.0f, 0.0f, 0.0f});

    auto tile = centeredTile();
    tile.poses.resize(1);

    auto lod = aerovista::createPlantLods(tile, {meshFromVertices(small), meshBox()});

    REQUIRE(lod);
    REQUIRE(lod->bound.radius == Catch::Approx(squareRadius(aerovista::cellSizeMeters) + 3.5));
}

// =============================================================================
// 每一档都没有包围盒时单株半径按 0，片包围球仍写入
// （lod分层实例.md §4、§8，码 PLANT-lod-bound）。
// 位姿不是空的。没有顶点的节点算没有包围盒。不验收占比。
// =============================================================================
TEST_CASE("a mesh without bounds adds nothing to the tile radius", "[unit][plants][PLANT-lod-bound]")
{
    auto tile = centeredTile();
    tile.poses.resize(1);

    auto lod = aerovista::createPlantLods(tile, {vsg::Node::create()});

    REQUIRE(lod);
    REQUIRE(lod->bound.radius == Catch::Approx(squareRadius(aerovista::cellSizeMeters)));
}
