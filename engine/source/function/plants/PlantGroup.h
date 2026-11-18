#pragma once

#include <vsg/maths/quat.h>
#include <vsg/maths/vec3.h>
#include <vsg/nodes/Group.h>
#include <vsg/nodes/Node.h>

#include <vector>

namespace aerovista
{

    struct PlantInstance
    {
        int type = 0;
        vsg::vec3 position;
        vsg::quat rotation{0.0f, 0.0f, 0.0f, 1.0f};
        vsg::vec3 scale{1.0f, 1.0f, 1.0f};
    };

    // 调用方传入已读入的网格。锚点由外层 MatrixTransform 承担，不在这里。
    vsg::ref_ptr<vsg::Group> createPlantsGroup(const std::vector<PlantInstance>& plants,
                                               vsg::ref_ptr<vsg::Node> mesh);

} // namespace aerovista
