#include <catch2/catch_test_macros.hpp>

#include "function/plants/PlantLodScan.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

    struct TempSpeciesDir
    {
        std::filesystem::path path;

        explicit TempSpeciesDir(const std::string& name) :
            path(std::filesystem::temp_directory_path() / "aerovista-plant-lod" / name)
        {
            std::filesystem::remove_all(path);
            std::filesystem::create_directories(path);
        }

        ~TempSpeciesDir() { std::filesystem::remove_all(path); }

        TempSpeciesDir(const TempSpeciesDir&) = delete;
        TempSpeciesDir& operator=(const TempSpeciesDir&) = delete;
    };

    void touch(const std::filesystem::path& file)
    {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream output(file);
        REQUIRE(output);
    }

    std::string filenameOf(const vsg::Path& path)
    {
        return std::filesystem::path(path.wstring()).filename().string();
    }

} // namespace

// =============================================================================
// 物种目录里的 _LOD{n}.gltf 按 n 从小到大收集，缺号保留，其它文件不收
// （lod分层实例.md §2、§8，码 PLANT-lod-scan）。
// LOD10 排在 LOD3 后面，用来确认按数字而不是按文件名排序。
// 子目录里的同名后缀不收：只看物种目录这一层。不读文件内容，空文件即可。
// =============================================================================
TEST_CASE("lod files in a species directory are kept in numeric order", "[unit][plants][PLANT-lod-scan]")
{
    TempSpeciesDir species("numeric-order");
    touch(species.path / "Oak_LOD10.gltf");
    touch(species.path / "Oak_LOD2.gltf");
    touch(species.path / "Oak_LOD3.gltf");
    touch(species.path / "Oak.gltf");
    touch(species.path / "Oak_LOD.gltf");
    touch(species.path / "Oak_LODx.gltf");
    touch(species.path / "Oak_LOD2.txt");
    touch(species.path / "nested" / "Oak_LOD1.gltf");

    auto lods = aerovista::collectPlantLodFiles(vsg::Path(species.path.wstring()));

    REQUIRE(lods.size() == 3);
    REQUIRE(lods[0].level == 2);
    REQUIRE(filenameOf(lods[0].path) == "Oak_LOD2.gltf");
    REQUIRE(lods[1].level == 3);
    REQUIRE(filenameOf(lods[1].path) == "Oak_LOD3.gltf");
    REQUIRE(lods[2].level == 10);
    REQUIRE(filenameOf(lods[2].path) == "Oak_LOD10.gltf");
}

// =============================================================================
// 目录里只有一份档时也收集，n 不必是 1
// （lod分层实例.md §2、§8，码 PLANT-lod-scan）。
// 「整片始终画它」是挂接时的行为，这里只验收这份文件被留下。
// =============================================================================
TEST_CASE("a single lod file is kept even when it is not lod 1", "[unit][plants][PLANT-lod-scan]")
{
    TempSpeciesDir species("single-file");
    touch(species.path / "Sapling_LOD4.gltf");

    auto lods = aerovista::collectPlantLodFiles(vsg::Path(species.path.wstring()));

    REQUIRE(lods.size() == 1);
    REQUIRE(lods[0].level == 4);
    REQUIRE(filenameOf(lods[0].path) == "Sapling_LOD4.gltf");
}

// =============================================================================
// 没有 _LOD{n}.gltf 时跳过该物种
// （lod分层实例.md §2、§8，码 PLANT-lod-scan）。
// 目录不存在和只有无关文件都是一份都没有。不验收日志原文，空结果就是跳过。
// =============================================================================
TEST_CASE("a species directory with no lod file yields nothing", "[unit][plants][PLANT-lod-scan]")
{
    TempSpeciesDir species("no-lod");
    touch(species.path / "readme.txt");
    touch(species.path / "tree.gltf");
    const auto missing = species.path / "missing";

    auto present = aerovista::collectPlantLodFiles(vsg::Path(species.path.wstring()));
    auto absent = aerovista::collectPlantLodFiles(vsg::Path(missing.wstring()));

    REQUIRE(present.empty());
    REQUIRE(absent.empty());
}

// =============================================================================
// 同一个 n 有两份文件时跳过整个物种，不返回其余档
// （lod分层实例.md §2、§8，码 PLANT-lod-scan）。
// LOD01 与 LOD1 是同一个数字。不验收日志原文。
// =============================================================================
TEST_CASE("two files with the same lod number yield nothing", "[unit][plants][PLANT-lod-scan]")
{
    TempSpeciesDir species("duplicate-level");
    touch(species.path / "Alder_LOD1.gltf");
    touch(species.path / "Birch_LOD01.gltf");
    touch(species.path / "Alder_LOD2.gltf");

    auto lods = aerovista::collectPlantLodFiles(vsg::Path(species.path.wstring()));

    REQUIRE(lods.empty());
}
