// npzViewer.h
// Qt 5.9 / C++11, header-only NPZ channel viewer.
// Include after the declaration of NpzMask (or any compatible mask type).
// qmake: QT += widgets
#ifndef NPZ_VIEWER_H
#define NPZ_VIEWER_H

#include <QComboBox>
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
          channels_(new QComboBox(this)), status_(new QLabel(this)) {
        setWindowTitle(QStringLiteral("NPZ Channel Viewer"));
        resize(900, 650);

        QVBoxLayout* layout = new QVBoxLayout(this);
        QHBoxLayout* controls = new QHBoxLayout();
        controls->addWidget(new QLabel(QStringLiteral("Channel:"), this));
        controls->addWidget(channels_, 1);
        QPushButton* fit = new QPushButton(QStringLiteral("Fit"), this);
        controls->addWidget(fit);
        layout->addLayout(controls);
        layout->addWidget(view_, 1);
        layout->addWidget(status_);

        view_->setDragMode(QGraphicsView::ScrollHandDrag);
        view_->setRenderHint(QPainter::SmoothPixmapTransform, false);
        view_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);

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

        displayChannel(0);
    }

private:
    void displayChannel(int channel) {
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
            table[i] = qRgb(gray, gray, gray);
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
        view_->resetTransform();
        view_->fitInView(scene_->sceneRect(), Qt::KeepAspectRatio);
        status_->setText(QStringLiteral("Channel %1  |  %2 x %3  |  0=white, 255=black")
                         .arg(channel).arg(static_cast<qulonglong>(w))
                         .arg(static_cast<qulonglong>(h)));
    }

    const Mask& mask_;
    QGraphicsScene* scene_;
    QGraphicsView* view_;
    QComboBox* channels_;
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
