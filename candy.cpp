// candy.cpp - standalone Qt 5.9 / C++11 PDMAP step-cell renderer
// Experimental point-sampling preview, not an exact QPainter rasterization match.
// Original region controls display; expanded region selects candidate indices.
// Sampling cannot recover pixels outside the actual step-cell footprint.
// Each density belongs to a step-sized rectangle located at its window center.
#include <QImage>
#include <QRectF>
#include <QElapsedTimer>
#include <QDebug>
#include <QtGlobal>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstddef>

typedef int (*ColorIndexFunction)(double);

bool renderPdmapScanLine(
    QImage& image,
    const std::vector<double>& values,
    const QRectF& regionOrg,
    double xMin, double yMin,
    double windowX, double windowY,
    double stepX, double stepY,
    qint64 nx, qint64 ny,
    const QRgb colors[11],
    ColorIndexFunction getColorIndex)
{
    if (image.isNull() || image.format() != QImage::Format_ARGB32 ||
        regionOrg.isEmpty() || nx <= 0 || ny <= 0 ||
        windowX <= 0 || windowY <= 0 ||
        stepX <= 0 || stepY <= 0 || !colors || !getColorIndex)
        return false;

    const size_t sx = static_cast<size_t>(nx);
    const size_t sy = static_cast<size_t>(ny);
    if (sy > values.size() / sx)
        return false;

    QElapsedTimer timer;
    timer.start();

    const int width = image.width();
    const int height = image.height();
    // A step-sized rectangle is moved to each window's center.
    const double offsetX = (windowX - stepX) * 0.5;
    const double offsetY = (windowY - stepY) * 0.5;
    const double cellLeft = xMin + offsetX;
    const double cellBottom = yMin + offsetY;

    // Expand the requested image region only for density index selection.
    const QRectF region = regionOrg.adjusted(
        -offsetX, -offsetY, offsetX, offsetY);
    const QRectF densityRegion(
        xMin, yMin, (nx - 1) * stepX + windowX,
                    (ny - 1) * stepY + windowY);
    const QRectF intersection = region.intersected(densityRegion);
    if (intersection.isEmpty())
    {
        image.fill(Qt::transparent);
        return true;
    }

    const qint64 xIndexBegin = std::max<qint64>(0,
        static_cast<qint64>(std::floor(
            (intersection.left() - xMin) / stepX)));
    const qint64 yIndexBegin = std::max<qint64>(0,
        static_cast<qint64>(std::floor(
            (intersection.top() - yMin) / stepY)));
    const qint64 xIndexEnd = std::min<qint64>(nx,
        static_cast<qint64>(std::ceil(
            (intersection.right() - xMin) / stepX)) + 1);
    const qint64 yIndexEnd = std::min<qint64>(ny,
        static_cast<qint64>(std::ceil(
            (intersection.bottom() - yMin) / stepY)) + 1);

    // Image coordinates must always use the ORIGINAL region.
    const double scale = std::min(width / regionOrg.width(),
                                  height / regionOrg.height());
    const double pixelSize = 1.0 / scale;
    const double physicalLeft =
        regionOrg.center().x() - width * pixelSize * 0.5;
    const double physicalBottom =
        regionOrg.center().y() - height * pixelSize * 0.5;
    const double physicalTop =
        physicalBottom + height * pixelSize;

    image.fill(Qt::transparent);
    for (int py = 0; py < height; ++py)
    {
        const double physicalY =
            physicalTop - (py + 0.5) * pixelSize;
        const qint64 iy = static_cast<qint64>(
            std::floor((physicalY - cellBottom) / stepY));
        if (iy < yIndexBegin || iy >= yIndexEnd)
            continue;

        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(py));
        const size_t rowOffset = static_cast<size_t>(iy) * sx;

        for (int px = 0; px < width; ++px)
        {
            const double physicalX =
                physicalLeft + (px + 0.5) * pixelSize;
            const qint64 ix = static_cast<qint64>(
                std::floor((physicalX - cellLeft) / stepX));
            if (ix < xIndexBegin || ix >= xIndexEnd)
                continue;

            const double density =
                values[rowOffset + static_cast<size_t>(ix)];
            const int colorIndex = getColorIndex(density);
            if (colorIndex >= 0 && colorIndex < 11)
                line[px] = colors[colorIndex];
        }
    }

    qDebug() << "Render time:" << timer.elapsed() << "ms";
    return true;
}

// Usage:
// renderPdmapScanLine(image, values, regionOrg,
//     xMin, yMin, windowX, windowY, stepX, stepY,
//     nx, ny, colors, getColorIndex);
