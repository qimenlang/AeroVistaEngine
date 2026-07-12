#pragma once

#include <vsg/all.h>

#include <cstdint>

struct CompareOptions
{
    uint8_t pixelTolerance = 2;
    double maxDiffPercent = 0.1;
    uint8_t maxAbsDiffLimit = 32;
    double minPsnrDb = 40.0;
    bool writeDiffImage = true;
};

struct CompareResult
{
    bool dimensionMatch = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t differingPixels = 0;
    double differingPercent = 0.0;
    uint8_t maxAbsDiff = 0;
    double meanAbsDiff = 0.0;
    double psnrDb = 0.0;
    vsg::Path diffImagePath;

    bool passes(const CompareOptions& options) const
    {
        return dimensionMatch
            && differingPercent < options.maxDiffPercent
            && maxAbsDiff < options.maxAbsDiffLimit
            && psnrDb > options.minPsnrDb;
    }
};

CompareResult compareImages(const vsg::Path& golden,
                            const vsg::Path& actual,
                            const CompareOptions& options = {},
                            vsg::ref_ptr<vsg::Options> ioOptions = {});
