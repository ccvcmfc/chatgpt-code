// candy.cpp
// Standalone Qt 5.9 Widgets-compatible diagnostic.
// Compares QPainter::drawRects(QRectF) with inverse-mapped sampling.
// This is a synthetic test; it does NOT reproduce the production transform automatically.
//
// Build (example):
//   g++ -std=c++11 -fPIC candy.cpp -o candy $(pkg-config --cflags --libs Qt5Gui Qt5Core)
// Run:
//   ./candy
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QTransform>
#include <QVector>
#include <QRectF>
#include <QColor>
#include <QDebug>
#include <QtMath>
#include <algorithm>
#include <cmath>

static int clampIndex(int v, int count)
{
    return std::max(0, std::min(v, count - 1));
}

static QRgb colorFor(int ix, int iy)
{
    // Opaque, deterministic colors, deliberately varying in both axes.
    return qRgb((ix * 53 + iy * 19) % 256,
                (ix * 11 + iy * 73) % 256,
                (ix * 97 + iy * 37) % 256);
}

static QImage renderPainter(int nx, int ny, double step,
                            const QTransform &transform, const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(qRgb(255, 255, 255));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setTransform(transform);

    // Row-major, same order as values[iy * nx + ix].
    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor::fromRgb(colorFor(ix, iy)));
            painter.drawRect(QRectF(ix * step, iy * step, step, step));
        }
    }
    painter.end();
    return image;
}

enum SampleMode {
    PixelCenter,
    PixelNearUpperLeft,
    PixelNearLowerRight
};

static QImage renderInverse(int nx, int ny, double step,
                            const QTransform &transform, const QSize &size,
                            SampleMode mode)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(qRgb(255, 255, 255));

    bool ok = false;
    const QTransform inverse = transform.inverted(&ok);
    if (!ok) {
        qWarning() << "Non-invertible transform";
        return image;
    }

    const double offset = (mode == PixelCenter) ? 0.5 :
                          (mode == PixelNearUpperLeft) ? 0.001 : 0.999;

    for (int py = 0; py < size.height(); ++py) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(py));
        for (int px = 0; px < size.width(); ++px) {
            const QPointF world = inverse.map(QPointF(px + offset, py + offset));
            const int ix = static_cast<int>(std::floor(world.x() / step));
            const int iy = static_cast<int>(std::floor(world.y() / step));
            if (ix >= 0 && ix < nx && iy >= 0 && iy < ny)
                line[px] = colorFor(ix, iy);
        }
    }
    return image;
}

static int mismatchCount(const QImage &a, const QImage &b)
{
    int mismatches = 0;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb *pa = reinterpret_cast<const QRgb *>(a.constScanLine(y));
        const QRgb *pb = reinterpret_cast<const QRgb *>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            mismatches += (pa[x] != pb[x]);
    }
    return mismatches;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    // Many tiny QRectF cells per output pixel, like the 1.5-billion-cell case.
    const int nx = 600, ny = 240;
    const double step = 0.06;
    const QSize imageSize(60, 24);

    // No rotation or reflection. Replace this with the real QPainter
    // transform and real rectangle origin to reproduce production behavior.
    QTransform transform;
    transform.scale(imageSize.width() / (nx * step),
                    imageSize.height() / (ny * step));

    qDebug() << "Transform:" << transform;
    const QImage baseline = renderPainter(nx, ny, step, transform, imageSize);
    const QImage center = renderInverse(nx, ny, step, transform, imageSize, PixelCenter);
    const QImage upperLeft = renderInverse(nx, ny, step, transform, imageSize, PixelNearUpperLeft);
    const QImage lowerRight = renderInverse(nx, ny, step, transform, imageSize, PixelNearLowerRight);

    qDebug() << "Image pixels:" << imageSize.width() * imageSize.height();
    qDebug() << "Mismatch vs QPainter (center):" << mismatchCount(baseline, center);
    qDebug() << "Mismatch vs QPainter (upper-left):" << mismatchCount(baseline, upperLeft);
    qDebug() << "Mismatch vs QPainter (lower-right):" << mismatchCount(baseline, lowerRight);

    baseline.save("candy_qpainter.png");
    center.save("candy_center.png");
    upperLeft.save("candy_upper_left.png");
    lowerRight.save("candy_lower_right.png");
    qDebug() << "Saved 4 diagnostic PNG images.";
    return 0;
}
