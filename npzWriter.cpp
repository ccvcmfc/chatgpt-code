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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
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

static size_t checkedMul(size_t a, size_t b) {
    if (!(b == 0 || a <= std::numeric_limits<size_t>::max() / b))
        throw std::runtime_error("Size overflow");
    return a * b;
}
static int checkedCvCoord(double x) {
    if (!(std::isfinite(x) &&
            x >= static_cast<double>(std::numeric_limits<int32_t>::min()) &&
            x <= static_cast<double>(std::numeric_limits<int32_t>::max())))
        throw std::runtime_error("OpenCV fixed-point coordinate out of int32 range");
    // np.round uses round-to-nearest-even; nearbyint matches under FE_TONEAREST.
    return static_cast<int>(std::nearbyint(x));
}
static int64_t checkedTrunc(double x) {
    if (!(std::isfinite(x) &&
            x >= static_cast<double>(std::numeric_limits<int64_t>::min()) &&
            x < 9223372036854775808.0))
        throw std::runtime_error("DBU coordinate out of range");
    return static_cast<int64_t>(x); // Python int() truncates toward zero
}

struct Raster {
    size_t channels, height, width;
    std::vector<uint8_t> pixels;
};

Raster renderClip(const ClipPolygons& layers,
                  double centerXUm, double centerYUm,
                  double clipUm, double pixelUm, double dbuUm) {
    if (!(std::isfinite(clipUm) && std::isfinite(pixelUm) &&
            std::isfinite(dbuUm) && std::isfinite(centerXUm) &&
            std::isfinite(centerYUm) &&
            clipUm > 0 && pixelUm > 0 && dbuUm > 0))
        throw std::runtime_error("Invalid geometry");
    const double hFloat = clipUm / pixelUm;
    if (!(hFloat >= 1 && hFloat <= static_cast<double>(INT32_MAX)))
        throw std::runtime_error("Invalid clip dimensions");
    const int h = static_cast<int>(hFloat); // matches Python int(...)
    const int64_t x0d = checkedTrunc((centerXUm - clipUm / 2.0) / dbuUm);
    const int64_t y0d = checkedTrunc((centerYUm - clipUm / 2.0) / dbuUm);
    const double scale = dbuUm / pixelUm * SHIFT_MULT;

    Raster result;
    result.channels = layers.size();
    result.height = result.width = static_cast<size_t>(h);
    const size_t plane = checkedMul(result.height, result.width);
    result.pixels.resize(checkedMul(result.channels, plane), 0);

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
                pts.push_back(cv::Point(checkedCvCoord(x), checkedCvCoord(y)));
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
    return result;
}

// Patch little-endian numeric fields in a fixed binary header.
static void setLe16(QByteArray& bytes, int offset, quint16 value) {
    qToLittleEndian<quint16>(value,
        reinterpret_cast<uchar*>(bytes.data() + offset));
}
static void setLe32(QByteArray& bytes, int offset, quint32 value) {
    qToLittleEndian<quint32>(value,
        reinterpret_cast<uchar*>(bytes.data() + offset));
}
static uint32_t as32(size_t n) {
    if (!(n <= UINT32_MAX))
        throw std::runtime_error("ZIP32 limit exceeded; ZIP64 not implemented");
    return static_cast<uint32_t>(n);
}
static std::vector<uint8_t> makeNpy(const Raster& r) {
    if (!(r.pixels.size() == checkedMul(r.channels,
            checkedMul(r.height, r.width))))
        throw std::runtime_error("Raster buffer size mismatch");
    std::string header = "{'descr': '|u1', 'fortran_order': False, 'shape': (" +
        std::to_string(r.channels) + ", " + std::to_string(r.height) +
        ", " + std::to_string(r.width) + "), }";
    // NPY v1.0: 10-byte preamble + header length must be a multiple of 16.
    const size_t padding = (16 - ((10 + header.size() + 1) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');
    if (!(header.size() <= UINT16_MAX))
        throw std::runtime_error("NPY v1 header too large");
    std::vector<uint8_t> npy;
    npy.reserve(10 + header.size() + r.pixels.size());
    const uint8_t magic[] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
    npy.insert(npy.end(), magic, magic + sizeof(magic));
    npy.push_back(static_cast<uint8_t>(header.size() & 0xff));
    npy.push_back(static_cast<uint8_t>((header.size() >> 8) & 0xff));
    npy.insert(npy.end(), header.begin(), header.end());
    npy.insert(npy.end(), r.pixels.begin(), r.pixels.end());
    return npy;
}

static std::vector<uint8_t> deflateRaw(const std::vector<uint8_t>& src) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (!(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                         -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK))
        throw std::runtime_error("deflateInit2 failed");
    std::vector<uint8_t> out;
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
            throw std::runtime_error("Deflate failed");
        }
        out.insert(out.end(), buffer, buffer + sizeof(buffer) - zs.avail_out);
    } while (status != Z_STREAM_END);
    deflateEnd(&zs);
    return out;
}

void saveNpz(const std::string& filename, const Raster& raster) {
    const std::vector<uint8_t> npy = makeNpy(raster);
    const std::vector<uint8_t> compressed = deflateRaw(npy);
    const uint32_t originalSize = as32(npy.size());
    const uint32_t compressedSize = as32(compressed.size());
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
    const uint32_t centralOffset = as32(centralOffsetSize);
    const uint32_t centralSize = as32(central.size() + entry.size());
    if (!(centralOffsetSize + centralSize + ending.size() <= UINT32_MAX))
        throw std::runtime_error("ZIP32 archive too large");

    setLe32(ending, 12, centralSize);
    setLe32(ending, 16, centralOffset);

    std::vector<uint8_t> zip;
    zip.reserve(centralOffsetSize + centralSize + ending.size());
    const auto addBytes = [&zip](const char* data, size_t count) {
        zip.insert(zip.end(),
                   reinterpret_cast<const uint8_t*>(data),
                   reinterpret_cast<const uint8_t*>(data) + count);
    };
    addBytes(local.constData(), static_cast<size_t>(local.size()));
    addBytes(entry.data(), entry.size());
    zip.insert(zip.end(), compressed.begin(), compressed.end());
    addBytes(central.constData(), static_cast<size_t>(central.size()));
    addBytes(entry.data(), entry.size());
    addBytes(ending.constData(), static_cast<size_t>(ending.size()));

    std::ofstream file(filename.c_str(), std::ios::binary | std::ios::trunc);
    if (!(file.is_open()))
        throw std::runtime_error("Cannot open NPZ output file");
    file.write(reinterpret_cast<const char*>(zip.data()),
               static_cast<std::streamsize>(zip.size()));
    if (!(file.good()))
        throw std::runtime_error("Failed writing NPZ");
}

std::string clipFilename(int clipId) {
    if (!(clipId >= 0))
        throw std::runtime_error("clipId must be nonnegative");
    char name[64];
    std::snprintf(name, sizeof(name), "clip_%05d.npz", clipId);
    return std::string(name);
}

void writeClip(int clipId, const ClipPolygons& layers,
               double centerXUm, double centerYUm,
               double clipUm, double pixelUm, double dbuUm) {
    Raster raster = renderClip(layers, centerXUm, centerYUm,
                               clipUm, pixelUm, dbuUm);
    saveNpz(clipFilename(clipId), raster);
}

} // namespace clipnpz

// Usage (no main):
// clipnpz::ClipPolygons layers; // TODO: populate via Skipper, union per layer
// clipnpz::writeClip(0, layers, x_um, y_um, clip_um, pixel_um, dbu_um);
// Creates clip_00000.npz with data.npy of shape (layers.size(), H, H).
// LIMITATIONS: ZIP32 (each .npy and compressed member < 4GiB),
// memory-buffered compression, no holes/interior contours in Polygon type.
