// launcher_png.cpp - a self-contained PNG writer.
//
// Only what a screenshot needs: 8-bit RGBA, one image, stored deflate blocks
// (BTYPE=00) inside a zlib stream, CRC-32 computed with a generated table.
// glReadPixels hands us rows bottom-up, so the writer flips them.
#include "launcher_png.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace tf::launcher {
namespace {

const uint32_t *crcTable() {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int bit = 0; bit < 8; ++bit)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        ready = true;
    }
    return table;
}

void put32(std::vector<uint8_t> &out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

// A chunk: length, type, data, crc over type + data.
void chunk(std::vector<uint8_t> &out, const char type[4], const std::vector<uint8_t> &data) {
    put32(out, static_cast<uint32_t>(data.size()));
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    const uint32_t *table = crcTable();
    uint32_t crc = 0xFFFFFFFFu;
    const auto update = [&](const uint8_t *bytes, size_t n) {
        for (size_t i = 0; i < n; ++i)
            crc = table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    };
    update(reinterpret_cast<const uint8_t *>(type), 4);
    if (!data.empty())
        update(data.data(), data.size());
    put32(out, crc ^ 0xFFFFFFFFu);
}

uint32_t adler32(const uint8_t *data, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

} // namespace

bool savePng(const std::string &path, int width, int height, const uint8_t *rgba) {
    if (width <= 0 || height <= 0 || !rgba)
        return false;
    // Raw scanlines, top row first, each with filter byte 0 (None).
    std::vector<uint8_t> raw(static_cast<size_t>(height) * (static_cast<size_t>(width) * 4 + 1));
    for (int y = 0; y < height; ++y) {
        uint8_t *row = raw.data() + static_cast<size_t>(y) * (static_cast<size_t>(width) * 4 + 1);
        row[0] = 0;
        std::memcpy(row + 1, rgba + static_cast<size_t>(height - 1 - y) * static_cast<size_t>(width) * 4,
                    static_cast<size_t>(width) * 4);
    }
    // The zlib stream: header, stored blocks, adler32.
    std::vector<uint8_t> zlib;
    zlib.push_back(0x78); // CMF: deflate, 32 K window
    zlib.push_back(0x01); // FLG: no dict, check bits valid
    size_t at = 0;
    while (at < raw.size()) {
        const size_t take = raw.size() - at > 65535 ? 65535 : raw.size() - at;
        const bool last = at + take == raw.size();
        zlib.push_back(last ? 1 : 0); // BFINAL + BTYPE=00
        zlib.push_back(static_cast<uint8_t>(take & 0xFF));      // LEN, little-endian
        zlib.push_back(static_cast<uint8_t>((take >> 8) & 0xFF));
        zlib.push_back(static_cast<uint8_t>(~take & 0xFF));     // NLEN, its complement
        zlib.push_back(static_cast<uint8_t>((~take >> 8) & 0xFF));
        zlib.insert(zlib.end(), raw.data() + at, raw.data() + at + take);
        at += take;
    }
    put32(zlib, adler32(raw.data(), raw.size()));

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    put32(ihdr, static_cast<uint32_t>(width));
    put32(ihdr, static_cast<uint32_t>(height));
    ihdr.push_back(8);  // bit depth
    ihdr.push_back(6);  // colour type: RGBA
    ihdr.push_back(0);  // compression: deflate
    ihdr.push_back(0);  // filter: adaptive (rows say it)
    ihdr.push_back(0);  // interlace: none
    chunk(png, "IHDR", ihdr);
    chunk(png, "IDAT", zlib);
    chunk(png, "IEND", {});
    FILE *file = fopen(path.c_str(), "wb");
    if (!file)
        return false;
    const bool wrote = fwrite(png.data(), 1, png.size(), file) == png.size();
    fclose(file);
    return wrote;
}

} // namespace tf::launcher
