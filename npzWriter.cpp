// npzWriter.cpp - C++11 / OpenCV / zlib, Qt 5.9 compatible.
// Build: link OpenCV core + imgproc and zlib (-lz).
// Input polygons are ALREADY collected and geometrically unioned per layer.
// Polygon coordinates are DBU units, matching KLayout's integer coordinates.
// Output: clip_%05d.npz containing data.npy, uint8 (C,H,W), values 0/255.
// No Skipper dependency here; its integration is intentionally deferred.

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <zlib.h>
#include <QByteArray>
#include <QtEndian>
#include <QSaveFile>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace clipnpz {

typedef std::vector<cv::Point2d> Polygon;
typedef std::vector<Polygon> LayerPolygons;
typedef std::vector<LayerPolygons> ClipPolygons;

static const int SHIFT_BITS = 5;
static const int SHIFT_MULT = 1 << SHIFT_BITS;

static bool checkedMul(size_t a, size_t b, size_t& out) {
    if (b != 0 && a > std::numeric_limits<size_t>::max() / b)
        return false;
    out = a * b;
    return true;
}
static bool checkedCvCoord(double x, int& out) {
    if (!(std::isfinite(x) &&
            x >= static_cast<double>(std::numeric_limits<int32_t>::min()) &&
            x <= static_cast<double>(std::numeric_limits<int32_t>::max())))
        return false;
    // np.round uses round-to-nearest-even; nearbyint matches under FE_TONEAREST.
    const double rounded = std::nearbyint(x);
    if (rounded < static_cast<double>(INT32_MIN) ||
        rounded > static_cast<double>(INT32_MAX))
        return false;
    out = static_cast<int>(rounded);
    return true;
}
static bool checkedTrunc(double x, int64_t& out) {
    if (!(std::isfinite(x) &&
            x >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
            x < 9223372036854775808.0))
        return false;
    out = static_cast<int64_t>(x);
    return true; // Python int() truncates toward zero
}

struct Raster {
    size_t channels, height, width;
    std::vector<uint8_t> pixels;
};

int renderClip(const ClipPolygons& layers,
                  double centerXUm, double centerYUm,
                  double clipUm, double pixelUm, double dbuUm,
               Raster& result) {
    if (!(std::isfinite(clipUm) && std::isfinite(pixelUm) &&
            std::isfinite(dbuUm) && std::isfinite(centerXUm) &&
            std::isfinite(centerYUm) &&
            clipUm > 0 && pixelUm > 0 && dbuUm > 0))
        return -1;
    const double hFloat = clipUm / pixelUm;
    if (!(hFloat >= 1 && hFloat <= static_cast<double>(INT32_MAX)))
        return -1;
    const int h = static_cast<int>(hFloat); // matches Python int(...)
    int64_t x0d = 0, y0d = 0;
    if (!checkedTrunc((centerXUm - clipUm / 2.0) / dbuUm, x0d) ||
        !checkedTrunc((centerYUm - clipUm / 2.0) / dbuUm, y0d)) return -3;
    const double scale = dbuUm / pixelUm * SHIFT_MULT;
    if (!std::isfinite(scale) || scale <= 0)
        return -1;

    result.channels = layers.size();
    result.height = result.width = static_cast<size_t>(h);
    size_t plane = 0, total = 0;
    if (!checkedMul(result.height, result.width, plane) ||
        !checkedMul(result.channels, plane, total)) return -2;
    result.pixels.resize(total, 0);

    for (size_t c = 0; c < layers.size(); ++c) {
        cv::Mat img(h, h, CV_8UC1, cv::Scalar(0));
        std::vector<std::vector<cv::Point> > batch;
        batch.reserve(128);

        // TODO Skipper: query polygons intersecting clip, transform boxes/paths,
        // and perform geometric UNION for each layer before filling layers[c].
        // This function assumes layers[c] is already populated and unioned.
        for (size_t p = 0; p < layers[c].size(); ++p) {
            const Polygon& poly = layers[c][p];
            if (poly.size() < 3) continue;
            std::vector<cv::Point> pts;
            pts.reserve(poly.size());
            for (size_t i = 0; i < poly.size(); ++i) {
                const double x = (poly[i].x - static_cast<double>(x0d)) * scale;
                const double y = (poly[i].y - static_cast<double>(y0d)) * scale;
                int px = 0, py = 0;
                if (!checkedCvCoord(x, px) || !checkedCvCoord(y, py)) return -3;
                pts.push_back(cv::Point(px, py));
            }
            batch.push_back(pts);
            if (batch.size() >= 128) {
                cv::fillPoly(img, batch, cv::Scalar(255), cv::LINE_AA, SHIFT_BITS);
                batch.clear();
            }
        }
        if (!batch.empty())
            cv::fillPoly(img, batch, cv::Scalar(255), cv::LINE_AA, SHIFT_BITS);

        cv::flip(img, img, 0);
        // Python: (arr > 0).astype(np.uint8) * 255
        cv::threshold(img, img, 0, 255, cv::THRESH_BINARY);
        if (img.isContinuous())
            std::memcpy(result.pixels.data() + c * plane, img.data, plane);
        else
            for (int y = 0; y < h; ++y)
                std::memcpy(result.pixels.data() + c * plane + size_t(y) * h,
                            img.ptr(y), static_cast<size_t>(h));
    }
    return 0;
}

// Patch little-endian numeric fields in a fixed binary header.
static void setLe32(QByteArray& bytes, int offset, quint32 value) {
    qToLittleEndian<quint32>(value,
        reinterpret_cast<uchar*>(bytes.data() + offset));
}
static uint32_t as32(size_t n) {
    if (n > UINT32_MAX)
        return -2;
    return static_cast<uint32_t>(n);
}
static std::vector<uint8_t> makeNpy(const Raster& r) {
    size_t plane = 0, total = 0;
    if (!checkedMul(r.height, r.width, plane) ||
        !checkedMul(r.channels, plane, total) || r.pixels.size() != total)
        return -2;
    std::string header = "{'descr': '|u1', 'fortran_order': False, 'shape': (" +
        std::to_string(r.channels) + ", " + std::to_string(r.height) +
        ", " + std::to_string(r.width) + "), }";
    // NPY v1.0: 10-byte preamble + header length must be a multiple of 16.
    const size_t padding = (16 - ((10 + header.size() + 1) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');
    if (header.size() > UINT16_MAX)
        return -6;
    std::vector<uint8_t> npy;
    const size_t prefixSize = 10 + header.size();
    if (r.pixels.size() > UINT32_MAX - prefixSize)
        return -2;
    npy.reserve(prefixSize + r.pixels.size());
    const uint8_t magic[] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
    npy.insert(npy.end(), magic, magic + sizeof(magic));
    npy.push_back(static_cast<uint8_t>(header.size() & 0xff));
    npy.push_back(static_cast<uint8_t>((header.size() >> 8) & 0xff));
    npy.insert(npy.end(), header.begin(), header.end());
    npy.insert(npy.end(), r.pixels.begin(), r.pixels.end());
    return 0;
}

static int deflateRaw(const std::vector<uint8_t>& src, std::vector<uint8_t>& out) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (!(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK))
        return -4;
    out.clear();
    uint8_t buffer[65536];
    size_t inputOffset = 0;
    int status = Z_OK;
    do {
        if (zs.avail_in == 0 && inputOffset < src.size()) {
            const size_t n = std::min<size_t>(
                src.size() - inputOffset, 65536);
            zs.next_in = const_cast<Bytef*>(src.data() + inputOffset);
            zs.avail_in = static_cast<uInt>(n);
            inputOffset += n;
        }
        const int flush = (inputOffset == src.size() && zs.avail_in == 0)
                        ? Z_FINISH : Z_NO_FLUSH;
        zs.next_out = buffer;
        zs.avail_out = sizeof(buffer);
        status = deflate(&zs, flush);
        if (status != Z_OK && status != Z_STREAM_END) {
            deflateEnd(&zs);
            return -4;
        }
        out.insert(out.end(), buffer, buffer + sizeof(buffer) - zs.avail_out);
    } while (status != Z_STREAM_END);
    deflateEnd(&zs);
    return 0;
}

int saveNpz(const std::string& filename, const Raster& raster) {
    std::vector<uint8_t> npy, compressed;
    int rc = makeNpy(raster, npy);
    if (rc < 0) return rc;
    rc = deflateRaw(npy, compressed);
    if (rc < 0) return rc;
    if (npy.size() > UINT32_MAX || compressed.size() > UINT32_MAX) return -2;
    const uint32_t originalSize = static_cast<uint32_t>(npy.size());
    const uint32_t compressedSize = static_cast<uint32_t>(compressed.size());
    const std::string entry = "data.npy";
    uint32_t crc = crc32(0L, Z_NULL, 0);
    for (size_t i = 0; i < npy.size(); i += 65536) {
        const size_t n = std::min<size_t>(65536, npy.size() - i);
        crc = crc32(crc, npy.data() + i, static_cast<uInt>(n));
    }

    // ZIP32 single-entry archive. Constant header fields are defined once;
    // only CRC, sizes, and offsets are patched below.
    static const char localTemplate[30] = {
        0x50, 0x4b, 0x03, 0x04,  20, 0,  0, 0,  8, 0,
        0, 0,  0, 0,           0, 0, 0, 0,  0, 0, 0, 0,
        0, 0, 0, 0,            8, 0,  0, 0
    };
    static const char centralTemplate[46] = {
        0x50, 0x4b, 0x01, 0x02,  20, 0,  20, 0,  0, 0,
        8, 0,  0, 0,  0, 0,     0, 0, 0, 0,  0, 0, 0, 0,
        0, 0, 0, 0,             8, 0,  0, 0,  0, 0,
        0, 0,  0, 0,            0, 0, 0, 0,  0, 0, 0, 0
    };
    static const char endTemplate[22] = {
        0x50, 0x4b, 0x05, 0x06,  0, 0,  0, 0,
        1, 0,  1, 0,           0, 0, 0, 0,
        0, 0, 0, 0,            0, 0
    };

    QByteArray local(localTemplate, sizeof(localTemplate));
    QByteArray central(centralTemplate, sizeof(centralTemplate));
    QByteArray ending(endTemplate, sizeof(endTemplate));

    setLe32(local, 14, crc);
    setLe32(local, 18, compressedSize);
    setLe32(local, 22, originalSize);

    setLe32(central, 16, crc);
    setLe32(central, 20, compressedSize);
    setLe32(central, 24, originalSize);

    const size_t centralOffsetSize =
        local.size() + entry.size() + compressed.size();
    if (centralOffsetSize > UINT32_MAX || central.size() + entry.size() > UINT32_MAX) return -2;
    const uint32_t centralOffset = static_cast<uint32_t>(centralOffsetSize);
    const uint32_t centralSize = static_cast<uint32_t>(central.size() + entry.size());
    if (centralOffsetSize + centralSize + ending.size() > UINT32_MAX)
        return -2;

    setLe32(ending, 12, centralSize);
    setLe32(ending, 16, centralOffset);

    // Write directly to a temporary file and atomically commit on success.
    // Avoid a second full-size ZIP buffer in memory.
    QSaveFile file(QString::fromStdString(filename));
    if (!file.open(QIODevice::WriteOnly))
        return -5;

    const auto writeBytes = [&file](const char* data, size_t count) -> bool {
        return count <= static_cast<size_t>(std::numeric_limits<qint64>::max()) &&
            file.write(data, static_cast<qint64>(count)) == static_cast<qint64>(count);
    };

    if (!writeBytes(local.constData(), static_cast<size_t>(local.size())) ||
        !writeBytes(entry.data(), entry.size()) ||
        !writeBytes(reinterpret_cast<const char*>(compressed.data()), compressed.size()) ||
        !writeBytes(central.constData(), static_cast<size_t>(central.size())) ||
        !writeBytes(entry.data(), entry.size()) ||
        !writeBytes(ending.constData(), static_cast<size_t>(ending.size()))) {
        file.cancelWriting();
        return -5;
    }
    if (!file.commit()) return -5;
    return 0;
}

int clipFilename(int clipId, std::string& filename) {
    if (clipId < 0) return -1;
    char name[64];
    std::snprintf(name, sizeof(name), "clip_%05d.npz", clipId);
    filename = name;
    return 0;
}

int writeClip(int clipId, const ClipPolygons& layers,
              double centerXUm, double centerYUm,
              double clipUm, double pixelUm, double dbuUm) {
    std::string filename;
    int rc = clipFilename(clipId, filename);
    if (rc < 0) return rc;
    Raster raster;
    rc = renderClip(layers, centerXUm, centerYUm, clipUm, pixelUm, dbuUm, raster);
    if (rc < 0) return rc;
    return saveNpz(filename, raster);
}

} // namespace clipnpz

// int rc = clipnpz::writeClip(0, layers, x_um, y_um, clip_um, pixel_um, dbu_um);
// 0=success; -1=invalid argument; -2=size; -3=coordinates;
// -4=zlib; -5=file I/O; -6=NPY/ZIP format.
// Note: OpenCV and STL may still throw internally (e.g. allocation failure).
