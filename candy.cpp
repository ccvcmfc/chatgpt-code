// candy.cpp
// Qt 5.9 / C++11 standalone experiment: MaxColorIndex vs AverageDensity.
// Synthetic data only. Does not modify production scanline rendering.
// Build: g++ -std=c++11 -fPIC candy.cpp -o candy $(pkg-config --cflags --libs Qt5Core Qt5Gui)
// Run: ./candy
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QTransform>
#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QtGlobal>
#include <vector>
#include <cmath>
#include <algorithm>

static const int NX = 600;
static const int NY = 240;
static const int W = 60;
static const int H = 24;
static const int COLORS = 11;
static const int LIMIT = 1024 * 1024;
static const double STEP = 0.06;

static QRgb palette(int i)
{
    static const QRgb c[COLORS] = {
        qRgb(20,25,55), qRgb(42,100,190), qRgb(32,175,125),
        qRgb(245,195,60), qRgb(220,85,70), qRgb(150,80,195),
        qRgb(235,235,235), qRgb(75,170,210), qRgb(180,135,60),
        qRgb(85,90,100), qRgb(240,115,175)
    };
    return c[i];
}

static int densityIndex(int x, int y)
{
    // Deterministic synthetic spatial pattern.
    const int band = (x / 17 + y / 11) % COLORS;
    const int detail = ((x * 13 + y * 7) % 19 == 0) ? 3 : 0;
    return (band + detail) % COLORS;
}

static QTransform makeTransform()
{
    QTransform t;
    t.scale(double(W) / (NX * STEP), double(H) / (NY * STEP));
    return t;
}

static QImage painterReference()
{
    QImage image(W, H, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setPen(Qt::NoPen);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setTransform(makeTransform());
    std::vector<QRectF> batches[COLORS];
    int pending = 0;
    const auto flush = [&]() {
        for (int c = 0; c < COLORS; ++c) {
            if (!batches[c].empty()) {
                painter.setBrush(QColor::fromRgb(palette(c)));
                painter.drawRects(batches[c].data(), static_cast<int>(batches[c].size()));
                batches[c].clear();
            }
        }
        pending = 0;
    };
    // Bottom to top, then left to right, as described for AS-IS.
    for (int y = NY - 1; y >= 0; --y) {
        for (int x = 0; x < NX; ++x) {
            if (pending >= LIMIT - 4) flush();
            const int c = densityIndex(x, y);
            batches[c].push_back(QRectF(x * STEP, y * STEP, STEP, STEP));
            ++pending;
        }
    }
    flush();
    painter.end();
    return image;
}

static QImage aggregate(bool useMaximum)
{
    QImage image(W, H, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    bool ok = false;
    const QTransform inverse = makeTransform().inverted(&ok);
    if (!ok) return image;

    for (int py = 0; py < H; ++py) {
        QRgb *line = reinterpret_cast<QRgb*>(image.scanLine(py));
        for (int px = 0; px < W; ++px) {
            // Pixel footprint in matrix coordinates, axis-aligned transform.
            const QPointF a = inverse.map(QPointF(px, py));
            const QPointF b = inverse.map(QPointF(px + 1.0, py + 1.0));
            const double xmin = std::min(a.x(), b.x());
            const double xmax = std::max(a.x(), b.x());
            const double ymin = std::min(a.y(), b.y());
            const double ymax = std::max(a.y(), b.y());
            const int x0 = std::max(0, static_cast<int>(std::floor(xmin / STEP)) - 1);
            const int x1 = std::min(NX - 1, static_cast<int>(std::ceil(xmax / STEP)));
            const int y0 = std::max(0, static_cast<int>(std::floor(ymin / STEP)) - 1);
            const int y1 = std::min(NY - 1, static_cast<int>(std::ceil(ymax / STEP)));
            int maximum = 0;
            double weightedSum = 0.0;
            double weightTotal = 0.0;
            for (int iy = y0; iy <= y1; ++iy) {
                const double sy0 = iy * STEP, sy1 = sy0 + STEP;
                const double overlapY = std::max(0.0, std::min(ymax, sy1) - std::max(ymin, sy0));
                if (overlapY <= 0.0) continue;
                for (int ix = x0; ix <= x1; ++ix) {
                    const double sx0 = ix * STEP, sx1 = sx0 + STEP;
                    const double overlapX = std::max(0.0, std::min(xmax, sx1) - std::max(xmin, sx0));
                    const double weight = overlapX * overlapY;
                    if (weight <= 0.0) continue;
                    const int c = densityIndex(ix, iy);
                    maximum = std::max(maximum, c);
                    weightedSum += c * weight;
                    weightTotal += weight;
                }
            }
            if (weightTotal > 0.0) {
                const int result = useMaximum ? maximum :
                    std::max(0, std::min(COLORS - 1,
                        static_cast<int>(std::floor(weightedSum / weightTotal + 0.5))));
                line[px] = palette(result);
            }
        }
    }
    return image;
}

static void compare(const char *name, const QImage &ref, const QImage &test)
{
    quint64 match = 0, mismatch = 0, refTransparent = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const QRgb a = ref.pixel(x, y);
            const QRgb b = test.pixel(x, y);
            if (qAlpha(a) == 0) ++refTransparent;
            if (a == b) ++match; else ++mismatch;
        }
    }
    qDebug() << name << "match:" << match << "mismatch:" << mismatch
             << "match rate:" << (100.0 * match / (match + mismatch))
             << "% reference transparent:" << refTransparent;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QElapsedTimer timer;
    timer.start();
    const QImage ref = painterReference();
    qDebug() << "Painter ms:" << timer.elapsed();
    ref.save("candy_painter.png");

    timer.restart();
    const QImage maxImage = aggregate(true);
    qDebug() << "MaxColorIndex ms:" << timer.elapsed();
    maxImage.save("candy_max_color.png");
    compare("MaxColorIndex", ref, maxImage);

    timer.restart();
    const QImage avgImage = aggregate(false);
    qDebug() << "AverageDensity ms:" << timer.elapsed();
    avgImage.save("candy_average.png");
    compare("AverageDensity", ref, avgImage);

    qDebug() << "Synthetic example:" << NX << "x" << NY
             << "matrix ->" << W << "x" << H << "image.";
    qDebug() << "NOTE: Flush-aware painter is the reference; Max/Average"
                " are approximations. Adapt region/transform/data for production.";
    return 0;
}
