// candy.cpp
// Qt 5.9 / C++11 standalone FULL-IMAGE area-weighted ColorIndex average.
// Synthetic input only. Replace colorIndexAt() and palette() with real data.
// Build: g++ -O3 -std=c++11 -fPIC candy.cpp -o candy $(pkg-config --cflags --libs Qt5Core Qt5Gui)
// Run: ./candy [nx ny imageWidth imageHeight]
// Examples: ./candy 600 240 60 24
//           ./candy 66667 22500 992 336
//
// Important: This reproduces the tested ColorIndex average, NOT a raw-density
// average, and is NOT pixel-identical to QPainter's grouped flush rendering.
// Runtime is O(nx*ny) in the general case; 1.5B cells may take substantial time.
// Output uses a fixed-size QImage; no 1.5B-cell array is allocated.

#include <QCoreApplication>
#include <QImage>
#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

static const int COLORS = 11;

// Replace with the actual palette lookup. Index order must match production.
static QRgb palette(int i)
{
    static const QRgb colors[COLORS] = {
        qRgb(20,25,55), qRgb(42,100,190), qRgb(32,175,125),
        qRgb(245,195,60), qRgb(220,85,70), qRgb(150,80,195),
        qRgb(235,235,235), qRgb(75,170,210), qRgb(180,135,60),
        qRgb(85,90,100), qRgb(240,115,175)
    };
    return colors[i];
}

// Replace with your real matrix/palette-index accessor.
// x in [0,nx), y in [0,ny). No production data is included.
static int colorIndexAt(int x, int y)
{
    const int band = (x / 17 + y / 11) % COLORS;
    const int detail = ((x * 13LL + y * 7LL) % 19 == 0) ? 3 : 0;
    return (band + detail) % COLORS;
}

// Each output pixel covers a continuous rectangle in input cell coordinates.
// This is an exact area-weighted mean of COLOR INDICES for axis-aligned,
// uniformly spaced cells. Output uses the nearest palette index.
// Image origin follows Qt (top-left); adapt y mapping if input is bottom-up.
static QImage renderAverageColorIndex(int nx, int ny, int width, int height)
{
    QImage result(width, height, QImage::Format_ARGB32);
    if (result.isNull()) return result;
    result.fill(Qt::transparent);

    const double sx = double(nx) / width;
    const double sy = double(ny) / height;

    for (int py = 0; py < height; ++py) {
        QRgb *dst = reinterpret_cast<QRgb *>(result.scanLine(py));
        const double top = py * sy;
        const double bottom = (py + 1.0) * sy;
        const int y0 = std::max(0, int(std::floor(top)));
        const int y1 = std::min(ny - 1, int(std::ceil(bottom)) - 1);

        for (int px = 0; px < width; ++px) {
            const double left = px * sx;
            const double right = (px + 1.0) * sx;
            const int x0 = std::max(0, int(std::floor(left)));
            const int x1 = std::min(nx - 1, int(std::ceil(right)) - 1);
            double weighted = 0.0;
            double total = 0.0;

            for (int y = y0; y <= y1; ++y) {
                const double wy = std::max(0.0,
                    std::min(bottom, double(y + 1)) - std::max(top, double(y)));
                if (wy <= 0.0) continue;
                for (int x = x0; x <= x1; ++x) {
                    const double wx = std::max(0.0,
                        std::min(right, double(x + 1)) - std::max(left, double(x)));
                    const double weight = wx * wy;
                    if (weight <= 0.0) continue;
                    const int idx = colorIndexAt(x, y);
                    if (idx < 0 || idx >= COLORS) continue; // missing cell
                    weighted += weight * idx;
                    total += weight;
                }
            }

            if (total > 0.0) {
                const int index = std::max(0, std::min(COLORS - 1,
                    int(std::floor(weighted / total + 0.5))));
                dst[px] = palette(index);
            }
        }
        if ((py + 1) % 32 == 0 || py + 1 == height)
            qDebug() << "rows:" << (py + 1) << "/" << height;
    }
    return result;
}

static int positiveArg(char *s, int fallback)
{
    if (!s) return fallback;
    char *end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || v <= 0 ||
        v > std::numeric_limits<int>::max()) return fallback;
    return static_cast<int>(v);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const int nx = argc > 1 ? positiveArg(argv[1], 600) : 600;
    const int ny = argc > 2 ? positiveArg(argv[2], 240) : 240;
    const int w = argc > 3 ? positiveArg(argv[3], 60) : 60;
    const int h = argc > 4 ? positiveArg(argv[4], 24) : 24;

    qDebug() << "Input matrix:" << nx << "x" << ny
             << "Output image:" << w << "x" << h;
    QElapsedTimer timer;
    timer.start();
    const QImage image = renderAverageColorIndex(nx, ny, w, h);
    if (image.isNull() || !image.save("candy_average_full.png")) {
        qCritical() << "Failed to generate/save output image";
        return 1;
    }
    qDebug() << "Saved candy_average_full.png; elapsed ms:" << timer.elapsed();
    return 0;
}
