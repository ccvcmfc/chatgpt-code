// candy.cpp
// Standalone Qt 5.9 / C++11 implementation of a separate clamped-index
// area-weighted average COLOR INDEX renderer. No proprietary source.
// This function is intentionally independent of an existing Center Scanline.
// Compile: g++ -O3 -std=c++11 -fPIC candy.cpp -o candy $(pkg-config --cflags --libs Qt5Core Qt5Gui)
// Usage: ./candy [nx ny imageWidth imageHeight]
// Example: ./candy 66667 22500 992 336
//
// Important: matches the earlier experiment's AVERAGE PALETTE INDEX, not
// average raw density. It does not reproduce QPainter flush semantics exactly.

#include <QCoreApplication>
#include <QImage>
#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QRgb>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

struct DensityRegion {
    double left, top, right, bottom; // physical coordinates; top < bottom
};

struct AverageScanlineInput {
    int nx, ny;
    double originX, originY; // physical position of matrix cell (0,0)
    double stepX, stepY;     // strictly positive
    DensityRegion region;    // requested image region
    int imageWidth, imageHeight;
    bool flipY;              // true if matrix Y increases upwards in output
};

static int clampIndex(long long i, int n) {
    return int(std::max(0LL, std::min(i, static_cast<long long>(n) - 1)));
}

// Main integration point:
// indexAt(x,y) -> palette index (0..paletteCount-1), or -1 to skip.
// colorAt(index) -> packed QRgb. Both callbacks must be thread-safe only if
// caller later introduces threading; this implementation is single-threaded.
//
// Returns false for invalid dimensions/geometry, or QImage allocation failure.
// Matrix storage and lifetime remain fully controlled by the caller.
template <typename IndexAt, typename ColorAt>
bool renderAverageScanLine(const AverageScanlineInput &in,
                           int paletteCount,
                           IndexAt indexAt,
                           ColorAt colorAt,
                           QImage &out)
{
    if (in.nx <= 0 || in.ny <= 0 || in.imageWidth <= 0 ||
        in.imageHeight <= 0 || paletteCount <= 0 ||
        !(in.stepX > 0.0) || !(in.stepY > 0.0) ||
        !(in.region.right > in.region.left) ||
        !(in.region.bottom > in.region.top)) return false;

    QImage image(in.imageWidth, in.imageHeight, QImage::Format_ARGB32);
    if (image.isNull()) return false;
    image.fill(Qt::transparent);

    const double pixelW = (in.region.right - in.region.left) / in.imageWidth;
    const double pixelH = (in.region.bottom - in.region.top) / in.imageHeight;

    for (int py = 0; py < in.imageHeight; ++py) {
        QRgb *dst = reinterpret_cast<QRgb *>(image.scanLine(py));
        const double physY0 = in.region.top + py * pixelH;
        const double physY1 = in.region.top + (py + 1.0) * pixelH;

        // In the normal mode, output y grows in the same direction as matrix y.
        // In flipped mode, reverse physical Y around the full matrix extent.
        const double mY0 = in.flipY
            ? (in.originY + in.ny * in.stepY - physY1) / in.stepY
            : (physY0 - in.originY) / in.stepY;
        const double mY1 = in.flipY
            ? (in.originY + in.ny * in.stepY - physY0) / in.stepY
            : (physY1 - in.originY) / in.stepY;

        // Exclusive upper index, clamped. Empty intersections stay empty.
        const long long rawY0 = static_cast<long long>(std::floor(mY0));
        const long long rawY1 = static_cast<long long>(std::ceil(mY1));
        const int yBegin = clampIndex(rawY0, in.ny);
        const int yEnd = clampIndex(rawY1 - 1, in.ny);

        for (int px = 0; px < in.imageWidth; ++px) {
            const double physX0 = in.region.left + px * pixelW;
            const double physX1 = in.region.left + (px + 1.0) * pixelW;
            const double mX0 = (physX0 - in.originX) / in.stepX;
            const double mX1 = (physX1 - in.originX) / in.stepX;
            const long long rawX0 = static_cast<long long>(std::floor(mX0));
            const long long rawX1 = static_cast<long long>(std::ceil(mX1));
            const int xBegin = clampIndex(rawX0, in.nx);
            const int xEnd = clampIndex(rawX1 - 1, in.nx);

            // Clamping alone is insufficient: a pixel outside the matrix
            // must remain transparent, not sample the nearest edge cell.
            if (mX1 <= 0.0 || mX0 >= in.nx ||
                mY1 <= 0.0 || mY0 >= in.ny) continue;

            double weightedIndex = 0.0;
            double totalArea = 0.0;
            for (int y = yBegin; y <= yEnd; ++y) {
                const double wy = std::max(0.0,
                    std::min(mY1, double(y + 1)) - std::max(mY0, double(y)));
                if (wy <= 0.0) continue;
                for (int x = xBegin; x <= xEnd; ++x) {
                    const double wx = std::max(0.0,
                        std::min(mX1, double(x + 1)) - std::max(mX0, double(x)));
                    const double area = wx * wy;
                    if (area <= 0.0) continue;
                    const int idx = indexAt(x, y);
                    if (idx < 0 || idx >= paletteCount) continue;
                    weightedIndex += idx * area;
                    totalArea += area;
                }
            }
            if (totalArea <= 0.0) continue;
            const int avg = std::max(0, std::min(paletteCount - 1,
                int(std::floor(weightedIndex / totalArea + 0.5))));
            dst[px] = colorAt(avg);
        }
    }
    out = image;
    return true;
}

// ----- Synthetic harness only. Remove main() when integrating. -----
static const int PALETTE_COUNT = 11;
static QRgb demoPalette(int i) {
    static const QRgb p[PALETTE_COUNT] = {
        qRgb(20,25,55), qRgb(42,100,190), qRgb(32,175,125),
        qRgb(245,195,60), qRgb(220,85,70), qRgb(150,80,195),
        qRgb(235,235,235), qRgb(75,170,210), qRgb(180,135,60),
        qRgb(85,90,100), qRgb(240,115,175)
    };
    return p[i];
}
static int demoIndex(int x, int y) {
    const int band = (x / 17 + y / 11) % PALETTE_COUNT;
    const int detail = ((13LL*x + 7LL*y) % 19 == 0) ? 3 : 0;
    return (band + detail) % PALETTE_COUNT;
}
static int argPositive(char *s, int fallback) {
    if (!s) return fallback;
    char *end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || v <= 0 ||
        v > std::numeric_limits<int>::max()) return fallback;
    return int(v);
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const int nx = argc > 1 ? argPositive(argv[1], 600) : 600;
    const int ny = argc > 2 ? argPositive(argv[2], 240) : 240;
    const int w  = argc > 3 ? argPositive(argv[3], 60) : 60;
    const int h  = argc > 4 ? argPositive(argv[4], 24) : 24;
    AverageScanlineInput in;
    in.nx = nx; in.ny = ny;
    in.originX = 0; in.originY = 0;
    in.stepX = 0.06; in.stepY = 0.06;
    in.region = DensityRegion{0, 0, nx * in.stepX, ny * in.stepY};
    in.imageWidth = w; in.imageHeight = h;
    in.flipY = false;
    QImage output;
    QElapsedTimer timer;
    timer.start();
    if (!renderAverageScanLine(in, PALETTE_COUNT, demoIndex, demoPalette, output)) {
        qCritical() << "Invalid geometry or QImage allocation failed";
        return 1;
    }
    if (!output.save("candy_average_full.png")) {
        qCritical() << "Could not save output image";
        return 1;
    }
    qDebug() << "renderAverageScanLine:" << nx << "x" << ny
             << "->" << w << "x" << h
             << "elapsed ms:" << timer.elapsed();
    return 0;
}
