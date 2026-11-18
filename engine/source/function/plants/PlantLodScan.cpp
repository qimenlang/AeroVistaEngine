#include "function/plants/PlantLodScan.h"

#include <vsg/io/FileSystem.h>

#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace
{

    std::vector<aerovista::PlantLodFile> skipSpecies(const vsg::Path& directory, const char* reason)
    {
        std::cerr << "[WARN] skip plant LOD group, " << reason << ": " << directory.string() << std::endl;
        return {};
    }

    // 后缀必须是「_LOD」加一段数字再加「.gltf」。数字按整数值读，LOD01 与 LOD1 是同一档。
    std::optional<int> lodLevelOf(const vsg::Path& filename)
    {
        const std::string name = filename.string();
        constexpr std::string_view marker = "_LOD";
        constexpr std::string_view extension = ".gltf";
        if (name.size() <= marker.size() + extension.size() || !name.ends_with(extension))
            return std::nullopt;

        const auto digitsEnd = name.size() - extension.size();
        auto digitsBegin = digitsEnd;
        while (digitsBegin > 0 && name[digitsBegin - 1] >= '0' && name[digitsBegin - 1] <= '9')
            --digitsBegin;
        if (digitsBegin == digitsEnd || digitsBegin < marker.size())
            return std::nullopt;
        if (name.compare(digitsBegin - marker.size(), marker.size(), marker) != 0)
            return std::nullopt;

        int level = 0;
        for (auto index = digitsBegin; index < digitsEnd; ++index)
        {
            const int digit = name[index] - '0';
            if (level > (std::numeric_limits<int>::max() - digit) / 10)
                return std::nullopt;
            level = level * 10 + digit;
        }
        return level;
    }

    // nullopt：同一个 n 出现两份，整组都不要。空 map：这一层没有可挂的档。
    std::optional<std::map<int, vsg::Path>> lodFilesIn(const vsg::Path& directory)
    {
        std::map<int, vsg::Path> files;
        for (const auto& name : vsg::getDirectoryContents(directory))
        {
            const auto path = directory / name;
            if (vsg::fileType(path) != vsg::REGULAR_FILE)
                continue;
            const auto level = lodLevelOf(name);
            if (!level)
                continue;
            if (files.contains(*level))
                return std::nullopt;
            files.emplace(*level, path);
        }
        return files;
    }

    std::vector<aerovista::PlantLodFile> toLodFiles(const std::map<int, vsg::Path>& files)
    {
        std::vector<aerovista::PlantLodFile> lods;
        lods.reserve(files.size());
        for (const auto& entry : files)
            lods.push_back({entry.first, entry.second});
        return lods;
    }

} // namespace

namespace aerovista
{

    std::vector<PlantLodFile> collectPlantLodFiles(const vsg::Path& speciesDirectory)
    {
        if (vsg::fileType(speciesDirectory) != vsg::DIRECTORY)
            return skipSpecies(speciesDirectory, "species directory unavailable");

        auto files = lodFilesIn(speciesDirectory);
        if (!files)
            return skipSpecies(speciesDirectory, "duplicate LOD number");
        if (files->empty())
            return skipSpecies(speciesDirectory, "no LOD files");
        return toLodFiles(*files);
    }

} // namespace aerovista
