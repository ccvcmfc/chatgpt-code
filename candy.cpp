// candy.cpp
// Qt 5.9 / C++11: separate full-image AverageColorIndex scanline renderer.
// Uses the SAME physical-to-image transform as the working Center Scanline.
// No manual Y flip, region.top reversal, or ny-1-y remapping.
// Compile standalone:
// g++ -O3 -std=c++11 -fPIC candy.cpp -o candy $(pkg-config --cflags --libs Qt5Core Qt5Gui)
// Run: ./candy
//
// IMPORTANT: The transform MUST be the real working scanline/painter transform
// for the CURRENT SCROLLED REGION, not an unrelated full-layout transform.
// Matrix cell (x,y) spans [originX+x*stepX, originX+(x+1)*stepX]
//                         [originY+y*stepY, originY+(y+1)*stepY].
// Matrix y storage direction is defined by densityAt(x,y).
// The renderer assumes an axis-aligned transform (including negative Y scale).
// It averages PALETTE INDICES, not raw density values.

#include <QCoreApplication>
#include <QImage>
#include <QTransform>
#include <QElapsedTimer>
#include <QDebug>
#include <QColor>
#include <QRgb>
#include <algorithm>
#include <cmath>
#include <limits>

struct AverageScanlineInput {
    int nx, ny;
    double originX, originY;
    double stepX, stepY;
    int imageWidth, imageHeight;
    QTransform physicalToImage; // EXACT transform from working Center Scanline
};

static int clampedIndex(long long index, int count)
{
    return static_cast<int>(std::max(0LL,
        std::min(index, static_cast<long long>(count) - 1)));
}

template <typename DensityAt, typename DensityToIndex, typename ColorAt>
bool renderAverageScanLine(const AverageScanlineInput &in,
                           int paletteCount,
                           DensityAt densityAt,
                           DensityToIndex densityToIndex,
                           ColorAt colorAt,
                           QImage &out)
{
    if (in.nx <= 0 || in.ny <= 0 || in.imageWidth <= 0 ||
        in.imageHeight <= 0 || paletteCount <= 0 ||
        !(in.stepX > 0.0) || !(in.stepY > 0.0) ||
        !std::isfinite(in.originX) || !std::isfinite(in.originY) ||
        !std::isfinite(in.stepX) || !std::isfinite(in.stepY))
        return false;

    // The interval-based algorithm requires no rotation/shear/perspective.
    const QTransform &t = in.physicalToImage;
    if (std::abs(t.m12()) > 1e-12 || std::abs(t.m21()) > 1e-12 ||
        std::abs(t.m13()) > 1e-12 || std::abs(t.m23()) > 1e-12 ||
        std::abs(t.m33() - 1.0) > 1e-12)
        return false;

    bool invertible = false;
    const QTransform inv = t.inverted(&invertible);
    if (!invertible) return false;

    QImage image(in.imageWidth, in.imageHeight, QImage::Format_ARGB32);
    if (image.isNull()) return false;
    image.fill(Qt::transparent);

    QElapsedTimer progress;
    QElapsedTimer total;
    progress.start();
    total.start();

    for (int py = 0; py < in.imageHeight; ++py) {
        QRgb *dst = reinterpret_cast<QRgb*>(image.scanLine(py));
        for (int px = 0; px < in.imageWidth; ++px) {
            // Inverse-map the pixel FOOTPRINT, not just its center.
            // min/max handles a negative Y scale without extra flipping.
            const QPointF p0 = inv.map(QPointF(px, py));
            const QPointF p1 = inv.map(QPointF(px + 1.0, py + 1.0));
            const double x0 = std::min(p0.x(), p1.x());
            const double x1 = std::max(p0.x(), p1.x());
            const double y0 = std::min(p0.y(), p1.y());
            const double y1 = std::max(p0.y(), p1.y());

            const double mx0 = (x0 - in.originX) / in.stepX;
            const double mx1 = (x1 - in.originX) / in.stepX;
            const double my0 = (y0 - in.originY) / in.stepY;
            const double my1 = (y1 - in.originY) / in.stepY;
            if (!std::isfinite(mx0) || !std::isfinite(mx1) ||
                !std::isfinite(my0) || !std::isfinite(my1))
                continue;
            // Do not clamp an OUTSIDE pixel to an edge cell.
            if (mx1 <= 0.0 || mx0 >= in.nx ||
                my1 <= 0.0 || my0 >= in.ny)
                continue;

            const int xBegin = clampedIndex(
                static_cast<long long>(std::floor(mx0)), in.nx);
            const int xEnd = clampedIndex(
                static_cast<long long>(std::ceil(mx1)) - 1, in.nx);
            const int yBegin = clampedIndex(
                static_cast<long long>(std::floor(my0)), in.ny);
            const int yEnd = clampedIndex(
                static_cast<long long>(std::ceil(my1)) - 1, in.ny);

            double weighted = 0.0;
            double totalWeight = 0.0;
            for (int iy = yBegin; iy <= yEnd; ++iy) {
                const double overlapY = std::max(0.0,
                    std::min(my1, double(iy + 1)) -
                    std::max(my0, double(iy)));
                if (overlapY <= 0.0) continue;

                for (int ix = xBegin; ix <= xEnd; ++ix) {
                    const double overlapX = std::max(0.0,
                        std::min(mx1, double(ix + 1)) -
                        std::max(mx0, double(ix)));
                    const double weight = overlapX * overlapY;
                    if (weight <= 0.0) continue;

                    const double density = densityAt(ix, iy);
                    if (!std::isfinite(density)) continue;
                    const int colorIndex = densityToIndex(density);
                    if (colorIndex < 0 || colorIndex >= paletteCount) continue;
                    weighted += colorIndex * weight;
                    totalWeight += weight;
                }
            }
            if (totalWeight <= 0.0) continue;
            const int avgIndex = std::max(0, std::min(paletteCount - 1,
                static_cast<int>(std::floor(weighted / totalWeight + 0.5))));
            dst[px] = colorAt(avgIndex);
        }
        if (progress.elapsed() >= 1000) {
            qDebug() << "AverageScanLine progress:"
                     << 100.0 * (py + 1) / in.imageHeight
                     << "% elapsed(ms):" << total.elapsed();
            progress.restart();
        }
    }
    out = image;
    qDebug() << "AverageScanLine total(ms):" << total.elapsed();
    return true;
}

// ------------------------------------------------------------
// Synthetic standalone demonstration (remove main on integration)
// ------------------------------------------------------------
static const int DEMO_COLORS = 11;
static double demoDensity(int x, int y)
{
    const int base = (x / 17 + y / 11) % DEMO_COLORS;
    const int detail = ((13LL*x + 7LL*y) % 19 == 0) ? 3 : 0;
    return double((base + detail) % DEMO_COLORS) / (DEMO_COLORS - 1);
}
static int demoDensityToIndex(double v)
{
    if (v < 0.0 || v > 1.0) return -1;
    return std::max(0, std::min(DEMO_COLORS - 1,
        static_cast<int>(std::floor(v * (DEMO_COLORS - 1) + 0.5))));
}
static QRgb demoColorAt(int i)
{
    static const QRgb p[DEMO_COLORS] = {
        qRgb(20,25,55), qRgb(42,100,190), qRgb(32,175,125),
        qRgb(245,195,60), qRgb(220,85,70), qRgb(150,80,195),
        qRgb(235,235,235), qRgb(75,170,210), qRgb(180,135,60),
        qRgb(85,90,100), qRgb(240,115,175)
    };
    return p[i];
}
int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    AverageScanlineInput in;
    in.nx = 600; in.ny = 240;
    in.originX = 0; in.originY = 0;
    in.stepX = 0.06; in.stepY = 0.06;
    in.imageWidth = 60; in.imageHeight = 24;

    // Demonstration: physical Y increases upward; image Y downward.
    // Current viewport covers the whole matrix.
    const double sx = in.imageWidth / (in.nx * in.stepX);
    const double sy = in.imageHeight / (in.ny * in.stepY);
    in.physicalToImage = QTransform(
        sx, 0, 0, 0, -sy, 0,
        -in.originX * sx, in.imageHeight + in.originY * sy, 1);

    QImage image;
    if (!renderAverageScanLine(in, DEMO_COLORS,
                               demoDensity, demoDensityToIndex,
                               demoColorAt, image)) {
        qCritical() << "AverageScanLine failed";
        return 1;
    }
    if (!image.save("candy_average_full.png")) {
        qCritical() << "Image save failed";
        return 1;
    }
    qDebug() << "Saved candy_average_full.png";
    return 0;
}
