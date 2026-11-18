#pragma once

#include <vsg/maths/quat.h>
#include <vsg/maths/vec3.h>
#include <vsg/nodes/LOD.h>
#include <vsg/nodes/Node.h>

#include <map>
#include <vector>

namespace aerovista
{
    // 划片边长的唯一字面量。JSON 配置之前，分片和测试都用这个值。
    constexpr double cellSizeMeters = 256.0;
    struct PlantTransform
    {
        vsg::vec3 position;
        vsg::quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
        vsg::vec3 scale{1.0f, 1.0f, 1.0f};
    };

    struct PlantInstance
    {
        int type = 0;
        PlantTransform transform;
    };

    // east、north 是 floor(米 / cellSizeMeters) 的片号，不是米。天向不参与。
    struct CellKey
    {
        int type = 0;
        int east = 0;
        int north = 0;
        vsg::dvec3 center;
        double sideLength = cellSizeMeters;

        bool operator<(const CellKey& rhs) const
        {
            if (type != rhs.type)
                return type < rhs.type;
            if (east != rhs.east)
                return east < rhs.east;
            return north < rhs.north;
        }

        bool operator==(const CellKey& rhs) const
        {
            return type == rhs.type && east == rhs.east && north == rhs.north;
        }
    };

    struct PlantTile
    {
        CellKey key;
        std::vector<PlantTransform> poses;
    };

    // 键是物种和片号。值里的 key 与 map 的键相同，位姿副本的组内顺序与输入一致。
    // map 的遍历顺序不验收。center、sideLength 不参与键的比较。
    std::map<CellKey, PlantTile> splitPlantTiles(const std::vector<PlantInstance>& plants);

    // 调用方传入一片 PlantTile，以及从细到粗的网格。这里不读 glTF，也不再分片，也不从位姿重算方格中心。
    // 各档占比暂时是 (档数 - 1 - 序号) / 档数，序号从 0 起。
    // 没有位姿或网格为空时返回空。网格没有包围盒时单株半径按 0，仍写片包围球。
    vsg::ref_ptr<vsg::LOD> createPlantLods(const PlantTile& tile, const std::vector<vsg::ref_ptr<vsg::Node>>& meshes);

} // namespace aerovista
