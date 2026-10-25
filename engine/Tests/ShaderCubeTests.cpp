#include <catch2/catch_test_macros.hpp>

#include "Testutil.h"
#include "function/scene/ShaderCube.h"

#ifndef RESOURCE_DIR
#    define RESOURCE_DIR "."
#endif

namespace
{
    bool sceneIsShaderCube(vsg::ref_ptr<vsg::Node> scene)
    {
        if (!scene)
            return false;
        bool marked = false;
        return scene->getValue(kShaderCubeObjectKey, marked) && marked;
    }

    vsg::ref_ptr<vsg::Options> makeOptions()
    {
        auto options = vsg::Options::create();
        return options;
    }
} // namespace

// =============================================================================
// 随仓 debug_cube.frag 能编成带立方体标记的场景节点（引擎加载与渲染.md §2，码 RND-shader-cube-glsl）。
// =============================================================================
TEST_CASE("ShaderCompiler compiles debug_cube.frag GLSL", "[unit][shader][RND-shader-cube-glsl]")
{
    const vsg::Path fragmentPath = vsg::Path(RESOURCE_DIR) / "shaders" / "debug_cube.frag";
    auto cube = createShaderCube(fragmentPath, makeOptions());
    REQUIRE(cube);
    REQUIRE(sceneIsShaderCube(cube));
}

// =============================================================================
// 片段着色器文本不是合法 GLSL 时，createShaderCube 返回空（引擎加载与渲染.md §2，码 RND-shader-cube-glsl）。
// 与上一条同一码：这一条钉拒绝，不钉错误原文。
// =============================================================================
TEST_CASE("createShaderCube rejects invalid GLSL", "[unit][shader][RND-shader-cube-glsl]")
{
    const TempConfigFile bad(R"(not a fragment shader)");
    auto cube = createShaderCube(vsg::Path(bad.path()), makeOptions());
    REQUIRE_FALSE(cube);
}
