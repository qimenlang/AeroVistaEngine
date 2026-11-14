#include "function/scene/InstanceAttach.h"

#include <iostream>

namespace
{
    constexpr const char* modelPath = "models/plants/SM_BlackAlder_Field_01_PP/SM_BlackAlder_Field_01_PP_LOD5.gltf";
    //constexpr const char* modelPath = "models/trees/SM_BlackAlder_Seedling_01/SM_BlackAlder_Seedling_01.gltf";

    constexpr int instancesPerSide = 100;
    constexpr int tileSide = 10;
    constexpr int tilesPerSide = instancesPerSide / tileSide;
    constexpr float instanceSpacing = 10.0f;
    const vsg::dvec3 instanceLla{39.9087, 116.3975, 0.0};

    static_assert(tilesPerSide * tilesPerSide == 100);
    static_assert(tileSide * tileSide * tilesPerSide * tilesPerSide == instancesPerSide * instancesPerSide);

    enum class InstanceAttachMode
    {
        SINGLE,
        SPLIT,
        SPLIT_WITH_CULL
    };

    // 改这一行切换三种挂法，用来对比帧率。
    constexpr InstanceAttachMode attachMode = InstanceAttachMode::SINGLE;

    constexpr int instancesPerTile()
    {
        return tileSide * tileSide;
    }

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

    vsg::vec3 instanceTranslation(int column, int row)
    {
        const float center = (instancesPerSide - 1) * 0.5f;
        const float east = (static_cast<float>(column) - center) * instanceSpacing;
        const float north = (static_cast<float>(row) - center) * instanceSpacing;
        return {east, north, 0.0f};
    }

    vsg::ref_ptr<vsg::vec3Array> allTranslations()
    {
        auto translations = vsg::vec3Array::create(instanceCount(), vsg::vec3{});
        for (int row = 0; row < instancesPerSide; ++row)
        {
            for (int column = 0; column < instancesPerSide; ++column)
            {
                const auto index = static_cast<std::size_t>(row * instancesPerSide + column);
                translations->at(index) = instanceTranslation(column, row);
            }
        }
        return translations;
    }

    vsg::ref_ptr<vsg::vec3Array> tileTranslations(int tileX, int tileY)
    {
        auto translations = vsg::vec3Array::create(instancesPerTile(), vsg::vec3{});
        std::size_t index = 0;
        for (int localY = 0; localY < tileSide; ++localY)
        {
            for (int localX = 0; localX < tileSide; ++localX)
            {
                translations->at(index) = instanceTranslation(tileX * tileSide + localX, tileY * tileSide + localY);
                ++index;
            }
        }
        return translations;
    }

    vsg::ref_ptr<vsg::InstanceNode> createInstanceNode(vsg::ref_ptr<vsg::vec3Array> translations,
                                                       vsg::ref_ptr<vsg::Node> subgraph)
    {
        const auto count = static_cast<std::uint32_t>(translations->valueCount());
        auto instance = vsg::InstanceNode::create();
        instance->setTranslations(translations);
        instance->setRotations(vsg::quatArray::create(count, vsg::quat{0.0f, 0.0f, 0.0f, 1.0f}));
        instance->setScales(vsg::vec3Array::create(count, vsg::vec3{1.0f, 1.0f, 1.0f}));
        instance->firstInstance = 0;
        instance->instanceCount = count;
        instance->child = subgraph;
        return instance;
    }

    vsg::ref_ptr<vsg::InstanceNode> createTile(int tileX, int tileY, vsg::ref_ptr<vsg::Node> subgraph)
    {
        return createInstanceNode(tileTranslations(tileX, tileY), subgraph);
    }

    vsg::dbox modelBounds(const vsg::ref_ptr<vsg::Node>& subgraph)
    {
        auto probe = createInstanceNode(vsg::vec3Array::create(1, vsg::vec3{}), subgraph);

        vsg::ComputeBounds computeBounds;
        probe->accept(computeBounds);
        return computeBounds.bounds;
    }

    vsg::dsphere tileSphere(int tileX, int tileY, const vsg::dbox& model)
    {
        const vsg::vec3 originMin = instanceTranslation(tileX * tileSide, tileY * tileSide);
        const vsg::vec3 originMax = instanceTranslation(tileX * tileSide + tileSide - 1, tileY * tileSide + tileSide - 1);
        vsg::dvec3 minPoint{originMin.x, originMin.y, originMin.z};
        vsg::dvec3 maxPoint{originMax.x, originMax.y, originMax.z};
        if (model.valid())
        {
            minPoint += model.min;
            maxPoint += model.max;
        }
        const vsg::dvec3 centre = (minPoint + maxPoint) * 0.5;
        return {centre, vsg::length(maxPoint - minPoint) * 0.5};
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

        // 启动时的场景包围盒不纳入实例组，避免整片树林改掉初始相机。
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
        auto anchor = InstanceAnchor::create(matrix);
        anchor->addChild(createInstanceNode(allTranslations(), subgraph));
        scene.addChild(anchor);
    }

    void addSplitInstanceGroup(vsg::Group& scene, const vsg::dmat4& matrix, vsg::ref_ptr<vsg::Node> subgraph)
    {
        auto anchor = InstanceAnchor::create(matrix);
        for (int tileY = 0; tileY < tilesPerSide; ++tileY)
        {
            for (int tileX = 0; tileX < tilesPerSide; ++tileX)
            {
                anchor->addChild(createTile(tileX, tileY, subgraph));
            }
        }
        scene.addChild(anchor);
    }

    void addSplitInstanceWithCullGroup(vsg::Group& scene, const vsg::dmat4& matrix, vsg::ref_ptr<vsg::Node> subgraph)
    {
        const vsg::dbox model = modelBounds(subgraph);
        if (!model.valid())
            std::cerr << "[WARN] instance model bounds unavailable, tile spheres use instance origins only" << std::endl;

        auto anchor = InstanceAnchor::create(matrix);
        for (int tileY = 0; tileY < tilesPerSide; ++tileY)
        {
            for (int tileX = 0; tileX < tilesPerSide; ++tileX)
            {
                auto tile = createTile(tileX, tileY, subgraph);
                anchor->addChild(vsg::CullNode::create(tileSphere(tileX, tileY, model), tile));
            }
        }
        scene.addChild(anchor);
    }

    void addInstances(vsg::Group& scene, const vsg::dmat4& matrix, vsg::ref_ptr<vsg::Node> subgraph)
    {
        switch (attachMode)
        {
        case InstanceAttachMode::SINGLE:
            addInstanceGroup(scene, matrix, subgraph);
            break;
        case InstanceAttachMode::SPLIT:
            addSplitInstanceGroup(scene, matrix, subgraph);
            break;
        case InstanceAttachMode::SPLIT_WITH_CULL:
            addSplitInstanceWithCullGroup(scene, matrix, subgraph);
            break;
        }
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
    addInstances(scene, anchorMatrix(ellipsoid.get()), loaded);
}
