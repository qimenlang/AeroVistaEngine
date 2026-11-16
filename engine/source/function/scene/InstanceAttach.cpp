#include "function/scene/InstanceAttach.h"

#include <array>
#include <iostream>
#include <string>

namespace
{
    constexpr const char* modelDirectory = "models/plants/SM_BlackAlder_Field_01_PP";
    constexpr const char* modelPrefix = "SM_BlackAlder_Field_01_PP_LOD";
    // 非 LOD 模式仍用单档网格，便于和旧三种挂法对比。
    constexpr const char* singleModelPath =
        "models/plants/SM_BlackAlder_Field_01_PP/SM_BlackAlder_Field_01_PP_LOD6.gltf";

    constexpr int instancesPerSide = 30;
    constexpr int tileSide = 10;
    constexpr int tilesPerSide = instancesPerSide / tileSide;
    constexpr float instanceSpacing = 10.0f;
    const vsg::dvec3 instanceLla{39.9087, 116.3975, 0.0};

    static_assert(tilesPerSide * tilesPerSide == 9);
    static_assert(tileSide * tileSide * tilesPerSide * tilesPerSide == instancesPerSide * instancesPerSide);

    enum class InstanceAttachMode
    {
        SINGLE,
        SPLIT,
        SPLIT_WITH_CULL,
        SPLIT_WITH_LOD,
        // 同数量、同划片 Cull：普通 MatrixTransform 树，对比 InstanceNode 收益。
        NONE_INSTANCE
    };

    // 改这一行切换挂法，用来对比帧率。
    constexpr InstanceAttachMode attachMode = InstanceAttachMode::SINGLE;

    // 片级球用的占比；半径远大于单株，需目测再调（见 1关键决策.md #6）。
    constexpr std::array<double, 6> tileScreenHeightRatios{
        0.25,
        0.12,
        0.06,
        0.03,
        0.015,
        0.0};

    constexpr int instancesPerTile()
    {
        return tileSide * tileSide;
    }

    constexpr int instanceCount()
    {
        return instancesPerSide * instancesPerSide;
    }

    constexpr int lodLevelCount()
    {
        return static_cast<int>(tileScreenHeightRatios.size());
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

    vsg::Path lodModelPath(int lodIndex)
    {
        return vsg::Path(RESOURCE_DIR) / modelDirectory /
               (std::string(modelPrefix) + std::to_string(lodIndex) + ".gltf");
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

    // 多档 LOD 共用同一份位姿 BufferInfo，避免片×档内存放大。
    vsg::ref_ptr<vsg::InstanceNode> createInstanceNodeWithSharedPose(vsg::ref_ptr<vsg::BufferInfo> translations,
                                                                     vsg::ref_ptr<vsg::BufferInfo> rotations,
                                                                     vsg::ref_ptr<vsg::BufferInfo> scales,
                                                                     std::uint32_t count,
                                                                     vsg::ref_ptr<vsg::Node> subgraph)
    {
        auto instance = vsg::InstanceNode::create();
        instance->translations = translations;
        instance->rotations = rotations;
        instance->scales = scales;
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

    // 非实例路径：直接对普通子图算包围盒（不能走 InstanceNode probe）。
    vsg::dbox plainModelBounds(const vsg::ref_ptr<vsg::Node>& subgraph)
    {
        vsg::ComputeBounds computeBounds;
        subgraph->accept(computeBounds);
        return computeBounds.bounds;
    }

    vsg::dsphere tileSphere(int tileX, int tileY, const vsg::dbox& model)
    {
        const vsg::vec3 originMin = instanceTranslation(tileX * tileSide, tileY * tileSide);
        const vsg::vec3 originMax =
            instanceTranslation(tileX * tileSide + tileSide - 1, tileY * tileSide + tileSide - 1);
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

    void addSplitInstanceWithCullGroup(vsg::Group& scene, const vsg::dmat4& matrix,
                                       vsg::ref_ptr<vsg::Node> subgraph)
    {
        const vsg::dbox model = modelBounds(subgraph);
        if (!model.valid())
            std::cerr << "[WARN] instance model bounds unavailable, tile spheres use instance origins only"
                      << std::endl;

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

    vsg::ref_ptr<vsg::Group> createTilePlainGroup(int tileX, int tileY, vsg::ref_ptr<vsg::Node> subgraph)
    {
        auto group = vsg::Group::create();
        group->children.reserve(static_cast<std::size_t>(instancesPerTile()));
        for (int localY = 0; localY < tileSide; ++localY)
        {
            for (int localX = 0; localX < tileSide; ++localX)
            {
                const vsg::vec3 pos = instanceTranslation(tileX * tileSide + localX, tileY * tileSide + localY);
                auto transform = vsg::MatrixTransform::create(vsg::translate(vsg::dvec3(pos.x, pos.y, pos.z)));
                transform->addChild(subgraph);
                group->addChild(transform);
            }
        }
        return group;
    }

    // 划片粒度与 SPLIT_WITH_CULL 一致，变量只剩「InstanceNode vs 普通 Node」。
    void addNoneInstanceWithCullGroup(vsg::Group& scene, const vsg::dmat4& matrix,
                                      vsg::ref_ptr<vsg::Node> subgraph)
    {
        const vsg::dbox model = plainModelBounds(subgraph);
        if (!model.valid())
            std::cerr << "[WARN] plain model bounds unavailable, tile spheres use instance origins only"
                      << std::endl;

        auto anchor = InstanceAnchor::create(matrix);
        for (int tileY = 0; tileY < tilesPerSide; ++tileY)
        {
            for (int tileX = 0; tileX < tilesPerSide; ++tileX)
            {
                auto tile = createTilePlainGroup(tileX, tileY, subgraph);
                anchor->addChild(vsg::CullNode::create(tileSphere(tileX, tileY, model), tile));
            }
        }
        scene.addChild(anchor);
    }

    using LodMeshes = std::array<vsg::ref_ptr<vsg::Node>, tileScreenHeightRatios.size()>;

    vsg::ref_ptr<vsg::LOD> createTileLod(int tileX, int tileY, const vsg::dbox& model,
                                         const LodMeshes& meshes,
                                         vsg::ref_ptr<vsg::vec3Array> translations)
    {
        const auto count = static_cast<std::uint32_t>(translations->valueCount());
        auto translationsInfo = vsg::BufferInfo::create(translations);
        auto rotationsInfo =
            vsg::BufferInfo::create(vsg::quatArray::create(count, vsg::quat{0.0f, 0.0f, 0.0f, 1.0f}));
        auto scalesInfo = vsg::BufferInfo::create(vsg::vec3Array::create(count, vsg::vec3{1.0f, 1.0f, 1.0f}));

        auto lod = vsg::LOD::create();
        lod->bound = tileSphere(tileX, tileY, model);
        lod->children.reserve(meshes.size());
        for (std::size_t i = 0; i < meshes.size(); ++i)
        {
            lod->addChild(vsg::LOD::Child{
                tileScreenHeightRatios[i],
                createInstanceNodeWithSharedPose(translationsInfo, rotationsInfo, scalesInfo, count, meshes[i])});
        }
        return lod;
    }

    void addSplitInstanceWithLodGroup(vsg::Group& scene, const vsg::dmat4& matrix, const LodMeshes& meshes)
    {
        const vsg::dbox model = modelBounds(meshes.front());
        if (!model.valid())
            std::cerr << "[WARN] instance LOD1 bounds unavailable, tile spheres use instance origins only"
                      << std::endl;

        auto anchor = InstanceAnchor::create(matrix);
        for (int tileY = 0; tileY < tilesPerSide; ++tileY)
        {
            for (int tileX = 0; tileX < tilesPerSide; ++tileX)
            {
                anchor->addChild(createTileLod(tileX, tileY, model, meshes, tileTranslations(tileX, tileY)));
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
        case InstanceAttachMode::SPLIT_WITH_LOD:
            // 由 attachInstanceModel 单独加载多档网格后走 addSplitInstanceWithLodGroup。
            break;
        case InstanceAttachMode::NONE_INSTANCE:
            // 由 attachInstanceModel 以普通 Node（无 instanceNodeHint）加载后挂接。
            break;
        }
    }

    LodMeshes loadLodMeshes(vsg::ref_ptr<vsg::Options> options)
    {
        LodMeshes meshes{};
        for (int lodIndex = 1; lodIndex <= lodLevelCount(); ++lodIndex)
        {
            const vsg::Path path = lodModelPath(lodIndex);
            auto loaded = vsg::read(path, options).cast<vsg::Node>();
            if (!loaded)
            {
                std::cerr << "[WARN] skip instance LOD group, model load failed: " << path.string() << std::endl;
                return {};
            }
            meshes[static_cast<std::size_t>(lodIndex - 1)] = loaded;
        }
        return meshes;
    }

    bool lodMeshesReady(const LodMeshes& meshes)
    {
        for (const auto& mesh : meshes)
        {
            if (!mesh)
                return false;
        }
        return true;
    }
} // namespace

void attachInstanceModel(vsg::Group& scene, vsg::ref_ptr<vsg::EllipsoidModel> ellipsoid,
                         vsg::ref_ptr<vsg::Options> options)
{
    const vsg::dmat4 matrix = anchorMatrix(ellipsoid.get());
    const vsg::Path path = vsg::Path(RESOURCE_DIR) / singleModelPath;

    if (attachMode == InstanceAttachMode::NONE_INSTANCE)
    {
        // 故意不设 instanceNodeHint，走普通 VertexDraw / MatrixTransform。
        auto plainOptions = vsg::Options::create(*options);
        auto loaded = vsg::read(path, plainOptions).cast<vsg::Node>();
        if (!loaded)
        {
            std::cerr << "[WARN] skip plain node group, model load failed: " << singleModelPath << std::endl;
            return;
        }
        addNoneInstanceWithCullGroup(scene, matrix, loaded);
        std::cout << "[INFO] attached NONE_INSTANCE (tile CullNode + MatrixTransform x "
                  << instanceCount() << ", shared mesh)" << std::endl;
        return;
    }

    // 不改调用方的 Options，避免实体加载也被打开 instanceNodeHint。
    auto instanceOptions = vsg::Options::create(*options);
    instanceOptions->instanceNodeHint = vsg::Options::INSTANCE_TRANSLATIONS | vsg::Options::INSTANCE_ROTATIONS |
                                        vsg::Options::INSTANCE_SCALES;

    if (attachMode == InstanceAttachMode::SPLIT_WITH_LOD)
    {
        const LodMeshes meshes = loadLodMeshes(instanceOptions);
        if (!lodMeshesReady(meshes))
            return;
        addSplitInstanceWithLodGroup(scene, matrix, meshes);
        std::cout << "[INFO] attached instance SPLIT_WITH_LOD (tile LOD1–LOD6, frustum via LOD.bound)"
                  << std::endl;
        return;
    }

    auto loaded = vsg::read(path, instanceOptions).cast<vsg::Node>();
    if (!loaded)
    {
        std::cerr << "[WARN] skip instance group, model load failed: " << singleModelPath << std::endl;
        return;
    }
    addInstances(scene, matrix, loaded);
}
