// npzParser.cpp
// C++11 / Qt 5.9 compatible NPZ reader for uint8 masks (C,H,W).
// Dependency: libzip. Build with: LIBS += -lzip
// No main() or synthetic data. Expects mask.npy; layer_ids.npy is optional.
#include <zip.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
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
            throw std::out_of_range("NPZ mask index");
        return pixels[(c * height + y) * width + x];
    }
};

namespace {
size_t checkedMul(size_t a, size_t b) {
    if (a && b > std::numeric_limits<size_t>::max() / a)
        throw std::runtime_error("NPZ dimension overflow");
    return a * b;
}
uint16_t u16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
uint32_t u32(const uint8_t* p) {
    return uint32_t(u16(p)) | (uint32_t(u16(p + 2)) << 16);
}
uint64_t u64(const uint8_t* p) {
    return uint64_t(u32(p)) | (uint64_t(u32(p + 4)) << 32);
}
std::vector<uint8_t> readEntry(zip_t* archive, const char* name) {
    zip_stat_t st;
    zip_stat_init(&st);
    if (zip_stat(archive, name, 0, &st) != 0 ||
        !(st.valid & ZIP_STAT_SIZE) ||
        st.size > std::numeric_limits<size_t>::max())
        throw std::runtime_error(std::string("Missing/invalid NPZ entry: ") + name);
    zip_file_t* file = zip_fopen(archive, name, 0);
    if (!file) throw std::runtime_error("Cannot open NPZ member");
    try {
        std::vector<uint8_t> bytes(static_cast<size_t>(st.size));
        size_t offset = 0;
        while (offset < bytes.size()) {
            const size_t chunk = std::min<size_t>(bytes.size() - offset, 1u << 20);
            const zip_int64_t n = zip_fread(file, bytes.data() + offset, chunk);
            if (n <= 0) throw std::runtime_error("Truncated NPZ member");
            offset += static_cast<size_t>(n);
        }
        uint8_t extra = 0;
        if (zip_fread(file, &extra, 1) != 0)
            throw std::runtime_error("NPZ member size mismatch");
        zip_file_t* closing = file;
        file = nullptr; // zip_fclose consumes the handle, including on failure.
        if (zip_fclose(closing) != 0)
            throw std::runtime_error("NPZ member CRC/close failure");
        return bytes;
    } catch (...) {
        if (file) zip_fclose(file);
        throw;
    }
}
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
        start = 10;
        length = u16(raw.data() + 8);
    } else if ((raw[6] == 2 || raw[6] == 3) && raw.size() >= 12) {
        start = 12;
        length = u32(raw.data() + 8);
    } else {
        throw std::runtime_error("Unsupported NPY version");
    }
    if (length > raw.size() - start)
        throw std::runtime_error("Truncated NPY header");
    const std::string header(reinterpret_cast<const char*>(raw.data() + start), length);
    const std::regex descr(R"re(['"]descr['"]\s*:\s*['"]([^'"]+)['"])re");
    const std::regex order(R"re(['"]fortran_order['"]\s*:\s*(True|False))re");
    const std::regex shape(R"re(['"]shape['"]\s*:\s*\(([^)]*)\))re");
    std::smatch m;
    Npy result;
    if (!std::regex_search(header, m, descr))
        throw std::runtime_error("NPY dtype missing");
    result.dtype = m[1].str();
    if (!std::regex_search(header, m, order))
        throw std::runtime_error("NPY order missing");
    result.fortran = m[1].str() == "True";
    if (!std::regex_search(header, m, shape))
        throw std::runtime_error("NPY shape missing");
    const std::string dims = m[1].str();
    const std::regex number("[0-9]+");
    for (std::sregex_iterator i(dims.begin(), dims.end(), number), end; i != end; ++i) {
        const unsigned long long n = std::stoull(i->str());
        if (n > std::numeric_limits<size_t>::max())
            throw std::runtime_error("NPY dimension overflow");
        result.shape.push_back(static_cast<size_t>(n));
    }
    const std::string leftover = std::regex_replace(dims, number, "");
    if (leftover.find_first_not_of(" ,\t\r\n") != std::string::npos)
        throw std::runtime_error("Malformed NPY shape");
    result.bytes.assign(raw.begin() + start + length, raw.end());
    return result;
}
std::vector<int64_t> decodeLayers(const Npy& a, size_t count) {
    if (a.fortran || a.shape.size() != 1 || a.shape[0] != count)
        throw std::runtime_error("Invalid layer_ids dimensions");
    const bool i4 = a.dtype == "<i4" || a.dtype == "=i4";
    const bool i8 = a.dtype == "<i8" || a.dtype == "=i8";
    if (!i4 && !i8) throw std::runtime_error("Unsupported layer_ids dtype");
    const size_t stride = i4 ? 4 : 8;
    if (a.bytes.size() != checkedMul(count, stride))
        throw std::runtime_error("layer_ids byte size mismatch");
    std::vector<int64_t> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = a.bytes.data() + i * stride;
        if (i4) {
            const uint32_t bits = u32(p);
            out.push_back(bits <= INT32_MAX ? int64_t(bits) :
                          int64_t(bits) - (int64_t(1) << 32));
        } else {
            const uint64_t bits = u64(p);
            int64_t signedBits;
            std::memcpy(&signedBits, &bits, sizeof(signedBits));
            out.push_back(signedBits);
        }
    }
    return out;
}
} // namespace

NpzMask loadNpzMask(const std::string& path) {
    int error = 0;
    zip_t* archive = zip_open(path.c_str(), ZIP_RDONLY, &error);
    if (!archive) throw std::runtime_error("Cannot open NPZ archive");
    try {
        const Npy a = parseNpy(readEntry(archive, "mask.npy"));
        if (a.dtype != "|u1" && a.dtype != "<u1" && a.dtype != "=u1")
            throw std::runtime_error("mask.npy must be uint8");
        if (a.fortran || a.shape.size() != 3)
            throw std::runtime_error("mask.npy must be C-order (C,H,W)");
        NpzMask mask;
        mask.channels = a.shape[0];
        mask.height = a.shape[1];
        mask.width = a.shape[2];
        if (!mask.channels || !mask.height || !mask.width)
            throw std::runtime_error("Zero-sized mask dimension");
        const size_t expected =
            checkedMul(checkedMul(mask.channels, mask.height), mask.width);
        if (a.bytes.size() != expected)
            throw std::runtime_error("mask.npy byte size mismatch");
        mask.pixels = a.bytes;
        if (zip_name_locate(archive, "layer_ids.npy", 0) >= 0)
            mask.layerIds = decodeLayers(
                parseNpy(readEntry(archive, "layer_ids.npy")), mask.channels);
        if (zip_close(archive) != 0)
            throw std::runtime_error("Cannot close NPZ archive");
        return mask;
    } catch (...) {
        zip_discard(archive);
        throw;
    }
}
