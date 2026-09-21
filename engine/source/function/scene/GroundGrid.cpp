#include "function/scene/GroundGrid.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
    constexpr int kMaxCells = 400;
    constexpr float kLinesBelowOriginM = -0.25f;
    const vsg::vec4 kMinorColor{0.863f, 0.769f, 0.188f, 1.0f};
    const vsg::vec4 kMajorColor{1.0f, 1.0f, 1.0f, 1.0f};

    int cellCountAcross(double halfExtentM, double cellM)
    {
        return static_cast<int>(std::lround((2.0 * halfExtentM) / cellM));
    }

    void appendSegment(std::vector<vsg::vec3>& vertices, const vsg::vec3& a, const vsg::vec3& b)
    {
        vertices.push_back(a);
        vertices.push_back(b);
    }

    void collectGridSegments(const GroundGridConfig& spec, std::vector<vsg::vec3>& major,
                             std::vector<vsg::vec3>& minor)
    {
        const int cells = std::clamp(cellCountAcross(spec.halfExtentM, spec.cellM), 1, kMaxCells);
        const auto half = static_cast<float>(spec.halfExtentM);
        const float step = (2.0f * half) / static_cast<float>(cells);
        const int majorEvery = std::max(1, spec.majorEvery);

        for (int n = 0; n <= cells; ++n)
        {
            const float t = -half + static_cast<float>(n) * step;
            auto& dest = ((n % majorEvery) == 0) ? major : minor;
            appendSegment(dest, {t, -half, kLinesBelowOriginM}, {t, half, kLinesBelowOriginM});
            appendSegment(dest, {-half, t, kLinesBelowOriginM}, {half, t, kLinesBelowOriginM});
        }
    }

    vsg::ref_ptr<vsg::vec3Array> toVec3Array(const std::vector<vsg::vec3>& src)
    {
        auto vertices = vsg::vec3Array::create(static_cast<uint32_t>(src.size()));
        for (uint32_t i = 0; i < vertices->size(); ++i)
            vertices->set(i, src[i]);
        return vertices;
    }

    vsg::ref_ptr<vsg::VertexIndexDraw> createSolidLineDraw(const std::vector<vsg::vec3>& segments,
                                                           const vsg::vec4& color)
    {
        if (segments.empty())
            return {};

        auto vertices = toVec3Array(segments);
        const uint32_t count = vertices->size();
        auto normals = vsg::vec3Array::create(count, vsg::vec3(0.0f, 0.0f, 1.0f));
        auto texcoords = vsg::vec2Array::create(count, vsg::vec2(0.0f, 0.0f));
        auto colors = vsg::vec4Array::create({color});
        auto indices = vsg::ushortArray::create(count);
        for (uint32_t i = 0; i < count; ++i)
            indices->set(i, static_cast<uint16_t>(i));

        auto vid = vsg::VertexIndexDraw::create();
        vid->assignArrays({vertices, normals, texcoords, colors});
        vid->assignIndices(indices);
        vid->indexCount = count;
        vid->instanceCount = 1;
        return vid;
    }
} // namespace

int groundGridLineCount(double halfExtentM, double cellM)
{
    return cellCountAcross(halfExtentM, cellM) + 1;
}

vsg::ref_ptr<vsg::Node> createEnuGroundGrid(const GroundGridConfig& spec)
{
    std::vector<vsg::vec3> major;
    std::vector<vsg::vec3> minor;
    collectGridSegments(spec, major, minor);

    auto builder = vsg::Builder::create();
    vsg::StateInfo state;
    state.lighting = false;
    state.wireframe = true;
    auto stateGroup = builder->createStateGroup(state);
    if (!stateGroup)
        return {};

    if (auto draw = createSolidLineDraw(minor, kMinorColor))
        stateGroup->addChild(draw);
    if (auto draw = createSolidLineDraw(major, kMajorColor))
        stateGroup->addChild(draw);
    return stateGroup;
}

vsg::ref_ptr<vsg::MatrixTransform> createPinnedGroundGrid(const vsg::EllipsoidModel& ellipsoid,
                                                          const GroundGridConfig& spec)
{
    const vsg::dvec3 lla{spec.lla.x, spec.lla.y, spec.lla.z};
    auto transform = vsg::MatrixTransform::create(ellipsoid.computeLocalToWorldTransform(lla));
    transform->setValue(kGroundGridObjectKey, true);
    transform->addChild(createEnuGroundGrid(spec));
    return transform;
}
