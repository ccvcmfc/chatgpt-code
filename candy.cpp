// candy.cpp - standalone Qt 5.9 / C++11 PDMAP step-cell renderer
// Experimental point-sampling preview, not an exact QPainter rasterization match.
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
    const double scale = std::min(width / regionOrg.width(),
                                  height / regionOrg.height());
    const double pixelSize = 1.0 / scale;
    const double physicalLeft =
        regionOrg.center().x() - width * pixelSize * 0.5;
    const double physicalBottom =
        regionOrg.center().y() - height * pixelSize * 0.5;
    const double physicalTop =
        physicalBottom + height * pixelSize;

    // Window lower-left is xMin/yMin. The cell is at the window center.
    const double cellLeft = xMin + windowX * 0.5;
    const double cellBottom = yMin + windowY * 0.5;

    image.fill(Qt::transparent);
    for (int py = 0; py < height; ++py)
    {
        const double physicalY =
            physicalTop - (py + 0.5) * pixelSize;
        const qint64 iy = static_cast<qint64>(
            std::floor((physicalY - cellBottom) / stepY));
        if (iy < 0 || iy >= ny)
            continue;

        QRgb* line = reinterpret_cast<QRgb*>(image.scanLine(py));
        const size_t rowOffset = static_cast<size_t>(iy) * sx;

        for (int px = 0; px < width; ++px)
        {
            const double physicalX =
                physicalLeft + (px + 0.5) * pixelSize;
            const qint64 ix = static_cast<qint64>(
                std::floor((physicalX - cellLeft) / stepX));
            if (ix < 0 || ix >= nx)
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
