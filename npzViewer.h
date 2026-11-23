// npzViewer.h
// Qt 5.9 / C++11, header-only NPZ channel viewer.
// Include after the declaration of NpzMask (or any compatible mask type).
// qmake: QT += widgets
#ifndef NPZ_VIEWER_H
#define NPZ_VIEWER_H

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPointF>
#include <QDialog>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QVector>
#include <QVBoxLayout>
#include <QtGlobal>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <limits>

// Expects mask.channels, mask.height, mask.width, mask.pixels,
// and optionally mask.layerIds. Pixels must be contiguous C-order CHW uint8.
// This viewer does not take ownership of the mask: use exec() while the mask
// remains alive, or store the mask in the owning dialog.
template <typename Mask>
class NpzChannelViewer : public QDialog {
public:
    explicit NpzChannelViewer(const Mask& mask, QWidget* parent = nullptr)
        : QDialog(parent), mask_(mask), scene_(new QGraphicsScene(this)),
          view_(new QGraphicsView(scene_, this)),
          channels_(new QComboBox(this)), aaOnly_(new QCheckBox(QStringLiteral("Highlight AA (1-254)"), this)), status_(new QLabel(this)) {
        setWindowTitle(QStringLiteral("NPZ Channel Viewer"));
        resize(900, 650);

        QVBoxLayout* layout = new QVBoxLayout(this);
        QHBoxLayout* controls = new QHBoxLayout();
        controls->addWidget(new QLabel(QStringLiteral("Channel:"), this));
        controls->addWidget(channels_, 1);
        controls->addWidget(aaOnly_);
        QPushButton* fit = new QPushButton(QStringLiteral("Fit"), this);
        controls->addWidget(fit);
        layout->addLayout(controls);
        layout->addWidget(view_, 1);
        layout->addWidget(status_);

        view_->setDragMode(QGraphicsView::ScrollHandDrag);
        view_->setRenderHint(QPainter::SmoothPixmapTransform, false);
        view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        view_->viewport()->setMouseTracking(true);
        view_->viewport()->installEventFilter(this);

        const size_t count = mask_.channels;
        for (size_t i = 0; i < count && i < static_cast<size_t>(INT_MAX); ++i) {
            QString label = QStringLiteral("Channel %1").arg(static_cast<qulonglong>(i));
            channels_->addItem(label);
        }

        QObject::connect(channels_,
                         static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                         this, [this](int channel) { displayChannel(channel); });
        QObject::connect(fit, &QPushButton::clicked, this, [this]() {
            if (!scene_->sceneRect().isEmpty())
                view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
        });

        QObject::connect(aaOnly_, &QCheckBox::toggled, this, [this](bool) {
            displayChannel(channels_->currentIndex(), false);
        });

        displayChannel(0);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == view_->viewport()) {
            if (event->type() == QEvent::MouseMove) {
                QMouseEvent* mouse = static_cast<QMouseEvent*>(event);
                const QPointF point = view_->mapToScene(mouse->pos());
                const int channel = channels_->currentIndex();
                if (channel >= 0 && point.x() >= 0 && point.y() >= 0 &&
                    point.x() < static_cast<qreal>(mask_.width) &&
                    point.y() < static_cast<qreal>(mask_.height)) {
                    const size_t x = static_cast<size_t>(point.x());
                    const size_t y = static_cast<size_t>(point.y());
                    const size_t offset =
                        (static_cast<size_t>(channel) * mask_.height + y)
                        * mask_.width + x;
                    if (offset < mask_.pixels.size()) {
                        const int value = mask_.pixels[offset];
                        status_->setText(
                            QStringLiteral("Channel %1 | X: %2 Y: %3 | Value: %4 | %5")
                            .arg(channel)
                            .arg(static_cast<qulonglong>(x))
                            .arg(static_cast<qulonglong>(y))
                            .arg(value)
                            .arg(value == 0 ? QStringLiteral("Background") :
                                 value == 255 ? QStringLiteral("Filled") :
                                                QStringLiteral("AA candidate")));
                    }
                } else {
                    status_->setText(QStringLiteral("Move cursor over image to inspect pixels"));
                }
            } else if (event->type() == QEvent::Wheel) {
                QWheelEvent* wheel = static_cast<QWheelEvent*>(event);
                const int delta = wheel->angleDelta().y();
                if (delta != 0) {
                    const qreal factor = delta > 0 ? 1.25 : 0.8;
                    const qreal current = view_->transform().m11();
                    if ((factor > 1 && current < 100.0) ||
                        (factor < 1 && current > 0.001))
                        view_->scale(factor, factor);
                    wheel->accept();
                    return true;
                }
            }
        }
        return QDialog::eventFilter(watched, event);
    }

private:
    void displayChannel(int channel, bool fit = true) {
        scene_->clear();
        const size_t w = mask_.width, h = mask_.height;
        if (channel < 0 || static_cast<size_t>(channel) >= mask_.channels ||
            !w || !h || w > static_cast<size_t>(INT_MAX) ||
            h > static_cast<size_t>(INT_MAX) ||
            w > std::numeric_limits<size_t>::max() / h) {
            status_->setText(QStringLiteral("Invalid channel or image dimensions"));
            return;
        }
        const size_t plane = w * h;
        if (mask_.channels > std::numeric_limits<size_t>::max() / plane ||
            mask_.pixels.size() < mask_.channels * plane) {
            status_->setText(QStringLiteral("Invalid NPZ pixel buffer size"));
            return;
        }

        // QImage borrows the mask buffer; QPixmap::fromImage makes a display copy.
        const uchar* data = reinterpret_cast<const uchar*>(
            mask_.pixels.data() + static_cast<size_t>(channel) * plane);
        QImage image(data, static_cast<int>(w), static_cast<int>(h),
                     static_cast<int>(w), QImage::Format_Indexed8);
        if (image.isNull()) {
            status_->setText(QStringLiteral("Cannot create QImage"));
            return;
        }
        QVector<QRgb> table(256);
        for (int i = 0; i < 256; ++i) {
            const int gray = 255 - i;
            // Diagnostic palette: intermediate NPZ values become red.
            table[i] = aaOnly_->isChecked() && i > 0 && i < 255
                ? qRgb(255, 0, 0) : qRgb(gray, gray, gray);
        }
        image.setColorTable(table);
        QPixmap pixmap = QPixmap::fromImage(image);
        if (pixmap.isNull()) {
            status_->setText(QStringLiteral("Cannot create QPixmap; image may be too large"));
            return;
        }
        scene_->addPixmap(pixmap);
        scene_->setSceneRect(QRectF(0, 0, static_cast<qreal>(w),
                                    static_cast<qreal>(h)));
        if (fit) {
            view_->resetTransform();
            view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
        }
        status_->setText(QStringLiteral("Channel %1  |  %2 x %3  |  0=white, 255=black")
                         .arg(channel).arg(static_cast<qulonglong>(w))
                         .arg(static_cast<qulonglong>(h)));
    }

    const Mask& mask_;
    QGraphicsScene* scene_;
    QGraphicsView* view_;
    QComboBox* channels_;
    QCheckBox* aaOnly_;
    QLabel* status_;
};

// Call from the existing Parse button after loadNpzMask() succeeds:
//   NpzMask mask = loadNpzMask(filePath.toLocal8Bit().constData());
//   showNpzViewer(mask, this);
// This modal call keeps the local mask alive until the viewer closes.
template <typename Mask>
inline void showNpzViewer(const Mask& mask, QWidget* parent = nullptr) {
    NpzChannelViewer<Mask> viewer(mask, parent);
    viewer.exec();
}

#endif // NPZ_VIEWER_H
