#include "function/scene/InstanceAttach.h"

#include <iostream>

namespace
{
    constexpr const char* modelPath = "models/trees/SM_BlackAlder_Field_01/SM_BlackAlder_Field_01.gltf";
    //constexpr const char* modelPath = "models/trees/SM_BlackAlder_Seedling_01/SM_BlackAlder_Seedling_01.gltf";

    constexpr int instancesPerSide = 8;
    constexpr float instanceSpacing = 10.0f;
    const vsg::dvec3 instanceLla{39.9087, 116.3975, 0.0};

    constexpr int instanceCount()
    {
        return instancesPerSide * instancesPerSide;
    }

    vsg::dmat4 anchorMatrix(const vsg::EllipsoidModel* ellipsoid)
    {
        if (!ellipsoid)
            return {};
        return ellipsoid->computeLocalToWorldTransform(instanceLla);
    }

    vsg::ref_ptr<vsg::vec3Array> instanceTranslations()
    {
        const int count = instanceCount();
        const float center = (instancesPerSide - 1) * 0.5f;
        auto translations = vsg::vec3Array::create(count, vsg::vec3{});
        for (int k = 0; k < count; ++k)
        {
            const float east = static_cast<float>(k % instancesPerSide) - center;
            const float north = static_cast<float>(k / instancesPerSide) - center;
            translations->at(static_cast<std::size_t>(k)) = vsg::vec3{east * instanceSpacing, north * instanceSpacing, 0.0f};
        }
        return translations;
    }

    vsg::ref_ptr<vsg::quatArray> instanceRotations()
    {
        return vsg::quatArray::create(instanceCount(), vsg::quat{0.0f, 0.0f, 0.0f, 1.0f});
    }

    vsg::ref_ptr<vsg::vec3Array> instanceScales()
    {
        return vsg::vec3Array::create(instanceCount(), vsg::vec3{1.0f, 1.0f, 1.0f});
    }

    class InstanceAnchor : public vsg::Inherit<vsg::MatrixTransform, InstanceAnchor>
    {
    public:
        explicit InstanceAnchor(const vsg::dmat4& inMatrix) :
            Inherit(inMatrix)
        {
        }

        void accept(vsg::Visitor& visitor) override
        {
            visitor.apply(static_cast<vsg::MatrixTransform&>(*this));
        }

        // 在 ComputeBounds 里停住,包围盒不计算实例组
        void accept(vsg::ConstVisitor& visitor) const override
        {
            if (dynamic_cast<const vsg::ComputeBounds*>(&visitor))
                return;
            visitor.apply(static_cast<const vsg::MatrixTransform&>(*this));
        }

        // 绘制仍按 MatrixTransform 入栈
        void accept(vsg::RecordTraversal& visitor) const override
        {
            visitor.apply(static_cast<const vsg::MatrixTransform&>(*this));
        }
    };

    void addInstanceGroup(vsg::Group& scene, const vsg::dmat4& matrix, vsg::ref_ptr<vsg::Node> subgraph)
    {
        auto instance = vsg::InstanceNode::create();
        instance->setTranslations(instanceTranslations());
        instance->setRotations(instanceRotations());
        instance->setScales(instanceScales());
        instance->firstInstance = 0;
        instance->instanceCount = static_cast<std::uint32_t>(instanceCount());
        instance->child = subgraph;

        auto anchor = InstanceAnchor::create(matrix);
        anchor->addChild(instance);
        scene.addChild(anchor);
    }
} // namespace

void attachInstanceModel(vsg::Group& scene, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid,
                         vsg::ref_ptr<vsg::Options> options)
{
    // 不改调用方的 Options，避免实体加载也被打开 instanceNodeHint。
    auto instanceOptions = vsg::Options::create(*options);
    instanceOptions->instanceNodeHint = vsg::Options::INSTANCE_TRANSLATIONS | vsg::Options::INSTANCE_ROTATIONS |
                                        vsg::Options::INSTANCE_SCALES;

    const vsg::Path path = vsg::Path(RESOURCE_DIR) / modelPath;
    auto loaded = vsg::read(path, instanceOptions).cast<vsg::Node>();
    if (!loaded)
    {
        std::cerr << "[WARN] skip instance group, model load failed: " << modelPath << std::endl;
        return;
    }
    addInstanceGroup(scene, anchorMatrix(ellipsoid.get()), loaded);
}
