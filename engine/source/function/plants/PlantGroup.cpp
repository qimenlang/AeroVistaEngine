#include "function/plants/PlantGroup.h"

#include <vsg/core/Array.h>
#include <vsg/nodes/InstanceNode.h>
#include <vsg/nodes/LOD.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace
{

    constexpr double cellSizeMeters = 256.0;

    // 负向要用 floor。整数除法会向 0 截断，-0.1 米会进错片。
    int cellIndex(float meters)
    {
        return static_cast<int>(std::floor(static_cast<double>(meters) / cellSizeMeters));
    }

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
    };

    CellKey keyOf(const aerovista::PlantInstance& plant)
    {
        return {plant.type, cellIndex(plant.position.x), cellIndex(plant.position.y)};
    }

    vsg::ref_ptr<vsg::LOD> createTileLod(const std::vector<const aerovista::PlantInstance*>& members,
                                         vsg::ref_ptr<vsg::Node> mesh)
    {
        const auto count = static_cast<std::uint32_t>(members.size());
        auto translations = vsg::vec3Array::create(count);
        auto rotations = vsg::quatArray::create(count);
        auto scales = vsg::vec3Array::create(count);
        for (std::uint32_t index = 0; index < count; ++index)
        {
            translations->at(index) = members[index]->position;
            rotations->at(index) = members[index]->rotation;
            scales->at(index) = members[index]->scale;
        }

        auto instance = vsg::InstanceNode::create();
        instance->setTranslations(translations);
        instance->setRotations(rotations);
        instance->setScales(scales);
        instance->firstInstance = 0;
        instance->instanceCount = count;
        instance->child = mesh;

        auto lod = vsg::LOD::create();
        lod->addChild({0.0, instance});
        return lod;
    }

} // namespace

namespace aerovista
{

    vsg::ref_ptr<vsg::Group> createPlantsGroup(const std::vector<PlantInstance>& plants, vsg::ref_ptr<vsg::Node> mesh)
    {
        std::map<CellKey, std::vector<const PlantInstance*>> cells;
        for (const auto& plant : plants)
            cells[keyOf(plant)].push_back(&plant);

        auto group = vsg::Group::create();
        for (const auto& entry : cells)
            group->addChild(createTileLod(entry.second, mesh));
        return group;
    }

} // namespace aerovista
