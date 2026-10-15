// candy.cpp
// Qt 5.9 / C++11, no QTransform.
// Independent area-weighted average COLOR INDEX renderer.
// Physical coordinate: X increases right, Y increases UP.
// QImage coordinate: X increases right, Y increases DOWN.
// Current viewport is expressed by its physical min/max region.
// Integration-only source: no main(), no synthetic input, no demo palette.
// This averages palette indices (matching prior experiment), not raw densities.

#include <QCoreApplication>
#include <QImage>
#include <QElapsedTimer>
#include <QDebug>
#include <QRgb>
#include <algorithm>
#include <cmath>

struct DensityRegion {
    double xmin, ymin, xmax, ymax;
};

struct AverageScanlineInput {
    int nx, ny;
    double originX, originY; // physical min coordinates of matrix
    double stepX, stepY;     // > 0
    DensityRegion region;    // CURRENT viewport in physical coordinates
    int imageWidth, imageHeight;
};

static int clampIndex(long long v, int n) {
    return int(std::max(0LL, std::min(v, static_cast<long long>(n) - 1)));
}

// DensityAt(ix,iy): raw density; DensityToIndex(density): palette index or -1;
// ColorAt(index): QRgb. Caller owns matrix and palette storage.
// The physical Y-to-image mapping is:
//    physicalY = region.ymax - (py + fraction) * pixelHeight
// No full-matrix Y flip is applied to the viewport coordinates.
template <typename DensityAt, typename DensityToIndex, typename ColorAt>
bool renderAverageScanLine(const AverageScanlineInput &in,
                           int colorCount,
                           DensityAt densityAt,
                           DensityToIndex densityToIndex,
                           ColorAt colorAt,
                           QImage &output)
{
    if (in.nx <= 0 || in.ny <= 0 ||
        in.imageWidth <= 0 || in.imageHeight <= 0 ||
        colorCount <= 0 || !(in.stepX > 0.0) || !(in.stepY > 0.0) ||
        !(in.region.xmax > in.region.xmin) ||
        !(in.region.ymax > in.region.ymin))
        return false;

    QImage image(in.imageWidth, in.imageHeight, QImage::Format_ARGB32);
    if (image.isNull()) return false;
    image.fill(Qt::transparent);

    const double pixelW = (in.region.xmax - in.region.xmin) / in.imageWidth;
    const double pixelH = (in.region.ymax - in.region.ymin) / in.imageHeight;
    QElapsedTimer total, progress;
    total.start();
    progress.start();

    for (int py = 0; py < in.imageHeight; ++py) {
        QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(py));

        // Upper image row corresponds to HIGH physical Y.
        const double physY0 = in.region.ymax - (py + 1.0) * pixelH;
        const double physY1 = in.region.ymax - py * pixelH;
        const double my0 = (physY0 - in.originY) / in.stepY;
        const double my1 = (physY1 - in.originY) / in.stepY;

        if (my1 <= 0.0 || my0 >= in.ny) continue;
        const int yBegin = clampIndex(
            static_cast<long long>(std::floor(my0)), in.ny);
        const int yEnd = clampIndex(
            static_cast<long long>(std::ceil(my1)) - 1, in.ny);

        for (int px = 0; px < in.imageWidth; ++px) {
            const double physX0 = in.region.xmin + px * pixelW;
            const double physX1 = in.region.xmin + (px + 1.0) * pixelW;
            const double mx0 = (physX0 - in.originX) / in.stepX;
            const double mx1 = (physX1 - in.originX) / in.stepX;

            if (mx1 <= 0.0 || mx0 >= in.nx) continue;
            const int xBegin = clampIndex(
                static_cast<long long>(std::floor(mx0)), in.nx);
            const int xEnd = clampIndex(
                static_cast<long long>(std::ceil(mx1)) - 1, in.nx);

            double sum = 0.0;
            double weightSum = 0.0;
            for (int iy = yBegin; iy <= yEnd; ++iy) {
                const double overlapY = std::max(0.0,
                    std::min(my1, double(iy + 1)) -
                    std::max(my0, double(iy)));
                if (overlapY <= 0.0) continue;

                for (int ix = xBegin; ix <= xEnd; ++ix) {
                    const double overlapX = std::max(0.0,
                        std::min(mx1, double(ix + 1)) -
                        std::max(mx0, double(ix)));
                    const double area = overlapX * overlapY;
                    if (area <= 0.0) continue;
                    const double density = densityAt(ix, iy);
                    if (!std::isfinite(density)) continue;
                    const int idx = densityToIndex(density);
                    if (idx < 0 || idx >= colorCount) continue;
                    sum += double(idx) * area;
                    weightSum += area;
                }
            }
            if (weightSum <= 0.0) continue;
            const int index = std::max(0, std::min(colorCount - 1,
                int(std::floor(sum / weightSum + 0.5))));
            dst[px] = colorAt(index);
        }

        if (progress.elapsed() >= 1000) {
            qDebug() << "AverageScanLine:"
                     << 100.0 * (py + 1) / in.imageHeight << "%"
                     << "elapsed(ms):" << total.elapsed();
            progress.restart();
        }
    }

    output = image;
    qDebug() << "AverageScanLine completed in" << total.elapsed() << "ms";
    return true;
}

