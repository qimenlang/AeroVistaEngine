#include <catch2/catch_test_macros.hpp>

#include "Common.h"
#include "engine.h"
#include "function/scene/ShaderCube.h"

#include <fstream>
#include <string>

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

SCENARIO("shader cube scene compiles GLSL and renders one frame",
         "[acceptance][bdd][render][shader][RND-shader-cube]")
{
    GIVEN("an offscreen Engine and scene_shader_cube.json")
    {
        Engine engine;
        engine.extent = {640, 480};
        engine.showWindow = false;
        const std::string configPath = std::string(RESOURCE_DIR) + "/config/scene_shader_cube.json";

        WHEN("the shader cube config is loaded and one frame is rendered")
        {
            REQUIRE(engine.loadConfig(configPath));
            const bool loaded = engine.init();
            const bool rendered = loaded && engine.tickOnFrame();

            THEN("GLSL compile succeeds, the cube is in the scene, and a frame is produced")
            {
                REQUIRE(loaded);
                REQUIRE(rendered);
                REQUIRE(sceneIsShaderCube(engine.mainScene()));
            }
        }
    }
}
