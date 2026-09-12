add_subdirectory(vsg)
add_subdirectory(vsgXchange)
#add_subdirectory(vsgImGui)
add_subdirectory(cigi)
if(AEROVISTA_BUILD_VSG_EXAMPLES)
    add_subdirectory(vsgExamples)
endif()

# header-only；作子工程时默认不编其测试。供 AeroVistaConfig / 引擎 / sync 配置解析共用。
set(JSON_BuildTests OFF CACHE BOOL "nlohmann_json: skip upstream tests" FORCE)
set(JSON_Install OFF CACHE BOOL "nlohmann_json: skip install from in-tree build" FORCE)
set(JSON_SystemInclude ON CACHE BOOL "nlohmann_json: system includes (skip clang-tidy)" FORCE)
add_subdirectory(nlohmannJson)
add_subdirectory(config)

add_subdirectory(sync)
add_subdirectory(Catch2)