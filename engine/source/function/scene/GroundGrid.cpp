#include "function/scene/GroundGrid.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr int kMaxCells = 400;
    constexpr int kMaxImageDim = 2048;
    constexpr float kQuadBelowOriginM = -0.25f;

    vsg::ubvec4 kGridBackground{20, 24, 32, 255};
    vsg::ubvec4 kMinorLine{220, 196, 48, 255};
    vsg::ubvec4 kMajorLine{255, 255, 255, 255};

    int cellCountAcross(double halfExtentM, double cellM)
    {
        return static_cast<int>(std::lround((2.0 * halfExtentM) / cellM));
    }

    void fillRgba(vsg::ubvec4Array2D& image, const vsg::ubvec4& color)
    {
        const uint32_t width = image.width();
        const uint32_t height = image.height();
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
                image.set(x, y, color);
        }
    }

    void stampVertical(vsg::ubvec4Array2D& image, int x, int halfWidth, const vsg::ubvec4& color)
    {
        const int width = static_cast<int>(image.width());
        const int height = static_cast<int>(image.height());
        const int x0 = std::max(0, x - halfWidth);
        const int x1 = std::min(width - 1, x + halfWidth);
        for (int pixelX = x0; pixelX <= x1; ++pixelX)
        {
            for (int y = 0; y < height; ++y)
                image.set(static_cast<uint32_t>(pixelX), static_cast<uint32_t>(y), color);
        }
    }

    void stampHorizontal(vsg::ubvec4Array2D& image, int y, int halfWidth, const vsg::ubvec4& color)
    {
        const int width = static_cast<int>(image.width());
        const int height = static_cast<int>(image.height());
        const int y0 = std::max(0, y - halfWidth);
        const int y1 = std::min(height - 1, y + halfWidth);
        for (int pixelY = y0; pixelY <= y1; ++pixelY)
        {
            for (int x = 0; x < width; ++x)
                image.set(static_cast<uint32_t>(x), static_cast<uint32_t>(pixelY), color);
        }
    }

    void paintGridLines(vsg::ubvec4Array2D& image, int cells, int pixelsPerCell, int majorEvery)
    {
        for (int i = 0; i <= cells; ++i)
        {
            const bool isMajor = (i % majorEvery) == 0;
            const vsg::ubvec4& color = isMajor ? kMajorLine : kMinorLine;
            const int halfWidth = isMajor ? 1 : 0;
            const int pixel = i * pixelsPerCell;
            stampVertical(image, pixel, halfWidth, color);
            stampHorizontal(image, pixel, halfWidth, color);
        }
    }

    vsg::ref_ptr<vsg::ubvec4Array2D> createGridImage(const GroundGridConfig& spec)
    {
        const int cells = std::clamp(cellCountAcross(spec.halfExtentM, spec.cellM), 1, kMaxCells);
        const int pixelsPerCell = std::max(1, kMaxImageDim / cells);
        const uint32_t dim = static_cast<uint32_t>(cells * pixelsPerCell + 1);
        vsg::Data::Properties props{VK_FORMAT_R8G8B8A8_UNORM};
        props.mipLevels = 1;
        auto image = vsg::ubvec4Array2D::create(dim, dim, props);
        fillRgba(*image, kGridBackground);
        paintGridLines(*image, cells, pixelsPerCell, spec.majorEvery);
        return image;
    }
} // namespace

int groundGridLineCount(double halfExtentM, double cellM)
{
    return cellCountAcross(halfExtentM, cellM) + 1;
}

vsg::ref_ptr<vsg::Node> createEnuGroundGrid(const GroundGridConfig& spec)
{
    auto builder = vsg::Builder::create();
    vsg::StateInfo state;
    state.image = createGridImage(spec);
    state.lighting = false;
    state.two_sided = true;

    vsg::GeometryInfo geom;
    const auto extent = static_cast<float>(2.0 * spec.halfExtentM);
    geom.position.set(0.0f, 0.0f, kQuadBelowOriginM);
    geom.dx.set(extent, 0.0f, 0.0f);
    geom.dy.set(0.0f, extent, 0.0f);
    geom.dz.set(0.0f, 0.0f, 1.0f);
    return builder->createQuad(geom, state);
}

vsg::ref_ptr<vsg::MatrixTransform> createPinnedGroundGrid(const vsg::EllipsoidModel& ellipsoid,
                                                          const GroundGridConfig& spec)
{
    const vsg::dvec3 lla{spec.lla.x, spec.lla.y, spec.lla.z};
    auto transform = vsg::MatrixTransform::create(ellipsoid.computeLocalToWorldTransform(lla));
    transform->setValue(kGroundGridObjectKey, true);
    transform->addChild(createEnuGroundGrid(spec));
    return transform;
}
