#pragma once

#include <vsg/io/Path.h>

#include <vector>

namespace aerovista
{

    struct PlantLodFile
    {
        int level = 0;
        vsg::Path path;
    };

    // 只按文件名收集。读网格、算半径、挂上 LOD 都不在这里。空结果表示跳过该物种。
    std::vector<PlantLodFile> collectPlantLodFiles(const vsg::Path& speciesDirectory);

} // namespace aerovista
