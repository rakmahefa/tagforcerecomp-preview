// launcher_iso.cpp - a minimal ISO9660 reader that recognises the game disc.
//
// Same approach as the project's own runtime/core/gameid.cpp: the primary
// volume descriptor at sector 16 says where the root directory is, the root
// directory says where UMD_DATA.BIN is, and its first bytes carry the game id
// ("ULES-00600|<serial>|..."). Everything else in the image stays untouched.
#include "launcher_iso.h"

#include <cstdio>
#include <cstring>

namespace tf::launcher {
namespace {

// "ULES-00600|AA86E435ABE8C363|0001|G ..." -> "ULES-00600"
std::string idFromUmd(const char *buf, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && buf[i]; ++i)
        s += buf[i];
    const size_t bar = s.find('|');
    if (bar == std::string::npos)
        return {};
    s.resize(bar);
    if (s.size() != 10 || s[4] != '-')
        return {};
    return s;
}

} // namespace

DiscInfo inspectDisc(const std::filesystem::path &iso) {
    DiscInfo info;
    std::error_code ec;
    if (iso.empty() || !std::filesystem::is_regular_file(iso, ec)) {
        info.error = "That file does not exist. Pick your disc image (a .iso file) and try again.";
        return info;
    }
    info.size = static_cast<uint64_t>(std::filesystem::file_size(iso, ec));
    if (ec)
        info.size = 0;
    FILE *file = fopen(iso.string().c_str(), "rb"); // paths are UTF-8 on Linux
    if (!file) {
        info.error = "That file could not be opened. Check that this account can read it.";
        return info;
    }
    std::string id, volume;
    uint8_t pvd[2048];
    const auto readSector = [&](uint64_t lba, void *dst, size_t n) {
        return fseek(file, static_cast<long>(lba * 2048), SEEK_SET) == 0 && fread(dst, 1, n, file) == n;
    };
    if (readSector(16, pvd, sizeof pvd) && memcmp(pvd + 1, "CD001", 5) == 0) {
        volume = std::string(reinterpret_cast<const char *>(pvd) + 40, 32); // volume id, space padded
        while (!volume.empty() && (volume.back() == ' '))
            volume.pop_back();
        const uint32_t rootLba = *reinterpret_cast<const uint32_t *>(pvd + 156 + 2);
        const uint32_t rootSize = *reinterpret_cast<const uint32_t *>(pvd + 156 + 10);
        if (rootSize > 0 && rootSize <= (1u << 20)) {
            std::vector<uint8_t> dir(rootSize);
            if (readSector(rootLba, dir.data(), rootSize)) {
                for (size_t i = 0; i < rootSize;) {
                    const uint8_t recordLength = dir[i];
                    if (!recordLength) { // records never cross a sector line
                        i = (i / 2048 + 1) * 2048;
                        continue;
                    }
                    if (i + 33 + dir[i + 32] > rootSize)
                        break;
                    const uint8_t nameLength = dir[i + 32];
                    const std::string name(reinterpret_cast<const char *>(&dir[i + 33]), nameLength);
                    if (name.compare(0, 12, "UMD_DATA.BIN") == 0) {
                        char head[64] = {};
                        const uint32_t lba = *reinterpret_cast<const uint32_t *>(&dir[i + 2]);
                        if (readSector(lba, head, sizeof head - 1))
                            id = idFromUmd(head, sizeof head);
                        break;
                    }
                    i += recordLength;
                }
            }
        }
        info.readable = true;
    }
    fclose(file);

    info.volume = volume;
    info.gameId = id;
    if (!info.readable) {
        info.error = "That does not look like a disc image (no ISO9660 volume). Pick the .iso you made from your own copy.";
        return info;
    }
    if (id.empty()) {
        info.error = "This disc has no PSP game id in it. Tag Force Recomp needs the European release of Yu-Gi-Oh! GX Tag Force, ULES-00600.";
        return info;
    }
    info.supported = id == kGameId;
    if (!info.supported)
        info.error = "This is " + id + ". Tag Force Recomp needs the European release of Yu-Gi-Oh! GX Tag Force, ULES-00600.";
    return info;
}

} // namespace tf::launcher
