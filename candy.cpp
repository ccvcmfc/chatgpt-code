// candy.cpp - Qt 5.9 / C++11, standalone nine-position scanline experiment.
// Build: g++ -std=c++11 -fPIC candy.cpp -o candy $(pkg-config --cflags --libs Qt5Core Qt5Gui)
// Run: ./candy
// Synthetic data only. Each output pixel reads ONE nearest matrix cell per mode.
// Compare against QPainter's rectangle rasterization, not against real production data.
#include <QCoreApplication>
#include <QImage>
#include <QPainter>
#include <QTransform>
#include <QRectF>
#include <QDebug>
#include <QElapsedTimer>
#include <QColor>
#include <QtGlobal>
#include <cmath>
#include <algorithm>

enum class SampleMode {
    Center, TopLeft, Top, TopRight, Left, Right,
    BottomLeft, Bottom, BottomRight
};

static const char* modeName(SampleMode m)
{
    switch (m) {
    case SampleMode::Center: return "Center";
    case SampleMode::TopLeft: return "TopLeft";
    case SampleMode::Top: return "Top";
    case SampleMode::TopRight: return "TopRight";
    case SampleMode::Left: return "Left";
    case SampleMode::Right: return "Right";
    case SampleMode::BottomLeft: return "BottomLeft";
    case SampleMode::Bottom: return "Bottom";
    case SampleMode::BottomRight: return "BottomRight";
    }
    return "Unknown";
}

static QPointF sampleOffset(SampleMode m)
{
    // Offsets within an OUTPUT pixel; not neighboring matrix cells.
    const double lo = 0.001, hi = 0.999, mid = 0.5;
    switch (m) {
    case SampleMode::TopLeft:     return QPointF(lo, lo);
    case SampleMode::Top:         return QPointF(mid, lo);
    case SampleMode::TopRight:    return QPointF(hi, lo);
    case SampleMode::Left:        return QPointF(lo, mid);
    case SampleMode::Right:       return QPointF(hi, mid);
    case SampleMode::BottomLeft:  return QPointF(lo, hi);
    case SampleMode::Bottom:      return QPointF(mid, hi);
    case SampleMode::BottomRight: return QPointF(hi, hi);
    default:                      return QPointF(mid, mid);
    }
}

static QRgb cellColor(int ix, int iy)
{
    // Deterministic, opaque synthetic colors; no company data.
    const unsigned int h = static_cast<unsigned int>(ix) * 73856093u
                         ^ static_cast<unsigned int>(iy) * 19349663u;
    static const QRgb palette[] = {
        qRgb(20, 25, 55), qRgb(42, 100, 190), qRgb(32, 175, 125),
        qRgb(245, 195, 60), qRgb(220, 85, 70), qRgb(150, 80, 195),
        qRgb(235, 235, 235), qRgb(75, 170, 210),
        qRgb(180, 135, 60), qRgb(85, 90, 100), qRgb(240, 115, 175)
    };
    return palette[h % (sizeof(palette) / sizeof(palette[0]))];
}

static int indexAt(double coordinate, double first, double step, int count)
{
    const double v = std::floor((coordinate - first) / step);
    if (v < 0.0) return 0;
    if (v >= static_cast<double>(count)) return count - 1;
    return static_cast<int>(v);
}

static QImage renderScanline(int nx, int ny, QSize size,
                             double windowX, double windowY,
                             double stepX, double stepY,
                             const QTransform &inverse,
                             SampleMode mode)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    const QPointF offset = sampleOffset(mode);
    const double firstX = (windowX - stepX) * 0.5;
    const double firstY = (windowY - stepY) * 0.5;

    for (int py = 0; py < size.height(); ++py) {
        QRgb *line = reinterpret_cast<QRgb*>(image.scanLine(py));
        for (int px = 0; px < size.width(); ++px) {
            const QPointF p = inverse.map(
                QPointF(px + offset.x(), py + offset.y()));
            const int ix = indexAt(p.x(), firstX, stepX, nx);
            const int iy = indexAt(p.y(), firstY, stepY, ny);
            line[px] = cellColor(ix, iy);
        }
    }
    return image;
}

static QImage renderPainter(int nx, int ny, QSize size,
                            double windowX, double windowY,
                            double stepX, double stepY,
                            const QTransform &transform)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(Qt::NoPen);
    painter.setTransform(transform);

    // Bottom-to-top on screen when transform has negative Y scale.
    // For this standalone example, positive Y scale means increasing iy
    // is top-to-bottom. Adjust loop direction to match production order.
    for (int iy = ny - 1; iy >= 0; --iy) {
        for (int ix = 0; ix < nx; ++ix) {
            const double x = ix * stepX, y = iy * stepY;
            QRectF windowRect(x, y, windowX, windowY);
            QRectF stepRect(x, y, stepX, stepY);
            stepRect.moveCenter(windowRect.center());
            painter.setBrush(QColor::fromRgb(cellColor(ix, iy)));
            painter.drawRect(stepRect);
        }
    }
    painter.end();
    return image;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const int nx = 600, ny = 240;
    const QSize imageSize(60, 24);
    const double stepX = 0.06, stepY = 0.06;
    const double windowX = 0.06, windowY = 0.06;

    QTransform transform;
    transform.scale(double(imageSize.width()) / (nx * stepX),
                    double(imageSize.height()) / (ny * stepY));

    bool ok = false;
    const QTransform inverse = transform.inverted(&ok);
    if (!ok) {
        qCritical() << "Transform is not invertible";
        return 1;
    }

    QElapsedTimer timer;
    timer.start();
    const QImage reference = renderPainter(
        nx, ny, imageSize, windowX, windowY, stepX, stepY, transform);
    qDebug() << "QPainter elapsed ms:" << timer.elapsed();
    reference.save("candy_painter.png");

    const SampleMode modes[] = {
        SampleMode::Center, SampleMode::TopLeft, SampleMode::Top,
        SampleMode::TopRight, SampleMode::Left, SampleMode::Right,
        SampleMode::BottomLeft, SampleMode::Bottom, SampleMode::BottomRight
    };

    for (SampleMode mode : modes) {
        timer.restart();
        const QImage image = renderScanline(
            nx, ny, imageSize, windowX, windowY,
            stepX, stepY, inverse, mode);
        const qint64 elapsed = timer.elapsed();
        quint64 matched = 0, mismatched = 0, untouched = 0;
        for (int y = 0; y < image.height(); ++y) {
            const QRgb *a = reinterpret_cast<const QRgb*>(reference.constScanLine(y));
            const QRgb *b = reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for (int x = 0; x < image.width(); ++x) {
                if (qAlpha(a[x]) == 0) ++untouched;
                if (a[x] == b[x]) ++matched;
                else ++mismatched;
            }
        }
        const double rate = 100.0 * matched / (matched + mismatched);
        const QString file = QString("candy_%1.png").arg(modeName(mode));
        image.save(file);
        qDebug().noquote() << modeName(mode)
            << "match" << matched << "mismatch" << mismatched
            << "rate(%)" << rate << "referenceTransparent" << untouched
            << "elapsed(ms)" << elapsed << "file" << file;
    }
    return 0;
}
