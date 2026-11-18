#pragma once

#include <vsg/maths/quat.h>
#include <vsg/maths/vec3.h>
#include <vsg/nodes/LOD.h>
#include <vsg/nodes/Node.h>

#include <map>
#include <vector>

namespace aerovista
{

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

    // east、north 是 floor(米 / 256) 的片号，不是米。天向不参与。
    struct CellKey
    {
        int type = 0;
        int east = 0;
        int north = 0;

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

    // 键是物种和片号。值是该片该物种的位姿副本，组内顺序与输入一致。
    // map 的遍历顺序不验收。
    std::map<CellKey, std::vector<PlantTransform>> splitPlantTiles(const std::vector<PlantInstance>& plants);

    // 调用方传入一个键上的位姿，以及从细到粗的网格。这里不读 glTF，也不再分片。
    // 各档占比暂时是 (档数 - 1 - 序号) / 档数，序号从 0 起。网格为空时返回空。
    vsg::ref_ptr<vsg::LOD> createPlantLods(const std::vector<PlantTransform>& poses,
                                           const std::vector<vsg::ref_ptr<vsg::Node>>& meshes);

} // namespace aerovista
