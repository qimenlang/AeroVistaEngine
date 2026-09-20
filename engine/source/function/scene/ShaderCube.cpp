#include "function/scene/ShaderCube.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

namespace
{
    std::optional<std::string> readTextFile(const vsg::Path& path)
    {
        std::ifstream in(std::filesystem::path(path.string()));
        if (!in)
            return std::nullopt;
        std::ostringstream oss;
        oss << in.rdbuf();
        std::string text = oss.str();
        // Windows 编辑器常写 UTF-8 BOM；glslang 会把 BOM 当成非法 token。
        if (text.size() >= 3 &&
            static_cast<unsigned char>(text[0]) == 0xEF &&
            static_cast<unsigned char>(text[1]) == 0xBB &&
            static_cast<unsigned char>(text[2]) == 0xBF)
        {
            text.erase(0, 3);
        }
        return text;
    }

    vsg::ref_ptr<vsg::ShaderStage> loadCompiledFragment(const vsg::Path& fragmentPath,
                                                        vsg::ref_ptr<vsg::Options> options)
    {
        const auto source = readTextFile(fragmentPath);
        if (!source)
        {
            std::cerr << "[shaderCube] failed to read " << fragmentPath << std::endl;
            return {};
        }

        auto compiler = vsg::ShaderCompiler::create();
        if (!compiler->supported())
        {
            std::cerr << "[shaderCube] VSG_SUPPORTS_ShaderCompiler is 0; glslang wrap missing" << std::endl;
            return {};
        }

        auto fragment = vsg::ShaderStage::create(VK_SHADER_STAGE_FRAGMENT_BIT, "main", *source);
        if (!compiler->compile(fragment, {}, options))
        {
            std::cerr << "[shaderCube] GLSL compile failed: " << fragmentPath << std::endl;
            return {};
        }
        return fragment;
    }

    void replaceFragmentStage(vsg::ShaderSet& shaderSet, vsg::ref_ptr<vsg::ShaderStage> fragment)
    {
        for (auto& stage : shaderSet.stages)
        {
            if (stage->stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                stage = fragment;
        }
    }
} // namespace

vsg::ref_ptr<vsg::Node> createShaderCube(const vsg::Path& fragmentPath, vsg::ref_ptr<vsg::Options> options)
{
    auto fragment = loadCompiledFragment(fragmentPath, options);
    if (!fragment)
        return {};

    auto shaderSet = vsg::createFlatShadedShaderSet(options);
    replaceFragmentStage(*shaderSet, fragment);

    auto builder = vsg::Builder::create();
    builder->options = options;
    builder->shaderSet = shaderSet;

    vsg::GeometryInfo geom;
    vsg::StateInfo state;
    state.lighting = false;
    state.two_sided = true;

    auto cube = builder->createBox(geom, state);
    if (!cube)
        return {};

    auto root = vsg::Group::create();
    root->setValue(kShaderCubeObjectKey, true);
    root->addChild(cube);
    return root;
}
