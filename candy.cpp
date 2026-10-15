// candy.cpp
// Qt 5.9 / C++11. Integration-only AverageColorIndex scanline renderer.
// No QTransform, main(), synthetic density or synthetic color table.
// Physical Y increases upward; image Y increases downward.
// The current viewport region is expressed in PHYSICAL coordinates.
// Assumes densityAt(x,y) addresses matrix rows starting at physical originY.
// Palette index is area-weighted (not raw density).
#include <QImage>
#include <QElapsedTimer>
#include <QDebug>
#include <QRgb>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

struct DensityRegion {
    double xmin, ymin, xmax, ymax;
};
struct AverageScanlineInput {
    size_t nx, ny;          // matrix dimensions (NOT image dimensions)
    double originX, originY;
    double stepX, stepY;
    DensityRegion region;   // current viewport in physical coordinates
    int imageWidth, imageHeight; // QImage requires int
};

// Half-open intersection of [lo, hi) with the matrix index domain [0, count).
// No conversion to an integer until the value has been clipped to [0,count].
// Returns begin/end in [0,count], where end is EXCLUSIVE.
static bool clippedCellRange(double lo, double hi, size_t count,
                             size_t &begin, size_t &end)
{
    begin = end = 0;
    if (count == 0 || !std::isfinite(lo) || !std::isfinite(hi) ||
        !(hi > lo)) return false;

    const double n = static_cast<double>(count);
    if (hi <= 0.0 || lo >= n) return false;

    // Compute in double; clamp BEFORE integer conversion.
    const double a = std::max(0.0, std::min(n, std::floor(lo)));
    const double b = std::max(0.0, std::min(n, std::ceil(hi)));
    // Avoid double->size_t conversion at count when count is too large
    // to be exactly representable by double.
    if (count > (size_t(1) << 53)) return false;
    begin = static_cast<size_t>(a);
    end = static_cast<size_t>(b);
    return begin < end;
}

// densityAt(size_t x, size_t y) -> double
// densityToIndex(double density) -> int in [0,colorCount), or -1
// colorAt(int index) -> QRgb
// Note: caller MUST guarantee densityAt is valid for [0,nx)*[0,ny).
// For flat arrays, validate actual allocated length against nx*ny.
template <typename DensityAt, typename DensityToIndex, typename ColorAt>
bool renderAverageScanLine(const AverageScanlineInput &in,
                           int colorCount,
                           DensityAt densityAt,
                           DensityToIndex densityToIndex,
                           ColorAt colorAt,
                           QImage &output)
{
    if (in.nx == 0 || in.ny == 0 ||
        in.imageWidth <= 0 || in.imageHeight <= 0 || colorCount <= 0 ||
        !std::isfinite(in.originX) || !std::isfinite(in.originY) ||
        !std::isfinite(in.stepX) || !std::isfinite(in.stepY) ||
        !(in.stepX > 0.0) || !(in.stepY > 0.0) ||
        !std::isfinite(in.region.xmin) || !std::isfinite(in.region.xmax) ||
        !std::isfinite(in.region.ymin) || !std::isfinite(in.region.ymax) ||
        !(in.region.xmax > in.region.xmin) ||
        !(in.region.ymax > in.region.ymin))
        return false;

    // Prevent multiplication overflow in flat matrix indexing.
    if (in.nx > std::numeric_limits<size_t>::max() / in.ny)
        return false;
    // Index range conversion uses double; exact integer representation required.
    const size_t maxExactDoubleInteger = size_t(1) << 53;
    if (in.nx > maxExactDoubleInteger || in.ny > maxExactDoubleInteger)
        return false;

    const double pixelW =
        (in.region.xmax - in.region.xmin) / in.imageWidth;
    const double pixelH =
        (in.region.ymax - in.region.ymin) / in.imageHeight;
    if (!std::isfinite(pixelW) || !std::isfinite(pixelH) ||
        !(pixelW > 0.0) || !(pixelH > 0.0)) return false;

    QImage image(in.imageWidth, in.imageHeight, QImage::Format_ARGB32);
    if (image.isNull()) return false;
    image.fill(Qt::transparent);

    QElapsedTimer progress, total;
    progress.start();
    total.start();

    for (int py = 0; py < in.imageHeight; ++py) {
        QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(py));
        if (!dst) return false;

        const double physY0 = in.region.ymax - (double(py) + 1.0) * pixelH;
        const double physY1 = in.region.ymax - double(py) * pixelH;
        const double my0 = (physY0 - in.originY) / in.stepY;
        const double my1 = (physY1 - in.originY) / in.stepY;
        size_t yBegin = 0, yEnd = 0;
        if (!clippedCellRange(my0, my1, in.ny, yBegin, yEnd))
            continue;

        for (int px = 0; px < in.imageWidth; ++px) {
            const double physX0 = in.region.xmin + double(px) * pixelW;
            const double physX1 = in.region.xmin + (double(px) + 1.0) * pixelW;
            const double mx0 = (physX0 - in.originX) / in.stepX;
            const double mx1 = (physX1 - in.originX) / in.stepX;
            size_t xBegin = 0, xEnd = 0;
            if (!clippedCellRange(mx0, mx1, in.nx, xBegin, xEnd))
                continue;

            double weighted = 0.0;
            double totalWeight = 0.0;
            for (size_t iy = yBegin; iy < yEnd; ++iy) {
                const double overlapY = std::max(0.0,
                    std::min(my1, double(iy) + 1.0) -
                    std::max(my0, double(iy)));
                if (!(overlapY > 0.0)) continue;

                for (size_t ix = xBegin; ix < xEnd; ++ix) {
                    const double overlapX = std::max(0.0,
                        std::min(mx1, double(ix) + 1.0) -
                        std::max(mx0, double(ix)));
                    const double area = overlapX * overlapY;
                    if (!(area > 0.0)) continue;

                    const double density = densityAt(ix, iy);
                    if (!std::isfinite(density)) continue;
                    const int index = densityToIndex(density);
                    if (index < 0 || index >= colorCount) continue;
                    weighted += double(index) * area;
                    totalWeight += area;
                }
            }
            if (!(totalWeight > 0.0)) continue;
            const double average = weighted / totalWeight;
            if (!std::isfinite(average)) continue;
            const int index = std::max(0, std::min(colorCount - 1,
                int(std::floor(average + 0.5))));
            dst[px] = colorAt(index);
        }
        if (progress.elapsed() >= 1000) {
            qDebug() << "AverageScanLine" << (100.0 * (py + 1) / in.imageHeight)
                     << "% elapsed(ms):" << total.elapsed();
            progress.restart();
        }
    }
    output = image;
    qDebug() << "AverageScanLine total(ms):" << total.elapsed();
    return true;
}
