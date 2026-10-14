// candy.cpp
// Qt 5.9 / C++11: identify the LAST QRectF cell written by QPainter.
// Standalone diagnostic. No proprietary data.
// Build: g++ -std=c++11 -fPIC candy.cpp -o candy $(pkg-config --cflags --libs Qt5Gui Qt5Core)
// Run: ./candy
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QTransform>
#include <QRectF>
#include <QVector>
#include <QColor>
#include <QDebug>
#include <QtGlobal>
#include <cmath>
#include <algorithm>

static QRgb encodeId(int id)
{
    // ID 0 is reserved for untouched pixels (black).
    return qRgb(id & 255, (id >> 8) & 255, (id >> 16) & 255);
}
static int decodeId(QRgb rgb)
{
    return qRed(rgb) | (qGreen(rgb) << 8) | (qBlue(rgb) << 16);
}
static int clampIndex(int i, int n)
{
    return std::max(0, std::min(n - 1, i));
}
static int centerIndex(double coordinate, double step, int n)
{
    return clampIndex(static_cast<int>(std::floor(coordinate / step)), n);
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    const int nx = 600;
    const int ny = 240;
    const double windowX = 0.06, windowY = 0.06;
    const double stepX = 0.06, stepY = 0.06;
    const QSize imageSize(60, 24);

    // Replace with the EXACT production transform for a second test.
    QTransform transform;
    transform.scale(double(imageSize.width()) / (nx * stepX),
                    double(imageSize.height()) / (ny * stepY));

    QImage baseline(imageSize, QImage::Format_ARGB32);
    baseline.fill(qRgb(0, 0, 0)); // ID=0 means untouched

    QPainter painter(&baseline);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setTransform(transform);

    // Preserve row-major draw order, ix increasing first.
    // Consecutive same-color rectangles can be batched; unique IDs
    // mean each cell must have its own brush and draw call.
    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            const int id = iy * nx + ix + 1;
            const double x = ix * stepX, y = iy * stepY;
            QRectF windowRect(x, y, windowX, windowY);
            QRectF stepRect(x, y, stepX, stepY);
            stepRect.moveCenter(windowRect.center());
            painter.setBrush(QColor::fromRgb(encodeId(id)));
            painter.drawRect(stepRect);
        }
    }
    painter.end();

    bool invertible = false;
    const QTransform inverse = transform.inverted(&invertible);
    if (!invertible) {
        qWarning() << "Non-invertible transform";
        return 1;
    }

    const double firstX = (windowX - stepX) * 0.5;
    const double firstY = (windowY - stepY) * 0.5;
    int untouched = 0, matched = 0, mismatched = 0;
    int minDx = nx, maxDx = -nx, minDy = ny, maxDy = -ny;
    int shown = 0;

    for (int py = 0; py < imageSize.height(); ++py) {
        const QRgb *line = reinterpret_cast<const QRgb *>(baseline.constScanLine(py));
        for (int px = 0; px < imageSize.width(); ++px) {
            const int id = decodeId(line[px]);
            if (id == 0) {
                ++untouched;
                continue;
            }
            if (id < 1 || id > nx * ny) {
                qWarning() << "Unexpected ID:" << id;
                continue;
            }
            const int painterIx = (id - 1) % nx;
            const int painterIy = (id - 1) / nx;
            const QPointF p = inverse.map(QPointF(px + 0.5, py + 0.5));
            const int centerIx = centerIndex((p.x() - firstX), stepX, nx);
            const int centerIy = centerIndex((p.y() - firstY), stepY, ny);
            const int dx = painterIx - centerIx;
            const int dy = painterIy - centerIy;
            minDx = std::min(minDx, dx);
            maxDx = std::max(maxDx, dx);
            minDy = std::min(minDy, dy);
            maxDy = std::max(maxDy, dy);
            if (dx == 0 && dy == 0) {
                ++matched;
            } else {
                ++mismatched;
                if (shown++ < 20) {
                    qDebug() << "Pixel" << px << py
                             << "Painter(ix,iy)" << painterIx << painterIy
                             << "Center(ix,iy)" << centerIx << centerIy
                             << "Delta(dx,dy)" << dx << dy;
                }
            }
        }
    }

    qDebug() << "Pixels:" << imageSize.width() * imageSize.height();
    qDebug() << "Untouched:" << untouched;
    qDebug() << "Matched:" << matched << "Mismatched:" << mismatched;
    if (matched + mismatched > 0)
        qDebug() << "Delta range X:" << minDx << maxDx
                 << "Y:" << minDy << maxDy;
    baseline.save("candy_id_debug.png");
    qDebug() << "Saved candy_id_debug.png (RGB encodes cell ID; not a natural-color image)";
    return 0;
}
