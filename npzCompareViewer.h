#ifndef NPZ_COMPARE_VIEWER_H
#define NPZ_COMPARE_VIEWER_H
// Include after npzParser.h. Qt 5.9 / C++11.
#include <QtWidgets>
#include <memory>
#include <cstring>
#include <limits>
#include <exception>
#include <algorithm>
#include <climits>
#include <cstdlib>
#include <stdexcept>
#include <vector>

class NpzCompareViewer : public QDialog {
    struct Stats { quint64 diff=0, onlyA=0, onlyB=0, other=0; int maxDiff=0; QRect bounds; };
    std::unique_ptr<NpzMask> a_, b_;
    QLineEdit* paths_[2];
    QGraphicsView* views_[3];
    QGraphicsScene* scenes_[3];
    QTableWidget* table_;
    QLabel* status_;
    QLabel* summary_;
    bool syncing_=false;
    int channel_=-1;
    std::vector<Stats> stats_;
    static bool valid(const NpzMask& m) {
        const size_t lim=std::numeric_limits<size_t>::max();
        return m.width && m.height && m.channels &&
            m.width<=size_t(INT_MAX) && m.height<=size_t(INT_MAX) &&
            m.channels<=size_t(INT_MAX) && m.width<=lim/m.height &&
            m.channels<=lim/(m.width*m.height) &&
            m.pixels.size()==m.channels*m.width*m.height &&
            m.width*m.height<=size_t(INT_MAX)/4;
    }
    void browse(int i) {
        QString p=QFileDialog::getOpenFileName(this,tr("Open NPZ"),paths_[i]->text(),tr("NPZ files (*.npz);;All files (*)"));
        if (!p.isEmpty()) paths_[i]->setText(p);
    }
    void compare() {
        if(paths_[0]->text().isEmpty() || paths_[1]->text().isEmpty()) return;
        try {
            std::unique_ptr<NpzMask> a(new NpzMask(loadNpzMask(QFile::encodeName(paths_[0]->text()).constData())));
            std::unique_ptr<NpzMask> b(new NpzMask(loadNpzMask(QFile::encodeName(paths_[1]->text()).constData())));
            if (!valid(*a)||!valid(*b)||a->width!=b->width||a->height!=b->height||a->channels!=b->channels)
                throw std::runtime_error("Invalid or incompatible dimensions/channel count");
            if (!a->layerIds.empty() && !b->layerIds.empty() && a->layerIds!=b->layerIds)
                throw std::runtime_error("Layer IDs/order differ");
            const size_t plane=a->width*a->height;
            std::vector<Stats> results(a->channels);
            int different=0;
            for(size_t c=0;c<a->channels;++c) {
                const auto* p=a->pixels.data()+c*plane;
                const auto* q=b->pixels.data()+c*plane;
                if(std::memcmp(p,q,plane)==0) continue;
                ++different;
                Stats& t=results[c];
                int xmin=int(a->width), ymin=int(a->height), xmax=-1,ymax=-1;
                for(size_t i=0;i<plane;++i) if(p[i]!=q[i]) {
                    ++t.diff;
                    if(p[i] && !q[i]) ++t.onlyA;
                    else if(!p[i] && q[i]) ++t.onlyB;
                    else ++t.other;
                    t.maxDiff=std::max(t.maxDiff,std::abs(int(p[i])-int(q[i])));
                    int x=int(i%a->width), y=int(i/a->width);
                    xmin=std::min(xmin,x); xmax=std::max(xmax,x);
                    ymin=std::min(ymin,y); ymax=std::max(ymax,y);
                }
                t.bounds=QRect(QPoint(xmin,ymin),QPoint(xmax,ymax));
            }
            a_=std::move(a); b_=std::move(b); stats_.swap(results);
            QSignalBlocker blocker(table_);
            table_->setRowCount(int(a_->channels));
            for(int c=0;c<table_->rowCount();++c) {
                const Stats& t=stats_[c];
                QStringList cells;
                cells<<QString::number(c)<<(t.diff?"Different":"Identical")
                     <<QString::number(t.diff)<<QString::number(100.0*double(t.diff)/double(plane),'f',6)
                     <<QString::number(t.onlyA)<<QString::number(t.onlyB)
                     <<QString::number(t.other)<<QString::number(t.maxDiff);
                for(int j=0;j<cells.size();++j) table_->setItem(c,j,new QTableWidgetItem(cells[j]));
            }
            summary_->setText(tr("Channels: %1 | Identical: %2 | Different: %3").arg(table_->rowCount()).arg(table_->rowCount()-different).arg(different));
            table_->selectRow(0); display(0);
        } catch(const std::exception& e) {
            QMessageBox::warning(this,tr("Compare failed"),QString::fromLocal8Bit(e.what()));
        }
    }
    void display(int c) {
        if(!a_||c<0||size_t(c)>=a_->channels) return;
        int w=int(a_->width),h=int(a_->height);
        size_t plane=size_t(w)*h;
        const auto* p=a_->pixels.data()+size_t(c)*plane;
        const auto* q=b_->pixels.data()+size_t(c)*plane;
        QPixmap pix[3];
        for(int k=0;k<3;++k) {
            QImage img(w,h,QImage::Format_RGB32);
            if(img.isNull()) { status_->setText(tr("Image allocation failed")); return; }
            for(int y=0;y<h;++y) {
                QRgb* row=reinterpret_cast<QRgb*>(img.scanLine(y));
                for(int x=0;x<w;++x) {
                    size_t i=size_t(y)*w+x;
                    int v=p[i],u=q[i],g=255-(k==1?u:v);
                    row[x]= k!=2||v==u ? qRgb(g,g,g) :
                        !u?qRgb(229,57,53):!v?qRgb(37,99,235):qRgb(245,158,11);
                }
            }
            pix[k]=QPixmap::fromImage(img);
            if(pix[k].isNull()) {status_->setText(tr("Pixmap allocation failed"));return;}
        }
        channel_=c; syncing_=true;
        for(int k=0;k<3;++k) {
            scenes_[k]->clear(); scenes_[k]->addPixmap(pix[k]);
            scenes_[k]->setSceneRect(0,0,w,h);
        }
        syncing_=false; fit(false);
        const Stats& t=stats_[c];
        status_->setText(t.diff?tr("Diff bounds: (%1,%2) - (%3,%4)").arg(t.bounds.left()).arg(t.bounds.top()).arg(t.bounds.right()).arg(t.bounds.bottom()):tr("Identical"));
    }
    void fit(bool diff) {
        if(channel_<0) return;
        QRectF r=scenes_[0]->sceneRect();
        if(diff && stats_[channel_].diff) r=QRectF(stats_[channel_].bounds).adjusted(-2,-2,2,2);
        syncing_=true;
        for(int k=0;k<3;++k) {views_[k]->resetTransform();views_[k]->fitInView(r,Qt::KeepAspectRatio);}
        syncing_=false;
    }
    void sync(int k) {
        if(syncing_) return;
        syncing_=true;
        QPointF center=views_[k]->mapToScene(views_[k]->viewport()->rect().center());
        for(int j=0;j<3;++j) if(j!=k) {views_[j]->setTransform(views_[k]->transform());views_[j]->centerOn(center);}
        syncing_=false;
    }
protected:
    bool eventFilter(QObject* obj,QEvent* e) override {
        for(int k=0;k<3;++k) if(obj==views_[k]->viewport() && channel_>=0) {
            if(e->type()==QEvent::Wheel) {
                auto* ev=static_cast<QWheelEvent*>(e);
                if(ev->angleDelta().y()) {
                    qreal f=ev->angleDelta().y()>0?1.25:0.8;
                    qreal z=views_[k]->transform().m11()*f;
                    if(z>=0.0001&&z<=100) {views_[k]->scale(f,f);sync(k);}
                }
                return true;
            }
            if(e->type()==QEvent::MouseMove) {
                QPointF p=views_[k]->mapToScene(static_cast<QMouseEvent*>(e)->pos());
                if(p.x()>=0&&p.y()>=0&&p.x()<a_->width&&p.y()<a_->height) {
                    size_t x=size_t(p.x()),y=size_t(p.y());
                    size_t i=(size_t(channel_)*a_->height+y)*a_->width+x;
                    int v=a_->pixels[i],u=b_->pixels[i];
                    status_->setText(tr("X: %1 Y: %2 | A: %3 B: %4 | A-B: %5").arg(qulonglong(x)).arg(qulonglong(y)).arg(v).arg(u).arg(v-u));
                }
            }
        }
        return QDialog::eventFilter(obj,e);
    }
public:
    explicit NpzCompareViewer(QWidget* parent=nullptr):QDialog(parent) {
        setWindowTitle(tr("NPZ Compare Viewer"));resize(1200,800);
        auto* layout=new QVBoxLayout(this);
        for(int i=0;i<2;++i) {
            auto* row=new QHBoxLayout;
            row->addWidget(new QLabel(i?"File B":"File A",this));
            paths_[i]=new QLineEdit(this);row->addWidget(paths_[i],1);
            auto* btn=new QPushButton(tr("Browse..."),this);row->addWidget(btn);
            connect(btn,&QPushButton::clicked,this,[this,i](){browse(i);});
            layout->addLayout(row);
        }
        auto* buttons=new QHBoxLayout;
        auto* run=new QPushButton(tr("Compare"),this);
        auto* full=new QPushButton(tr("Fit"),this);
        auto* diff=new QPushButton(tr("Fit Difference"),this);
        buttons->addWidget(run);buttons->addWidget(full);buttons->addWidget(diff);
        layout->addLayout(buttons);
        summary_=new QLabel(this);layout->addWidget(summary_);
        table_=new QTableWidget(0,8,this);
        table_->setHorizontalHeaderLabels(QStringList()<<"Channel"<<"Result"<<"Diff Pixels"<<"Diff (%)"<<"Only A"<<"Only B"<<"Other"<<"Max Diff");
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->setMaximumHeight(200);layout->addWidget(table_);
        auto* panels=new QHBoxLayout;
        for(int k=0;k<3;++k) {
            auto* col=new QVBoxLayout;
            col->addWidget(new QLabel(k==0?"File A":k==1?"File B":"Difference",this));
            scenes_[k]=new QGraphicsScene(this);
            views_[k]=new QGraphicsView(scenes_[k],this);
            views_[k]->setDragMode(QGraphicsView::ScrollHandDrag);
            views_[k]->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
            views_[k]->viewport()->setMouseTracking(true);
            col->addWidget(views_[k]);panels->addLayout(col,1);
        }
        for(int k=0;k<3;++k) {
            views_[k]->viewport()->installEventFilter(this);
            connect(views_[k]->horizontalScrollBar(),&QScrollBar::valueChanged,this,[this,k](int){if(channel_>=0)sync(k);});
            connect(views_[k]->verticalScrollBar(),&QScrollBar::valueChanged,this,[this,k](int){if(channel_>=0)sync(k);});
        }
        layout->addLayout(panels,1);
        layout->addWidget(new QLabel(tr("Red: only A | Blue: only B | Orange: unequal nonzero | Gray: equal"),this));
        status_=new QLabel(this);layout->addWidget(status_);
        connect(run,&QPushButton::clicked,this,[this](){compare();});
        connect(full,&QPushButton::clicked,this,[this](){fit(false);});
        connect(diff,&QPushButton::clicked,this,[this](){fit(true);});
        connect(table_,&QTableWidget::currentCellChanged,this,[this](int r,int,int,int){display(r);});
    }
};
inline NpzCompareViewer* openNpzCompareViewer(QWidget* parent=nullptr) {
    auto* v=new NpzCompareViewer(parent);
    v->setAttribute(Qt::WA_DeleteOnClose);
    v->show();
    return v;
}
#endif
