// candy.cpp - Qt 5.9 / C++11 standalone rasterization diagnostic
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QTransform>
#include <QRectF>
#include <QVector>
#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <algorithm>
#include <cmath>

static QRgb getColor(int ix, int iy)
{
    return qRgb((ix * 53 + iy * 19) % 256,
                (ix * 11 + iy * 73) % 256,
                (ix * 97 + iy * 37) % 256);
}

static QRectF makeStepRect(int ix, int iy,
                           double windowX, double windowY,
                           double stepX, double stepY)
{
    const double x = ix * stepX;
    const double y = iy * stepY;
    QRectF windowRect(x, y, windowX, windowY);
    QRectF stepRect(x, y, stepX, stepY);
    stepRect.moveCenter(windowRect.center());
    return stepRect;
}

static QImage renderPainter(int nx, int ny,
                            double windowX, double windowY,
                            double stepX, double stepY,
                            const QTransform &transform,
                            const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setTransform(transform);

    QVector<QRectF> batch;
    batch.reserve(4096);
    QRgb batchColor = 0;
    bool hasBatch = false;

    auto flush = [&]() {
        if (batch.isEmpty())
            return;
        painter.setBrush(QColor::fromRgb(batchColor));
        painter.drawRects(batch.constData(), batch.size());
        batch.clear();
    };

    for (int iy = 0; iy < ny; ++iy) {
        for (int ix = 0; ix < nx; ++ix) {
            const QRgb color = getColor(ix, iy);
            if (hasBatch && color != batchColor)
                flush();
            batchColor = color;
            hasBatch = true;
            batch.append(makeStepRect(ix, iy,
                                      windowX, windowY,
                                      stepX, stepY));
            if (batch.size() >= 4096)
                flush();
        }
    }
    flush();
    painter.end();
    return image;
}

static QImage renderScanline(int nx, int ny,
                             double windowX, double windowY,
                             double stepX, double stepY,
                             const QTransform &transform,
                             const QSize &size,
                             double offset)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::white);
    bool ok = false;
    const QTransform inverse = transform.inverted(&ok);
    if (!ok)
        return image;

    const double startX = (windowX - stepX) * 0.5;
    const double startY = (windowY - stepY) * 0.5;

    for (int py = 0; py < size.height(); ++py) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(py));
        for (int px = 0; px < size.width(); ++px) {
            const QPointF p = inverse.map(
                QPointF(px + offset, py + offset));
            const int ix = static_cast<int>(
                std::floor((p.x() - startX) / stepX));
            const int iy = static_cast<int>(
                std::floor((p.y() - startY) / stepY));
            if (ix >= 0 && ix < nx && iy >= 0 && iy < ny)
                line[px] = getColor(ix, iy);
        }
    }
    return image;
}

static void compareImages(const QImage &baseline,
                          const QImage &candidate,
                          const char *name)
{
    int mismatch = 0;
    int white = 0;
    int baselineWhite = 0;
    int bothColoredMismatch = 0;
    const QRgb whiteColor = qRgb(255, 255, 255);

    for (int y = 0; y < baseline.height(); ++y) {
        const QRgb *a = reinterpret_cast<const QRgb *>(
            baseline.constScanLine(y));
        const QRgb *b = reinterpret_cast<const QRgb *>(
            candidate.constScanLine(y));
        for (int x = 0; x < baseline.width(); ++x) {
            if (a[x] == whiteColor)
                ++baselineWhite;
            if (b[x] == whiteColor)
                ++white;
            if (a[x] != b[x]) {
                ++mismatch;
                if (a[x] != whiteColor && b[x] != whiteColor)
                    ++bothColoredMismatch;
            }
        }
    }
    qDebug() << name
             << "Mismatch:" << mismatch
             << "BaselineWhite:" << baselineWhite
             << "CandidateWhite:" << white
             << "BothColoredMismatch:" << bothColoredMismatch;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const int nx = 600;
    const int ny = 240;
    const double windowX = 0.06;
    const double windowY = 0.06;
    const double stepX = 0.06;
    const double stepY = 0.06;
    const QSize size(60, 24);

    QTransform transform;
    transform.scale(
        size.width() / (nx * stepX),
        size.height() / (ny * stepY));

    QElapsedTimer timer;
    timer.start();
    const QImage baseline = renderPainter(
        nx, ny, windowX, windowY,
        stepX, stepY, transform, size);
    qDebug() << "QPainter elapsed(ms):"
             << timer.nsecsElapsed() / 1000000.0;

    const QImage center = renderScanline(
        nx, ny, windowX, windowY,
        stepX, stepY, transform, size, 0.5);
    const QImage left = renderScanline(
        nx, ny, windowX, windowY,
        stepX, stepY, transform, size, 0.001);
    const QImage right = renderScanline(
        nx, ny, windowX, windowY,
        stepX, stepY, transform, size, 0.999);

    qDebug() << "Image pixels:"
             << size.width() * size.height();
    compareImages(baseline, center, "Center");
    compareImages(baseline, left, "UpperLeft");
    compareImages(baseline, right, "LowerRight");

    baseline.save("candy_qpainter.png");
    center.save("candy_center.png");
    left.save("candy_upper_left.png");
    right.save("candy_lower_right.png");
    return 0;
}
