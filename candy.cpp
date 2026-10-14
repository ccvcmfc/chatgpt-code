// candy.cpp
// PDMAP Scanline sampling alternatives (Qt 5.9 / C++11).
// Integration snippet: insert into existing rendering function.
// Required existing variables:
//   QImage image (Format_ARGB32), std::vector<double> values,
//   int nx, ny; double stepX, stepY, startX, startY;
//   QTransform painterTransform; QRgb colors[11];
//   int getDensityColorIndex(double density);
// startX/startY = first stepRect's left/top after moveCenter(windowRect.center()).
// IMPORTANT: Use the EXACT transform used by the existing QPainter renderer.
// This snippet assumes an opaque 11-color palette. No rotations/reflections needed.
//
// SampleMode::LastCell is only a hypothesis for approximating drawRects().
// Majority4x4 is a distribution-oriented alternative, NOT pixel-exact QPainter.
// Run each mode against its OWN dataset's drawRects() reference image.

#include <QImage>
#include <QTransform>
#include <QPointF>
#include <QElapsedTimer>
#include <QDebug>
#include <QRgb>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

enum class SampleMode
{
    Center,
    LastCell,
    Majority4x4
};

// Paste this function into the existing renderer and connect the actual
// getDensityColorIndex() function through the callback.
// Returns false if the inputs cannot be rendered safely.
template <typename ColorIndexFn>
bool renderPdmapSampling(
    QImage &image,
    const std::vector<double> &values,
    int nx, int ny,
    double stepX, double stepY,
    double startX, double startY,
    const QTransform &painterTransform,
    const QRgb (&colors)[11],
    ColorIndexFn getDensityColorIndex,
    SampleMode mode)
{
    if (image.format() != QImage::Format_ARGB32 ||
        image.width() <= 0 || image.height() <= 0 ||
        nx <= 0 || ny <= 0 ||
        !(stepX > 0.0) || !(stepY > 0.0) ||
        !std::isfinite(startX) || !std::isfinite(startY))
        return false;

    const size_t sx = static_cast<size_t>(nx);
    const size_t sy = static_cast<size_t>(ny);
    if (sx > values.size() / sy)
        return false;

    bool invertible = false;
    const QTransform inverseTransform =
        painterTransform.inverted(&invertible);
    if (!invertible)
        return false;

    auto sampleColor = [&](int px, int py,
                           double offsetX, double offsetY) -> int
    {
        const QPointF world = inverseTransform.map(
            QPointF(px + offsetX, py + offsetY));

        // Clamp also reproduces border extension ONLY when that is how
        // the original renderer treats out-of-matrix coordinates.
        const double fx = std::floor((world.x() - startX) / stepX);
        const double fy = std::floor((world.y() - startY) / stepY);
        if (!std::isfinite(fx) || !std::isfinite(fy))
            return -1;

        const int ix = fx < 0.0 ? 0 :
            fx >= static_cast<double>(nx) ? nx - 1 :
            static_cast<int>(fx);
        const int iy = fy < 0.0 ? 0 :
            fy >= static_cast<double>(ny) ? ny - 1 :
            static_cast<int>(fy);

        const size_t index = static_cast<size_t>(iy) * sx +
                             static_cast<size_t>(ix);
        const int colorIndex = getDensityColorIndex(values[index]);
        return (colorIndex >= 0 && colorIndex < 11) ? colorIndex : -1;
    };

    QElapsedTimer timer;
    timer.start();

    for (int py = 0; py < image.height(); ++py)
    {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(py));

        for (int px = 0; px < image.width(); ++px)
        {
            int colorIndex = -1;

            if (mode == SampleMode::Center)
            {
                colorIndex = sampleColor(px, py, 0.5, 0.5);
            }
            else if (mode == SampleMode::LastCell)
            {
                colorIndex = sampleColor(px, py, 0.999, 0.999);
            }
            else
            {
                int counts[11] = {0};

                for (int syi = 0; syi < 4; ++syi)
                {
                    for (int sxi = 0; sxi < 4; ++sxi)
                    {
                        const int ci = sampleColor(
                            px, py,
                            (sxi + 0.5) / 4.0,
                            (syi + 0.5) / 4.0);
                        if (ci >= 0)
                            ++counts[ci];
                    }
                }

                int bestCount = 0;
                for (int i = 0; i < 11; ++i)
                {
                    if (counts[i] > bestCount)
                    {
                        bestCount = counts[i];
                        colorIndex = i;
                    }
                }
            }

            if (colorIndex >= 0)
                line[px] = colors[colorIndex];
        }
    }

    qDebug() << "PDMAP Scanline mode:" << static_cast<int>(mode)
             << "Matrix:" << nx << "x" << ny
             << "Image:" << image.size()
             << "Elapsed(ms):" << timer.nsecsElapsed() / 1000000.0;
    return true;
}

/*
Integration example (inside your existing rendering function):

    // Construct from the SAME geometry used by your drawRects path:
    // QRectF windowRect(xMin, yMin, windowX, windowY);
    // QRectF stepRect(xMin, yMin, stepX, stepY);
    // stepRect.moveCenter(windowRect.center());
    // double startX = stepRect.left();
    // double startY = stepRect.top();

    // const QTransform painterTransform = ...; // existing painter transform
    // QRgb colors[11] = { ... };                 // existing color table
    // auto colorFn = [&](double density) {
    //     return getDensityColorIndex(density); // existing function
    // };
    //
    // renderPdmapSampling(image, values, nx, ny,
    //     stepX, stepY, startX, startY, painterTransform,
    //     colors, colorFn, SampleMode::Center);
    //
    // For the second/third run use SampleMode::LastCell or
    // SampleMode::Majority4x4.
    //
    // NOTE: No main() here. This is an integration snippet, not a
    // standalone program. Do not compile candy.cpp by itself.
*/
