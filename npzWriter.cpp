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
        return -1; // invalid input argument or raster scale
    const double hFloat = clipUm / pixelUm;
    if (!std::isfinite(hFloat) || hFloat < 1 || hFloat > INT32_MAX)
        return -2; // size overflow or ZIP32 size limit
    const int h = static_cast<int>(hFloat); // matches Python int(...)
    int64_t x0d = 0, y0d = 0;
    if (!checkedTrunc((centerXUm - clipUm / 2.0) / dbuUm, x0d) ||
        !checkedTrunc((centerYUm - clipUm / 2.0) / dbuUm, y0d)) return -3; // coordinate conversion out of range
    const double scale = dbuUm / pixelUm * SHIFT_MULT;
    if (!std::isfinite(scale) || scale <= 0)
        return -1; // invalid input argument or raster scale

    result.channels = layers.size();
    result.height = result.width = static_cast<size_t>(h);
    size_t plane = 0, total = 0;
    if (!checkedMul(result.height, result.width, plane) ||
        !checkedMul(result.channels, plane, total)) return -2; // size overflow or ZIP32 size limit
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
                if (!checkedCvCoord(x, px) || !checkedCvCoord(y, py)) return -3; // coordinate conversion out of range
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
static void setLe16(QByteArray& bytes, int offset, quint16 value) {
    qToLittleEndian<quint16>(value,
        reinterpret_cast<uchar*>(bytes.data() + offset));
}
static void setLe64(QByteArray& bytes, int offset, quint64 value) {
    qToLittleEndian<quint64>(value,
        reinterpret_cast<uchar*>(bytes.data() + offset));
}
static void setLe32(QByteArray& bytes, int offset, quint32 value) {
    qToLittleEndian<quint32>(value,
        reinterpret_cast<uchar*>(bytes.data() + offset));
}
static int makeNpyHeader(const Raster& r, std::vector<uint8_t>& npyHeader) {
    size_t plane = 0, total = 0;
    if (!checkedMul(r.height, r.width, plane) ||
        !checkedMul(r.channels, plane, total) || r.pixels.size() != total)
        return -2; // Raster dimensions and pixel buffer size mismatch
    std::string header = "{'descr': '|u1', 'fortran_order': False, 'shape': (" +
        std::to_string(r.channels) + ", " + std::to_string(r.height) +
        ", " + std::to_string(r.width) + "), }";
    // NPY v1.0: 10-byte preamble + header length must be a multiple of 16.
    const size_t padding = (16 - ((10 + header.size() + 1) % 16)) % 16;
    header.append(padding, ' ');
    header.push_back('\n');
    if (header.size() > UINT16_MAX)
        return -6; // NPY header format limit
    npyHeader.clear();
    const size_t prefixSize = 10 + header.size();
    if (prefixSize > std::numeric_limits<size_t>::max() - r.pixels.size())
        return -2; // NPY buffer size overflow
    npyHeader.reserve(prefixSize);
    const uint8_t magic[] = {0x93, 'N', 'U', 'M', 'P', 'Y', 1, 0};
    npyHeader.insert(npyHeader.end(), magic, magic + sizeof(magic));
    npyHeader.push_back(static_cast<uint8_t>(header.size() & 0xff));
    npyHeader.push_back(static_cast<uint8_t>((header.size() >> 8) & 0xff));
    npyHeader.insert(npyHeader.end(), header.begin(), header.end());
    return 0;
}

// Read the NPY header and Raster pixels directly, without concatenating them.
static int deflateRaw(const std::vector<uint8_t>& npyHeader,
                      const std::vector<uint8_t>& pixels,
                      std::vector<uint8_t>& out) {
    if (npyHeader.size() > std::numeric_limits<size_t>::max() - pixels.size())
        return -2; // NPY input size overflow
    const size_t inputSize = npyHeader.size() + pixels.size();

    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return -4; // zlib initialization failed

    out.clear();
    if (inputSize <= static_cast<size_t>(std::numeric_limits<uLong>::max())) {
        const uLong upperBound = deflateBound(&zs, static_cast<uLong>(inputSize));
        if (static_cast<uint64_t>(upperBound) <=
            static_cast<uint64_t>(std::numeric_limits<size_t>::max()))
            out.reserve(static_cast<size_t>(upperBound));
    }

    uint8_t buffer[65536];
    size_t headerOffset = 0;
    size_t pixelOffset = 0;
    int status = Z_OK;
    do {
        if (zs.avail_in == 0) {
            if (headerOffset < npyHeader.size()) {
                const size_t count = std::min<size_t>(65536, npyHeader.size() - headerOffset);
                zs.next_in = const_cast<Bytef*>(npyHeader.data() + headerOffset);
                zs.avail_in = static_cast<uInt>(count);
                headerOffset += count;
            } else if (pixelOffset < pixels.size()) {
                const size_t count = std::min<size_t>(65536, pixels.size() - pixelOffset);
                zs.next_in = const_cast<Bytef*>(pixels.data() + pixelOffset);
                zs.avail_in = static_cast<uInt>(count);
                pixelOffset += count;
            }
        }
        const bool allInputConsumed = headerOffset == npyHeader.size() &&
            pixelOffset == pixels.size() && zs.avail_in == 0;
        zs.next_out = buffer;
        zs.avail_out = sizeof(buffer);
        status = deflate(&zs, allInputConsumed ? Z_FINISH : Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            deflateEnd(&zs);
            return -4; // zlib compression failed
        }
        out.insert(out.end(), buffer, buffer + sizeof(buffer) - zs.avail_out);
    } while (status != Z_STREAM_END);
    deflateEnd(&zs);
    return 0;
}

// ZIP64 is allowed only when requested and required by the actual sizes.
// A small archive keeps the ZIP32 layout for compatibility with older readers.
int saveNpz(const std::string& filename, const Raster& raster) {
    std::vector<uint8_t> npyHeader, compressed;
    int rc = makeNpyHeader(raster, npyHeader);
    if (rc < 0) return rc; // NPY construction failed
    rc = deflateRaw(npyHeader, raster.pixels, compressed);
    if (rc < 0) return rc; // DEFLATE compression failed

    const quint64 originalSize = static_cast<quint64>(npyHeader.size()) +
        static_cast<quint64>(raster.pixels.size());
    const quint64 compressedSize = static_cast<quint64>(compressed.size());
    const QByteArray entry("data.npy");
    const quint64 filenameLength = static_cast<quint64>(entry.size());
    const quint64 localBase = 30 + filenameLength;
    const quint64 centralBase = 46 + filenameLength;
    const quint64 zip32Maximum = UINT32_MAX;

    // The ZIP32 layout is selected only when all relevant sizes and
    // offsets fit. Include the archive end in the size check.
    const bool useZip64 =
        originalSize >= zip32Maximum ||
        compressedSize >= zip32Maximum ||
        compressedSize > zip32Maximum - localBase - centralBase - 22;

    const quint64 localExtraSize = useZip64 ? 20 : 0;
    const quint64 centralExtraSize = useZip64 ? 20 : 0;
    const quint64 centralOffset = localBase + localExtraSize + compressedSize;
    const quint64 centralSize = centralBase + centralExtraSize;
    if (centralOffset > std::numeric_limits<quint64>::max() - centralSize - 98)
        return -2; // ZIP archive size overflow

    quint32 crc = crc32(0L, Z_NULL, 0);
    for (size_t i = 0; i < npyHeader.size(); i += 65536) {
        const size_t count = std::min<size_t>(65536, npyHeader.size() - i);
        crc = crc32(crc, npyHeader.data() + i, static_cast<uInt>(count));
    }
    for (size_t i = 0; i < raster.pixels.size(); i += 65536) {
        const size_t count = std::min<size_t>(65536, raster.pixels.size() - i);
        crc = crc32(crc, raster.pixels.data() + i, static_cast<uInt>(count));
    }

    QByteArray local(30, '\0');
    QByteArray central(46, '\0');
    QByteArray ending(22, '\0');
    setLe32(local, 0, 0x04034b50);
    setLe16(local, 4, useZip64 ? 45 : 20);
    setLe16(local, 8, 8); // DEFLATE
    setLe32(local, 14, crc);
    setLe32(local, 18, useZip64 ? UINT32_MAX : static_cast<quint32>(compressedSize));
    setLe32(local, 22, useZip64 ? UINT32_MAX : static_cast<quint32>(originalSize));
    setLe16(local, 26, static_cast<quint16>(filenameLength));
    setLe16(local, 28, static_cast<quint16>(localExtraSize));

    setLe32(central, 0, 0x02014b50);
    setLe16(central, 4, useZip64 ? 45 : 20);
    setLe16(central, 6, useZip64 ? 45 : 20);
    setLe16(central, 10, 8); // DEFLATE
    setLe32(central, 16, crc);
    setLe32(central, 20, useZip64 ? UINT32_MAX : static_cast<quint32>(compressedSize));
    setLe32(central, 24, useZip64 ? UINT32_MAX : static_cast<quint32>(originalSize));
    setLe16(central, 28, static_cast<quint16>(filenameLength));
    setLe16(central, 30, static_cast<quint16>(centralExtraSize));

    setLe32(ending, 0, 0x06054b50);
    setLe16(ending, 8, 1);
    setLe16(ending, 10, 1);
    setLe32(ending, 12, useZip64 ? UINT32_MAX : static_cast<quint32>(centralSize));
    setLe32(ending, 16, useZip64 ? UINT32_MAX : static_cast<quint32>(centralOffset));

    QByteArray localExtra;
    QByteArray centralExtra;
    QByteArray zip64Ending;
    QByteArray zip64Locator;
    if (useZip64) {
        localExtra = QByteArray(20, '\0');
        setLe16(localExtra, 0, 0x0001); // ZIP64 extra field
        setLe16(localExtra, 2, 16);
        setLe64(localExtra, 4, originalSize);
        setLe64(localExtra, 12, compressedSize);

        centralExtra = QByteArray(20, '\0');
        setLe16(centralExtra, 0, 0x0001);
        setLe16(centralExtra, 2, 16);
        setLe64(centralExtra, 4, originalSize);
        setLe64(centralExtra, 12, compressedSize);

        zip64Ending = QByteArray(56, '\0');
        setLe32(zip64Ending, 0, 0x06064b50);
        setLe64(zip64Ending, 4, 44); // Size excluding signature and size field
        setLe16(zip64Ending, 12, 45);
        setLe16(zip64Ending, 14, 45);
        setLe64(zip64Ending, 24, 1);
        setLe64(zip64Ending, 32, 1);
        setLe64(zip64Ending, 40, centralSize);
        setLe64(zip64Ending, 48, centralOffset);

        zip64Locator = QByteArray(20, '\0');
        setLe32(zip64Locator, 0, 0x07064b50);
        setLe64(zip64Locator, 8, centralOffset + centralSize);
        setLe32(zip64Locator, 16, 1);
    }

    QSaveFile file(QString::fromStdString(filename));
    if (!file.open(QIODevice::WriteOnly))
        return -5; // File open failed

    const auto writeBytes = [&file](const char* data, size_t count) -> bool {
        return count <= static_cast<size_t>(std::numeric_limits<qint64>::max()) &&
            file.write(data, static_cast<qint64>(count)) == static_cast<qint64>(count);
    };

    if (!writeBytes(local.constData(), static_cast<size_t>(local.size())) ||
        !writeBytes(entry.constData(), static_cast<size_t>(entry.size())) ||
        !writeBytes(localExtra.constData(), static_cast<size_t>(localExtra.size())) ||
        !writeBytes(reinterpret_cast<const char*>(compressed.data()), compressed.size()) ||
        !writeBytes(central.constData(), static_cast<size_t>(central.size())) ||
        !writeBytes(entry.constData(), static_cast<size_t>(entry.size())) ||
        !writeBytes(centralExtra.constData(), static_cast<size_t>(centralExtra.size())) ||
        !writeBytes(zip64Ending.constData(), static_cast<size_t>(zip64Ending.size())) ||
        !writeBytes(zip64Locator.constData(), static_cast<size_t>(zip64Locator.size())) ||
        !writeBytes(ending.constData(), static_cast<size_t>(ending.size()))) {
        file.cancelWriting();
        return -5; // File write failed
    }
    if (!file.commit())
        return -5; // File commit failed
    return 0;
}

int clipFilename(int clipId, std::string& filename) {
    if (clipId < 0) return -1; // invalid input argument or raster scale
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
    return saveNpz(filename, raster); // propagate save status (0 or negative)
}

} // namespace clipnpz

// int rc = clipnpz::writeClip(0, layers, x_um, y_um, clip_um, pixel_um, dbu_um);
// 0=success; -1=invalid argument; -2=size; -3=coordinates;
// -4=zlib; -5=file I/O; -6=NPY/ZIP format.
// Note: OpenCV and STL may still throw internally (e.g. allocation failure).
