#include <QImage>
#include <QString>
#include <memory>
#include <QSaveFile>
#include <QByteArray>
#include <QtEndian>
#include <zlib.h>
#include <algorithm>
#include <cstring>
#include <new>

class NpzWriter {
private:
static constexpr quint64 Limit = 0xffffffffULL; // reserved ZIP64 sentinel
static constexpr int LocalSize = 38; // 30 + strlen("data.npy")
static constexpr int CentralSize = 54;
static constexpr int EndSize = 22;
static constexpr quint64 MaxCompressed = Limit - 1 - LocalSize - CentralSize - EndSize;
static void le16(QByteArray& b, int i, quint16 v) {
    qToLittleEndian<quint16>(v, reinterpret_cast<uchar*>(b.data()+i));
}
static void le32(QByteArray& b, int i, quint32 v) {
    qToLittleEndian<quint32>(v, reinterpret_cast<uchar*>(b.data()+i));
}
static bool writeAll(QSaveFile& f, const char* p, qint64 n) {
    while (n > 0) {
        const qint64 k = f.write(p,n);
        if (k <= 0) return false;
        p += k; n -= k;
    }
    return true;
}
static QByteArray npyHeader(int c,int h,int w) {
    QByteArray d = "{'descr': '|u1', 'fortran_order': False, 'shape': (" +
        QByteArray::number(c) + ", " + QByteArray::number(h) + ", " +
        QByteArray::number(w) + "), }";
    d.append(QByteArray((16-(10+d.size()+1)%16)%16,' '));
    d.append('\n');
    QByteArray b("\x93NUMPY\x01\x00",8);
    b.append(char(d.size() & 255));
    b.append(char((d.size() >> 8) & 255));
    b.append(d);
    return b;
}

struct State {
    QSaveFile file;
    z_stream z;
    bool initialized;
    int c,h,w,count;
    quint64 raw,compressed;
    quint32 crc;
    unsigned char out[65536];
    unsigned char row[65536];
    explicit State(const QString& path)
        : file(path), initialized(false), c(0),h(0),w(0),count(0),
          raw(0),compressed(0),crc(crc32(0L,Z_NULL,0)) {
        std::memset(&z,0,sizeof(z));
        file.setDirectWriteFallback(false);
    }
    ~State() {
        if (initialized) deflateEnd(&z);
        if (file.isOpen()) file.cancelWriting();
    }
    int feed(const unsigned char* p, unsigned int n, bool finish=false) {
        if (n) {
            if (raw + n >= Limit) return -2;
            raw += n;
            crc = crc32(crc,p,n);
        }
        z.next_in = const_cast<Bytef*>(p);
        z.avail_in = n;
        do {
            z.next_out = out;
            z.avail_out = sizeof(out);
            const int rc = deflate(&z,finish ? Z_FINISH : Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END) return -4;
            const unsigned int k = sizeof(out)-z.avail_out;
            if (compressed + k > MaxCompressed) return -2;
            if (!writeAll(file,reinterpret_cast<const char*>(out),k)) return -5;
            compressed += k;
            if (rc == Z_STREAM_END) return finish ? 0 : -4;
        } while (finish || z.avail_in != 0);
        return 0;
    }
};

std::unique_ptr<State> s_;
public:
NpzWriter() = default;
~NpzWriter() { cancel(); }
NpzWriter(const NpzWriter&) = delete;
NpzWriter& operator=(const NpzWriter&) = delete;
void cancel() { s_.reset(); }

int begin(const QString& path,int channels,int height,int width) {
    cancel();
    if (path.isEmpty() || channels<=0 || height<=0 || width<=0) return -1;
    try {
        const QByteArray header = npyHeader(channels,height,width);
        const quint64 plane = quint64(height)*quint64(width);
        if (plane > (Limit-1-quint64(header.size()))/quint64(channels))
            return -2;
        s_.reset(new State(path));
        s_->c=channels; s_->h=height; s_->w=width;
        if (!s_->file.open(QIODevice::WriteOnly)) { cancel(); return -5; }
        const QByteArray placeholder(LocalSize,'\0');
        if (!writeAll(s_->file,placeholder.constData(),placeholder.size())) {
            cancel(); return -5;
        }
        if (deflateInit2(&s_->z,Z_DEFAULT_COMPRESSION,Z_DEFLATED,
                         -MAX_WBITS,8,Z_DEFAULT_STRATEGY)!=Z_OK) {
            cancel(); return -4;
        }
        s_->initialized=true;
        const int rc=s_->feed(reinterpret_cast<const unsigned char*>(header.constData()),
                              static_cast<unsigned int>(header.size()));
        if (rc) cancel();
        return rc;
    } catch (const std::bad_alloc&) { cancel(); return -7; }
}

int append(const QImage& image, bool saveAA = false) {
    if (!s_) return -1;
    if (image.isNull() || image.format()!=QImage::Format_RGB32 ||
        image.width()!=s_->w || image.height()!=s_->h || s_->count>=s_->c) {
        cancel(); return -1;
    }
    for (int y=0;y<s_->h;++y) {
        const QRgb* src=reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for (int x=0;x<s_->w;) {
            const int n=std::min(65536,s_->w-x);
            if (saveAA) {
                for (int i=0;i<n;++i)
                    s_->row[i]=static_cast<unsigned char>(qRed(src[x+i]));
            } else {
                for (int i=0;i<n;++i)
                    s_->row[i]=(src[x+i]&0x00ffffffU) ? 255 : 0;
            }
            const int rc=s_->feed(s_->row,static_cast<unsigned int>(n));
            if (rc) { cancel(); return rc; }
            x+=n;
        }
    }
    ++s_->count;
    return 0;
}

int end() {
    if (!s_) return -1;
    if (s_->count!=s_->c) { cancel(); return -1; }
    try {
        const int rc=s_->feed(Z_NULL,0,true);
        if (rc) { cancel(); return rc; }
        deflateEnd(&s_->z); s_->initialized=false;
        const quint32 offset=static_cast<quint32>(LocalSize+s_->compressed);
        QByteArray local(30,'\0'), central(46,'\0'), ending(22,'\0');
        const QByteArray name("data.npy");
        le32(local,0,0x04034b50); le16(local,4,20); le16(local,8,8);
        le32(local,14,s_->crc); le32(local,18,quint32(s_->compressed));
        le32(local,22,quint32(s_->raw)); le16(local,26,8);
        le32(central,0,0x02014b50); le16(central,4,20);
        le16(central,6,20); le16(central,10,8);
        le32(central,16,s_->crc); le32(central,20,quint32(s_->compressed));
        le32(central,24,quint32(s_->raw)); le16(central,28,8);
        le32(ending,0,0x06054b50); le16(ending,8,1); le16(ending,10,1);
        le32(ending,12,CentralSize); le32(ending,16,offset);
        local+=name; central+=name;
        if (!writeAll(s_->file,central.constData(),central.size()) ||
            !writeAll(s_->file,ending.constData(),ending.size()) ||
            !s_->file.seek(0) ||
            !writeAll(s_->file,local.constData(),local.size())) {
            cancel(); return -5;
        }
        const bool ok=s_->file.commit();
        cancel();
        return ok ? 0 : -5;
    } catch (const std::bad_alloc&) { cancel(); return -7; }
}
};
// 0 success; -1 invalid input/state; -2 ZIP32 overflow;
// -4 compression; -5 file I/O; -7 allocation.
