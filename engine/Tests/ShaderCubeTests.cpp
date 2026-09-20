#include <catch2/catch_test_macros.hpp>

#include "Common.h"
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

TEST_CASE("ShaderCompiler compiles debug_cube.frag GLSL", "[unit][shader][RND-shader-cube-glsl]")
{
    const vsg::Path fragmentPath = vsg::Path(RESOURCE_DIR) / "shaders" / "debug_cube.frag";
    auto cube = createShaderCube(fragmentPath, makeOptions());
    REQUIRE(cube);
    REQUIRE(sceneIsShaderCube(cube));
}

TEST_CASE("createShaderCube rejects invalid GLSL", "[unit][shader][RND-shader-cube-glsl]")
{
    const TempConfigFile bad(R"(not a fragment shader)");
    auto cube = createShaderCube(vsg::Path(bad.path()), makeOptions());
    REQUIRE_FALSE(cube);
}
