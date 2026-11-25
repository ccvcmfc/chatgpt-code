// npzViewer.h
// Qt 5.9 / C++11, header-only NPZ channel viewer.
// Include after npzParser.h (NpzMask and loadNpzMask declarations).
// qmake: QT += widgets
#ifndef NPZ_VIEWER_H
#define NPZ_VIEWER_H

#include <QCheckBox>
#include <QFileDialog>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QTimer>
#include <exception>
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
#include <memory>
#include <utility>

// Expects mask.channels, mask.height, mask.width, mask.pixels,
// and optionally mask.layerIds. Pixels must be contiguous C-order CHW uint8.
// This viewer does not take ownership of the mask: use exec() while the mask
// remains alive, or store the mask in the owning dialog.
template <typename Mask>
class NpzChannelViewer : public QDialog {
public:
    explicit NpzChannelViewer(QWidget* parent = nullptr)
        : NpzChannelViewer(std::shared_ptr<const Mask>(), nullptr, parent) {}

    explicit NpzChannelViewer(const Mask& mask, QWidget* parent = nullptr)
        : NpzChannelViewer(std::shared_ptr<const Mask>(), &mask, parent) {}

    explicit NpzChannelViewer(std::shared_ptr<const Mask> mask,
                              QWidget* parent = nullptr)
        : NpzChannelViewer(mask, mask.get(), parent) {}

private:
    NpzChannelViewer(std::shared_ptr<const Mask> ownedMask, const Mask* mask,
                     QWidget* parent)
        : QDialog(parent), ownedMask_(std::move(ownedMask)), mask_(mask), scene_(new QGraphicsScene(this)),
          view_(new QGraphicsView(scene_, this)),
          channels_(new QComboBox(this)), aaOnly_(new QCheckBox(QStringLiteral("Highlight AA (1-254)"), this)), status_(new QLabel(this)) {
        setWindowTitle(QStringLiteral("NPZ Channel Viewer"));
        resize(900, 650);

        QVBoxLayout* layout = new QVBoxLayout(this);
        QHBoxLayout* files = new QHBoxLayout();
        path_ = new QLineEdit(this);
        path_->setPlaceholderText(QStringLiteral("Select an NPZ file"));
        QPushButton* browse = new QPushButton(QStringLiteral("Browse..."), this);
        QPushButton* load = new QPushButton(QStringLiteral("Load"), this);
        files->addWidget(path_, 1);
        files->addWidget(browse);
        files->addWidget(load);
        layout->addLayout(files);
        QObject::connect(browse, &QPushButton::clicked, this, [this]() { chooseFile(); });
        QObject::connect(load, &QPushButton::clicked, this, [this]() { loadFile(path_->text()); });
        QObject::connect(path_, &QLineEdit::returnPressed, this, [this]() { loadFile(path_->text()); });
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

        refreshChannels();
        if (!mask_)
            QTimer::singleShot(0, this, [this]() { chooseFile(); });
    }

private:
    void chooseFile() {
        const QString file = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open NPZ"), path_->text(),
            QStringLiteral("NPZ files (*.npz);;All files (*)"));
        if (!file.isEmpty()) loadFile(file);
    }

    void loadFile(const QString& file) {
        if (file.isEmpty()) return;
        try {
            std::shared_ptr<const Mask> next(
                new Mask(loadNpzMask(QFile::encodeName(file).constData())));
            ownedMask_ = std::move(next);
            mask_ = ownedMask_.get();
            path_->setText(file);
            setWindowTitle(QStringLiteral("NPZ Channel Viewer - %1").arg(file));
            refreshChannels();
        } catch (const std::exception& e) {
            QMessageBox::warning(this, QStringLiteral("NPZ load failed"),
                                 QString::fromLocal8Bit(e.what()));
        }
    }

    void refreshChannels() {
        const QSignalBlocker blocker(channels_);
        channels_->clear();
        if (mask_) {
            for (size_t i = 0; i < mask_->channels && i < static_cast<size_t>(INT_MAX); ++i)
                channels_->addItem(QStringLiteral("Channel %1").arg(static_cast<qulonglong>(i)));
        }
        displayChannel(channels_->currentIndex());
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == view_->viewport() && mask_) {
            if (event->type() == QEvent::MouseMove) {
                QMouseEvent* mouse = static_cast<QMouseEvent*>(event);
                const QPointF point = view_->mapToScene(mouse->pos());
                const int channel = channels_->currentIndex();
                if (channel >= 0 && point.x() >= 0 && point.y() >= 0 &&
                    point.x() < static_cast<qreal>(mask_->width) &&
                    point.y() < static_cast<qreal>(mask_->height)) {
                    const size_t x = static_cast<size_t>(point.x());
                    const size_t y = static_cast<size_t>(point.y());
                    const size_t offset =
                        (static_cast<size_t>(channel) * mask_->height + y)
                        * mask_->width + x;
                    if (offset < mask_->pixels.size()) {
                        const int value = mask_->pixels[offset];
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
        if (!mask_) {
            scene_->setSceneRect(QRectF());
            status_->setText(QStringLiteral("Select an NPZ file"));
            return;
        }
        const size_t w = mask_->width, h = mask_->height;
        if (channel < 0 || static_cast<size_t>(channel) >= mask_->channels ||
            !w || !h || w > static_cast<size_t>(INT_MAX) ||
            h > static_cast<size_t>(INT_MAX) ||
            w > std::numeric_limits<size_t>::max() / h) {
            status_->setText(QStringLiteral("Invalid channel or image dimensions"));
            return;
        }
        const size_t plane = w * h;
        if (mask_->channels > std::numeric_limits<size_t>::max() / plane ||
            mask_->pixels.size() < mask_->channels * plane) {
            status_->setText(QStringLiteral("Invalid NPZ pixel buffer size"));
            return;
        }

        // QImage borrows the mask buffer; QPixmap::fromImage makes a display copy.
        const uchar* data = reinterpret_cast<const uchar*>(
            mask_->pixels.data() + static_cast<size_t>(channel) * plane);
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

    std::shared_ptr<const Mask> ownedMask_;
    const Mask* mask_;
    QLineEdit* path_;
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

// Non-modal, independently closable window. Moves mask data into a viewer-owned
// shared_ptr; no additional pixel-buffer copy when called with std::move(mask).
// Caller may continue parsing and opening more NPZ files.
template <typename Mask>
inline NpzChannelViewer<Mask>* showNpzViewerMulti(
    Mask mask, QWidget* parent = nullptr) {
    std::shared_ptr<const Mask> owned(new Mask(std::move(mask)));
    NpzChannelViewer<Mask>* viewer = new NpzChannelViewer<Mask>(owned, parent);
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    viewer->setWindowFlag(Qt::Window, true);
    viewer->show();
    return viewer;
}

// Open an empty non-modal viewer; the file dialog opens automatically.
inline NpzChannelViewer<NpzMask>* showNpzViewerMulti(QWidget* parent = nullptr) {
    NpzChannelViewer<NpzMask>* viewer = new NpzChannelViewer<NpzMask>(parent);
    viewer->setAttribute(Qt::WA_DeleteOnClose);
    viewer->setWindowFlag(Qt::Window, true);
    viewer->show();
    return viewer;
}

#endif // NPZ_VIEWER_H
