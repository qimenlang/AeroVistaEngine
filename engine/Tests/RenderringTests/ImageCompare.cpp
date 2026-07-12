#include "ImageCompare.h"

#ifdef vsgXchange_FOUND
#    include <vsgXchange/all.h>
#endif

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{

vsg::ref_ptr<vsg::Options> createIoOptions()
{
    auto options = vsg::Options::create();
    options->fileCache = vsg::getEnv("VSG_FILE_CACHE");
    options->paths = vsg::getEnvPaths("VSG_FILE_PATH");
#ifdef vsgXchange_all
    options->add(vsgXchange::all::create());
#endif
    return options;
}

vsg::ref_ptr<vsg::ubvec4Array2D> loadRGBA8(const vsg::Path& path, vsg::ref_ptr<vsg::Options> options)
{
    if (!options) options = createIoOptions();
    return vsg::read_cast<vsg::ubvec4Array2D>(path, options);
}

void diffPixel(const vsg::ubvec4& golden,
               const vsg::ubvec4& actual,
               uint8_t& outMax,
               int& outSumAbs,
               bool& outDiffers,
               uint8_t tolerance)
{
    int maxDiff = 0;
    int sumAbs = 0;

    for (int channel = 0; channel < 3; ++channel)
    {
        const int diff = std::abs(int(golden[channel]) - int(actual[channel]));
        maxDiff = std::max(maxDiff, diff);
        sumAbs += diff;
    }

    outMax = static_cast<uint8_t>(maxDiff);
    outSumAbs = sumAbs;
    outDiffers = maxDiff > tolerance;
}

} // namespace

CompareResult compareImages(const vsg::Path& golden,
                            const vsg::Path& actual,
                            const CompareOptions& options,
                            vsg::ref_ptr<vsg::Options> ioOptions)
{
    CompareResult result;

    if (!ioOptions) ioOptions = createIoOptions();

    const auto goldenImage = loadRGBA8(golden, ioOptions);
    const auto actualImage = loadRGBA8(actual, ioOptions);

    if (!goldenImage || !actualImage)
    {
        return result;
    }

    if (goldenImage->width() != actualImage->width() || goldenImage->height() != actualImage->height())
    {
        return result;
    }

    result.dimensionMatch = true;
    result.width = goldenImage->width();
    result.height = goldenImage->height();

    const auto pixelCount = static_cast<size_t>(result.width) * result.height;
    uint64_t sumAbs = 0;
    uint64_t sumSq = 0;

    vsg::ref_ptr<vsg::ubvec4Array2D> diffImage;
    if (options.writeDiffImage)
    {
        diffImage = vsg::ubvec4Array2D::create(result.width, result.height, vsg::Data::Properties{VK_FORMAT_R8G8B8A8_UNORM});
    }

    for (size_t i = 0; i < pixelCount; ++i)
    {
        const auto& goldenPixel = (*goldenImage)[i];
        const auto& actualPixel = (*actualImage)[i];

        uint8_t maxDiff = 0;
        int sumAbsPixel = 0;
        bool differs = false;
        diffPixel(goldenPixel, actualPixel, maxDiff, sumAbsPixel, differs, options.pixelTolerance);

        if (differs) ++result.differingPixels;
        result.maxAbsDiff = std::max(result.maxAbsDiff, maxDiff);
        sumAbs += static_cast<uint64_t>(sumAbsPixel);

        for (int channel = 0; channel < 3; ++channel)
        {
            const int diff = int(goldenPixel[channel]) - int(actualPixel[channel]);
            sumSq += static_cast<uint64_t>(diff * diff);
        }

        if (diffImage)
        {
            auto& diffPixelOut = (*diffImage)[i];
            if (differs)
            {
                diffPixelOut.set(std::min(255, int(maxDiff) * 4), 0, 0, 255);
            }
            else
            {
                const uint8_t gray = static_cast<uint8_t>((actualPixel[0] + actualPixel[1] + actualPixel[2]) / 6);
                diffPixelOut.set(gray, gray, gray, 255);
            }
        }
    }

    const double channelCount = double(pixelCount) * 3.0;
    result.meanAbsDiff = double(sumAbs) / channelCount;
    result.differingPercent = 100.0 * double(result.differingPixels) / double(pixelCount);

    const double mse = double(sumSq) / channelCount;
    result.psnrDb = (mse == 0.0) ? std::numeric_limits<double>::infinity()
                                 : 10.0 * std::log10(255.0 * 255.0 / mse);

    if (diffImage)
    {
        const std::string actualStr = actual.string();
        const auto slashPos = actualStr.find_last_of("/\\");
        const auto dotPos = actualStr.rfind('.');
        const std::string stem = (dotPos != std::string::npos && dotPos > slashPos)
            ? actualStr.substr(slashPos + 1, dotPos - slashPos - 1)
            : actualStr.substr(slashPos + 1);
        const vsg::Path parent = (slashPos != std::string::npos)
            ? vsg::Path(actualStr.substr(0, slashPos))
            : vsg::Path(".");

        result.diffImagePath = parent / (stem + "_diff.png");
        vsg::write(diffImage, result.diffImagePath, ioOptions);
    }

    return result;
}
