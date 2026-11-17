// npzParser.cpp
// Standalone C++11 NPZ reader, Qt 5.9 compatible.
// Only dependency: zlib (qmake: LIBS += -lz).
// Auto-detects one uint8 C-order (C,H,W) array; lists all NPZ members.
// Optionally reads layer_ids.npy: little-endian signed int32/int64.
// Supports ZIP stored/deflate, ZIP64 central directory, CRC32 verification.
// Does not support encrypted or multi-disk ZIP archives.
#include <zlib.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

struct NpzMask {
    size_t channels = 0, height = 0, width = 0;
    std::vector<uint8_t> pixels;
    std::vector<int64_t> layerIds;
    uint8_t at(size_t c, size_t y, size_t x) const {
        if (c >= channels || y >= height || x >= width)
            throw std::out_of_range("Mask index out of range");
        return pixels[(c * height + y) * width + x];
    }
};

namespace {
size_t checkedMul(size_t a, size_t b) {
    if (a && b > std::numeric_limits<size_t>::max())
        throw std::runtime_error("Array dimension overflow");
    return a * b;
}
uint16_t rd16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
uint32_t rd32(const uint8_t* p) {
    return uint32_t(rd16(p)) | (uint32_t(rd16(p+2)) << 16);
}
uint64_t rd64(const uint8_t* p) {
    return uint64_t(rd32(p)) | (uint64_t(rd32(p+4)) << 32);
}
size_t checkedSize(uint64_t n) {
    if (n > std::numeric_limits<size_t>::max())
        throw std::runtime_error("ZIP entry exceeds addressable memory");
    return static_cast<size_t>(n);
}
void requireRange(size_t pos, size_t length, size_t total) {
    if (pos > total || length > total - pos)
        throw std::runtime_error("Truncated or malformed ZIP/NPY");
}
size_t checkedOffset(uint64_t n, size_t total) {
    const size_t p = checkedSize(n);
    requireRange(p, 0, total);
    return p;
}
struct ZipEntry {
    std::string name;
    uint64_t compressed = 0, uncompressed = 0, localOffset = 0;
    uint32_t crc = 0;
    uint16_t method = 0, flags = 0;
};
class ZipArchive {
public:
    explicit ZipArchive(const std::string& path) {
        std::ifstream f(path.c_str(), std::ios::binary | std::ios::ate);
        if (!f) throw std::runtime_error("Cannot open NPZ file");
        const std::streamoff end = f.tellg();
        if (end < 0 || static_cast<uint64_t>(end) >
            std::numeric_limits<size_t>::max())
            throw std::runtime_error("Invalid NPZ file size");
        bytes_.resize(static_cast<size_t>(end));
        f.seekg(0, std::ios::beg);
        if (!bytes_.empty() &&
            !f.read(reinterpret_cast<char*>(bytes_.data()),
                    static_cast<std::streamsize>(bytes_.size())))
            throw std::runtime_error("Cannot read NPZ file");
        parseDirectory();
    }
    std::vector<std::string> names() const {
        std::vector<std::string> result;
        for (const ZipEntry& e : entries_) result.push_back(e.name);
        return result;
    }
    bool has(const std::string& name) const {
        for (const ZipEntry& e : entries_)
            if (e.name == name) return true;
        return false;
    }
    std::vector<uint8_t> read(const std::string& name) const {
        for (const ZipEntry& e : entries_)
            if (e.name == name) return extract(e);
        throw std::runtime_error("Missing NPZ member: " + name);
    }
private:
    std::vector<uint8_t> bytes_;
    std::vector<ZipEntry> entries_;

    void parseDirectory() {
        const size_t n = bytes_.size();
        if (n < 22) throw std::runtime_error("ZIP end record missing");
        const size_t earliest = n > 65557 ? n - 65557 : 0;
        size_t eocd = n;
        for (size_t p = n - 22;; --p) {
            if (rd32(&bytes_[p]) == 0x06054b50 &&
                p + 22 + rd16(&bytes_[p+20]) == n) {
                eocd = p;
                break;
            }
            if (p == earliest) break;
        }
        if (eocd == n) throw std::runtime_error("ZIP EOCD not found");
        const uint8_t* end = &bytes_[eocd];
        if (rd16(end+4) || rd16(end+6) ||
            rd16(end+8) != rd16(end+10))
            throw std::runtime_error("Multi-disk ZIP unsupported");
        uint64_t count = rd16(end+10);
        uint64_t dirSize = rd32(end+12);
        uint64_t dirOffset = rd32(end+16);
        if (count == 0xffff || dirSize == 0xffffffffULL ||
            dirOffset == 0xffffffffULL) {
            if (eocd < 20 || rd32(&bytes_[eocd-20]) != 0x07064b50)
                throw std::runtime_error("ZIP64 locator missing");
            const uint8_t* locator = &bytes_[eocd-20];
            if (rd32(locator+4) || rd32(locator+16) != 1)
                throw std::runtime_error("Multi-disk ZIP64 unsupported");
            const size_t p = checkedOffset(rd64(locator+8), n);
            requireRange(p, 56, n);
            const uint8_t* z = &bytes_[p];
            if (rd32(z) != 0x06064b50 || rd64(z+4) < 44 ||
                rd32(z+16) || rd32(z+20) || rd64(z+24) != rd64(z+32))
                throw std::runtime_error("Invalid ZIP64 end record");
            count = rd64(z+32);
            dirSize = rd64(z+40);
            dirOffset = rd64(z+48);
        }
        const size_t dir = checkedOffset(dirOffset, n);
        const size_t size = checkedSize(dirSize);
        requireRange(dir, size, n);
        if (count > size / 46)
            throw std::runtime_error("Invalid ZIP directory count");
        size_t p = dir;
        for (uint64_t i = 0; i < count; ++i) {
            requireRange(p, 46, dir + size);
            const uint8_t* h = &bytes_[p];
            if (rd32(h) != 0x02014b50)
                throw std::runtime_error("Invalid ZIP central header");
            const size_t nameLen = rd16(h+28), extraLen = rd16(h+30),
                         commentLen = rd16(h+32);
            const size_t recordLen = 46 + nameLen + extraLen + commentLen;
            requireRange(p, recordLen, dir + size);
            if (rd16(h+34) != 0)
                throw std::runtime_error("Multi-disk ZIP entry unsupported");
            ZipEntry e;
            e.flags = rd16(h+8);
            e.method = rd16(h+10);
            e.crc = rd32(h+16);
            e.compressed = rd32(h+20);
            e.uncompressed = rd32(h+24);
            e.localOffset = rd32(h+42);
            e.name.assign(reinterpret_cast<const char*>(h+46), nameLen);
            if (e.compressed == 0xffffffffULL ||
                e.uncompressed == 0xffffffffULL ||
                e.localOffset == 0xffffffffULL) {
                bool found = false;
                size_t x = p + 46 + nameLen;
                const size_t extraEnd = x + extraLen;
                while (x < extraEnd) {
                    requireRange(x, 4, extraEnd);
                    const uint16_t id = rd16(&bytes_[x]);
                    const size_t len = rd16(&bytes_[x+2]);
                    x += 4;
                    requireRange(x, len, extraEnd);
                    if (id == 0x0001) {
                        found = true;
                        size_t q = x;
                        const size_t limit = x + len;
                        if (e.uncompressed == 0xffffffffULL) {
                            requireRange(q, 8, limit);
                            e.uncompressed = rd64(&bytes_[q]); q += 8;
                        }
                        if (e.compressed == 0xffffffffULL) {
                            requireRange(q, 8, limit);
                            e.compressed = rd64(&bytes_[q]); q += 8;
                        }
                        if (e.localOffset == 0xffffffffULL) {
                            requireRange(q, 8, limit);
                            e.localOffset = rd64(&bytes_[q]);
                        }
                        break;
                    }
                    x += len;
                }
                if (!found)
                    throw std::runtime_error("ZIP64 entry metadata missing");
            }
            entries_.push_back(e);
            p += recordLen;
        }
    }
    std::vector<uint8_t> extract(const ZipEntry& e) const {
        if (e.flags & 1)
            throw std::runtime_error("Encrypted ZIP entry unsupported");
        if (e.method != 0 && e.method != 8)
            throw std::runtime_error("Unsupported ZIP compression method");
        const size_t p = checkedOffset(e.localOffset, bytes_.size());
        requireRange(p, 30, bytes_.size());
        const uint8_t* h = &bytes_[p];
        if (rd32(h) != 0x04034b50 || rd16(h+8) != e.method)
            throw std::runtime_error("Invalid ZIP local header");
        const size_t dataPos = p + 30 + rd16(h+26) + rd16(h+28);
        const size_t compressed = checkedSize(e.compressed);
        const size_t uncompressed = checkedSize(e.uncompressed);
        requireRange(dataPos, compressed, bytes_.size());
        std::vector<uint8_t> out(uncompressed);
        if (e.method == 0) {
            if (compressed != uncompressed)
                throw std::runtime_error("Stored ZIP entry size mismatch");
            if (uncompressed)
                std::memcpy(out.data(), &bytes_[dataPos], uncompressed);
        } else {
            z_stream stream;
            std::memset(&stream, 0, sizeof(stream));
            if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
                throw std::runtime_error("zlib inflateInit2 failed");
            size_t inPos = 0, outPos = 0;
            int status = Z_OK;
            try {
                do {
                    if (stream.avail_in == 0 && inPos < compressed) {
                        const size_t chunk = std::min<size_t>(
                            compressed - inPos, std::numeric_limits<uInt>::max());
                        stream.next_in = const_cast<Bytef*>(
                            &bytes_[dataPos + inPos]);
                        stream.avail_in = static_cast<uInt>(chunk);
                        inPos += chunk;
                    }
                    if (stream.avail_out == 0 && outPos < uncompressed) {
                        const size_t chunk = std::min<size_t>(
                            uncompressed - outPos, std::numeric_limits<uInt>::max());
                        stream.next_out = out.data() + outPos;
                        stream.avail_out = static_cast<uInt>(chunk);
                        outPos += chunk;
                    }
                    // For empty output, use a one-byte scratch buffer to
                    // allow zlib to reach Z_STREAM_END.
                    uint8_t scratch = 0;
                    const bool scratchOutput = stream.avail_out == 0 &&
                                               outPos == uncompressed;
                    if (scratchOutput) {
                        stream.next_out = &scratch;
                        stream.avail_out = 1;
                    }
                    const uInt beforeIn = stream.avail_in;
                    const uInt beforeOut = stream.avail_out;
                    status = inflate(&stream, Z_NO_FLUSH);
                    if (scratchOutput && stream.avail_out != 1)
                        throw std::runtime_error("ZIP output exceeds expected size");
                    if (status != Z_OK && status != Z_STREAM_END)
                        throw std::runtime_error("ZIP deflate error");
                    if (status == Z_OK && beforeIn == stream.avail_in &&
                        beforeOut == stream.avail_out)
                        throw std::runtime_error("ZIP deflate stalled");
                } while (status != Z_STREAM_END);
                const uint64_t consumed = uint64_t(inPos) - stream.avail_in;
                const uint64_t produced = uint64_t(outPos) - stream.avail_out +
                    ((outPos == uncompressed && stream.next_out != nullptr &&
                      stream.avail_out <= 1) ? 0 : 0);
                // total_in/total_out are zlib's definitive counters.
                if (consumed != compressed ||
                    stream.total_out != uncompressed)
                    throw std::runtime_error("ZIP decompressed size mismatch");
                (void)produced;
            } catch (...) {
                inflateEnd(&stream);
                throw;
            }
            inflateEnd(&stream);
        }
        uLong crc = crc32(0L, Z_NULL, 0);
        size_t offset = 0;
        while (offset < out.size()) {
            const size_t chunk = std::min<size_t>(
                out.size() - offset, std::numeric_limits<uInt>::max());
            crc = crc32(crc, out.data() + offset, static_cast<uInt>(chunk));
            offset += chunk;
        }
        if (static_cast<uint32_t>(crc) != e.crc)
            throw std::runtime_error("ZIP CRC32 mismatch");
        return out;
    }
};
struct Npy {
    std::string dtype;
    bool fortran = false;
    std::vector<size_t> shape;
    std::vector<uint8_t> bytes;
};
Npy parseNpy(const std::vector<uint8_t>& raw) {
    static const uint8_t magic[] = {0x93,'N','U','M','P','Y'};
    if (raw.size() < 10 || std::memcmp(raw.data(), magic, 6) != 0)
        throw std::runtime_error("Invalid NPY signature");
    size_t start = 0, length = 0;
    if (raw[6] == 1) {
        start = 10; length = rd16(raw.data()+8);
    } else if ((raw[6] == 2 || raw[6] == 3) && raw.size() >= 12) {
        start = 12; length = rd32(raw.data()+8);
    } else {
        throw std::runtime_error("Unsupported NPY version");
    }
    requireRange(start, length, raw.size());
    const std::string header(
        reinterpret_cast<const char*>(raw.data()+start), length);
    const std::regex descr(R"re(['"]descr['"]\s*:\s*['"]([^'"]+)['"])re");
    const std::regex order(R"re(['"]fortran_order['"]\s*:\s*(True|False))re");
    const std::regex shape(R"re(['"]shape['"]\s*:\s*\(([^)]*)\))re");
    std::smatch m;
    Npy a;
    if (!std::regex_search(header, m, descr))
        throw std::runtime_error("Missing NPY dtype");
    a.dtype = m[1].str();
    if (!std::regex_search(header, m, order))
        throw std::runtime_error("Missing NPY memory order");
    a.fortran = m[1].str() == "True";
    if (!std::regex_search(header, m, shape))
        throw std::runtime_error("Missing NPY shape");
    const std::string dims = m[1].str();
    const std::regex digits("[0-9]+");
    for (std::sregex_iterator i(dims.begin(), dims.end(), digits), end;
         i != end; ++i) {
        const unsigned long long v = std::stoull(i->str());
        if (v > std::numeric_limits<size_t>::max())
            throw std::runtime_error("NPY dimension overflow");
        a.shape.push_back(static_cast<size_t>(v));
    }
    const std::string rest = std::regex_replace(dims, digits, "");
    if (rest.find_first_not_of(" ,\t\r\n") != std::string::npos)
        throw std::runtime_error("Invalid NPY shape");
    a.bytes.assign(raw.begin()+start+length, raw.end());
    return a;
}
std::vector<int64_t> decodeLayers(const Npy& a, size_t channels) {
    if (a.fortran || a.shape.size() != 1 || a.shape[0] != channels)
        throw std::runtime_error("Invalid layer_ids dimensions");
    const bool i4 = a.dtype == "<i4" || a.dtype == "=i4";
    const bool i8 = a.dtype == "<i8" || a.dtype == "=i8";
    if (!i4 && !i8)
        throw std::runtime_error("layer_ids must be little-endian int32/int64");
    const size_t stride = i4 ? 4 : 8;
    if (a.bytes.size() != checkedMul(channels, stride))
        throw std::runtime_error("layer_ids byte size mismatch");
    std::vector<int64_t> out;
    out.reserve(channels);
    for (size_t i = 0; i < channels; ++i) {
        const uint8_t* p = a.bytes.data() + i*stride;
        if (i4) {
            const uint32_t bits = rd32(p);
            out.push_back(bits <= INT32_MAX ? int64_t(bits) :
                          int64_t(bits) - (int64_t(1) << 32));
        } else {
            const uint64_t bits = rd64(p);
            int64_t v;
            std::memcpy(&v, &bits, sizeof(v)); // Linux two's complement
            out.push_back(v);
        }
    }
    return out;
}
} // namespace

// List all ZIP members without assuming any NumPy array name.
std::vector<std::string> listNpzEntries(const std::string& path) {
    return ZipArchive(path).names();
}

NpzMask loadNpzMask(const std::string& path) {
    const ZipArchive zip(path);
    const std::vector<std::string> names = zip.names();
    std::string selected;
    Npy selectedArray;
    size_t matches = 0;
    for (const std::string& name : names) {
        if (name.size() < 4 || name.substr(name.size() - 4) != ".npy")
            continue;
        // Skip conventional metadata arrays.
        if (name == "layer_ids.npy")
            continue;
        const Npy a = parseNpy(zip.read(name));
        if ((a.dtype != "|u1" && a.dtype != "<u1" && a.dtype != "=u1") ||
            a.fortran || a.shape.size() != 3)
            continue;
        if (!a.shape[0] || !a.shape[1] || !a.shape[2])
            continue;
        const size_t expected =
            checkedMul(checkedMul(a.shape[0], a.shape[1]), a.shape[2]);
        if (a.bytes.size() != expected)
            throw std::runtime_error("Invalid mask data size: " + name);
        selected = name;
        selectedArray = a;
        ++matches;
    }
    if (matches == 0)
        throw std::runtime_error("No uint8 C-order (C,H,W) NPY array found");
    if (matches > 1)
        throw std::runtime_error("Multiple mask candidates; select an array explicitly");
    NpzMask mask;
    mask.channels = selectedArray.shape[0];
    mask.height = selectedArray.shape[1];
    mask.width = selectedArray.shape[2];
    mask.pixels.swap(selectedArray.bytes);
    if (zip.has("layer_ids.npy"))
        mask.layerIds = decodeLayers(
            parseNpy(zip.read("layer_ids.npy")), mask.channels);
    return mask;
}

#ifdef NPZ_ENABLE_QT_DEBUG
#include <QDebug>
#include <QString>
#include <QStringList>

// Set a breakpoint here or inside loadNpzMask()/parseNpy().
// Enable with DEFINES += NPZ_ENABLE_QT_DEBUG in your .pro file.
void debugNpzFile(const QString& filePath)
{
    try {
        qDebug() << "NPZ file:" << filePath;
        const std::string path = filePath.toLocal8Bit().constData();
        const ZipArchive archive(path);
        for (const std::string& name : archive.names()) {
            qDebug() << "NPZ entry:" << QString::fromStdString(name);
            if (name.size() >= 4 && name.substr(name.size()-4) == ".npy") {
                try {
                    const Npy a = parseNpy(archive.read(name));
                    QStringList dims;
                    for (size_t dim : a.shape)
                        dims << QString::number(static_cast<qulonglong>(dim));
                    qDebug() << "  dtype:" << QString::fromStdString(a.dtype)
                             << "shape:" << dims.join("x")
                             << "Fortran:" << a.fortran;
                } catch (const std::exception& e) {
                    qWarning() << "  NPY inspect error:" << e.what();
                }
            }
        }
        const NpzMask mask = loadNpzMask(path);
        qDebug() << "Shape (C,H,W):"
                 << static_cast<qulonglong>(mask.channels)
                 << static_cast<qulonglong>(mask.height)
                 << static_cast<qulonglong>(mask.width);
        qDebug() << "Data bytes:"
                 << static_cast<qulonglong>(mask.pixels.size());
        for (size_t c = 0; c < mask.channels; ++c) {
            uint64_t pixelSum = 0;
            size_t zeros = 0, filled = 0, unexpected = 0;
            const size_t begin = c * mask.height * mask.width;
            const size_t end = begin + mask.height * mask.width;
            for (size_t i = begin; i < end; ++i) {
                const uint8_t value = mask.pixels[i];
                pixelSum += value;
                if (value == 0) ++zeros;
                else if (value == 255) ++filled;
                else ++unexpected;
            }
            const qlonglong layer = c < mask.layerIds.size()
                ? static_cast<qlonglong>(mask.layerIds[c]) : -1;
            qDebug() << "Channel:" << static_cast<qulonglong>(c)
                     << "Layer:" << layer
                     << "Pixel sum:" << static_cast<qulonglong>(pixelSum)
                     << "Zero:" << static_cast<qulonglong>(zeros)
                     << "Filled:" << static_cast<qulonglong>(filled)
                     << "Other:" << static_cast<qulonglong>(unexpected);
        }
    } catch (const std::exception& e) {
        qWarning() << "NPZ parse failed:" << e.what();
    }
}
#endif
