#include "function/scene/LodAttach.h"

#include <array>
#include <iostream>
#include <string>

namespace
{
    constexpr const char* modelDirectory = "models/plants/SM_BlackAlder_Field_01_PP";
    constexpr const char* modelPrefix = "SM_BlackAlder_Field_01_PP_LOD";

    // 与实例树林同一 LLA，本地偏移到林缘外侧，方便单独观察 LOD 切换。
    const vsg::dvec3 lodLla{39.9087, 116.3975, 0.0};
    const vsg::dvec3 localOffset{0.0, 0.0, 0.0};

    // 高精度在前；最后一档 0.0 保证远处总有可见网格。
    constexpr std::array<double, 6> screenHeightRatios{
        0.25,
        0.12,
        0.06,
        0.03,
        0.015,
        0.0};

    vsg::dmat4 lodWorldMatrix(const vsg::EllipsoidModel* ellipsoid)
    {
        const vsg::dmat4 local = vsg::translate(localOffset);
        if (!ellipsoid)
            return local;
        return ellipsoid->computeLocalToWorldTransform(lodLla) * local;
    }

    vsg::Path lodModelPath(int lodIndex)
    {
        return vsg::Path(RESOURCE_DIR) / modelDirectory /
               (std::string(modelPrefix) + std::to_string(lodIndex) + ".gltf");
    }

    vsg::dsphere computeBound(const vsg::ref_ptr<vsg::Node>& node)
    {
        vsg::ComputeBounds computeBounds;
        node->accept(computeBounds);
        const auto& bounds = computeBounds.bounds;
        if (!bounds.valid())
            return {};

        const vsg::dvec3 centre = (bounds.min + bounds.max) * 0.5;
        return {centre, vsg::length(bounds.max - bounds.min) * 0.5};
    }

    vsg::ref_ptr<vsg::LOD> createBlackAlderLod(vsg::ref_ptr<vsg::Options> options)
    {
        auto lod = vsg::LOD::create();
        lod->children.reserve(screenHeightRatios.size());

        for (std::size_t i = 0; i < screenHeightRatios.size(); ++i)
        {
            const int lodIndex = static_cast<int>(i + 1);
            const vsg::Path path = lodModelPath(lodIndex);
            auto loaded = vsg::read(path, options).cast<vsg::Node>();
            if (!loaded)
            {
                std::cerr << "[WARN] skip LOD demo, model load failed: " << path.string() << std::endl;
                return {};
            }

            if (i == 0)
            {
                lod->bound = computeBound(loaded);
                if (!lod->bound.valid())
                {
                    std::cerr << "[WARN] skip LOD demo, LOD1 bounds unavailable" << std::endl;
                    return {};
                }
            }

            lod->addChild(vsg::LOD::Child{screenHeightRatios[i], loaded});
        }

        return lod;
    }
} // namespace

void attachLodModel(vsg::Group& scene, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid,
                    vsg::ref_ptr<vsg::Options> options)
{
    auto lod = createBlackAlderLod(options);
    if (!lod)
        return;

    auto transform = vsg::MatrixTransform::create(lodWorldMatrix(ellipsoid.get()));
    transform->addChild(lod);
    scene.addChild(transform);

    std::cout << "[INFO] attached BlackAlder vsg::LOD demo (LOD1–LOD6, screen-height ratios)" << std::endl;
}
