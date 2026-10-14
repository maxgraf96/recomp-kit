// gdi32_fontdb.cpp - the platform's installed fonts, as GDI's font mapper
// would see them, for [runtime] system_fonts.
//
// A game names a face - "Arial", "Tahoma" - and Windows answers from its own
// font directory. macOS ships the same faces (Arial, Arial Bold, Tahoma,
// Times New Roman, Courier New, Verdana, ... in /System/Library/Fonts/
// Supplemental), built by the same foundries with the same Windows tables
// (OS/2, hdmx, VDMX) GDI measures with. Using them instead of a stand-in is
// what makes a game's text the width and height it was laid out for.
//
// The directories are scanned once, lazily, reading only each file's table
// directory and its name, OS/2 and head tables; a face is read whole only
// when it is first chosen. Matching follows GDI: the family name (name ID 1,
// Windows English record) first, then the typographic family (ID 16), then a
// face whose full name (ID 4) is the requested name ("Arial Bold"); among a
// family's faces, the requested slant and the nearest weight, with bold and
// italic simulated when the family has no such face. A few legacy names
// resolve through Windows' own FontSubstitutes.
#include "gdi32_truetype.h"
#include "win32.h"
#include "../platform/os.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace gdi {
namespace {

struct Entry {
    std::string path;
    int index = 0;
    std::string family, typo, full; // lower case
    int weight = 400;
    bool italic = false;
};

std::string g_dirs_override;
bool g_scanned = false;
std::vector<Entry> &entries() {
    static std::vector<Entry> list;
    return list;
}
std::map<std::pair<std::string, int>, const TrueTypeFace *> &loaded() {
    static std::map<std::pair<std::string, int>, const TrueTypeFace *> m;
    return m;
}

std::string lower_ascii(std::string s) {
    for (char &ch : s)
        if (ch >= 'A' && ch <= 'Z')
            ch = char(ch - 'A' + 'a');
    return s;
}
bool ends_with(const std::string &s, const char *tail) {
    const size_t n = strlen(tail);
    return s.size() >= n && lower_ascii(s.substr(s.size() - n)) == tail;
}

// Reads exactly `n` bytes at `offset`, or fails.
bool read_at(int fd, uint64_t offset, void *buf, size_t n) {
    if (os_fd_seek(fd, int64_t(offset), OS_SEEK_SET) != int64_t(offset))
        return false;
    uint8_t *p = static_cast<uint8_t *>(buf);
    while (n) {
        const int64_t got = os_fd_read(fd, p, n);
        if (got <= 0)
            return false;
        p += got;
        n -= size_t(got);
    }
    return true;
}
uint16_t be16(const uint8_t *p) {
    return uint16_t((p[0] << 8) | p[1]);
}
uint32_t be32(const uint8_t *p) {
    return (uint32_t(be16(p)) << 16) | be16(p + 2);
}

// One name from a name table: the Windows Unicode English record when there
// is one (what GDI matches), else any Windows Unicode record, else Mac Roman.
std::string name_of(const std::vector<uint8_t> &t, uint16_t id) {
    if (t.size() < 6)
        return {};
    const uint16_t count = be16(&t[2]), strings = be16(&t[4]);
    int best = -1, best_rank = 0;
    for (uint16_t i = 0; i < count; ++i) {
        const size_t r = 6 + 12 * size_t(i);
        if (r + 12 > t.size())
            break;
        const uint16_t platform = be16(&t[r]), encoding = be16(&t[r + 2]),
                       language = be16(&t[r + 4]), name = be16(&t[r + 6]);
        if (name != id)
            continue;
        int rank = 0;
        if (platform == 3 && (encoding == 1 || encoding == 0))
            rank = language == 0x409 ? 3 : 2;
        else if (platform == 1 && encoding == 0 && language == 0)
            rank = 1;
        if (rank > best_rank) {
            best_rank = rank;
            best = i;
        }
    }
    if (best < 0)
        return {};
    const size_t r = 6 + 12 * size_t(best);
    const uint16_t platform = be16(&t[r]), length = be16(&t[r + 8]), offset = be16(&t[r + 10]);
    const size_t at = size_t(strings) + offset;
    if (at + length > t.size())
        return {};
    std::string out;
    if (platform == 3) {
        for (size_t k = 0; k + 1 < length; k += 2) {
            const uint16_t unit = be16(&t[at + k]);
            out.push_back(unit < 128 ? char(unit) : '?');
        }
    } else
        out.assign(reinterpret_cast<const char *>(&t[at]), length);
    return lower_ascii(out);
}

// The faces in one file: a TrueType or OpenType font, or a collection.
void scan_file(const std::string &path) {
    const int fd = os_fd_open(path.c_str(), OS_O_RDONLY);
    if (fd < 0)
        return;
    uint8_t head[12];
    std::vector<uint32_t> offsets;
    if (read_at(fd, 0, head, 12)) {
        if (be32(head) == 0x74746366 /* 'ttcf' */) {
            const uint32_t n = std::min<uint32_t>(be32(head + 8), 64);
            std::vector<uint8_t> list(4 * size_t(n));
            if (n && read_at(fd, 12, list.data(), list.size()))
                for (uint32_t i = 0; i < n; ++i)
                    offsets.push_back(be32(&list[4 * i]));
        } else
            offsets.push_back(0);
    }
    for (size_t index = 0; index < offsets.size(); ++index) {
        uint8_t sfnt[12];
        if (!read_at(fd, offsets[index], sfnt, 12))
            continue;
        const uint32_t version = be32(sfnt);
        if (version != 0x00010000 && version != 0x74727565 /* 'true' */ &&
            version != 0x4f54544f /* 'OTTO' */)
            continue;
        const uint16_t tables = std::min<uint16_t>(be16(sfnt + 4), 256);
        std::vector<uint8_t> dir(16 * size_t(tables));
        if (!read_at(fd, offsets[index] + 12, dir.data(), dir.size()))
            continue;
        uint32_t name_at = 0, name_size = 0, os2_at = 0, os2_size = 0, head_at = 0;
        for (uint16_t t = 0; t < tables; ++t) {
            const uint8_t *e = &dir[16 * size_t(t)];
            const uint32_t at = be32(e + 8), size = be32(e + 12);
            if (!memcmp(e, "name", 4))
                name_at = at, name_size = size;
            else if (!memcmp(e, "OS/2", 4))
                os2_at = at, os2_size = size;
            else if (!memcmp(e, "head", 4) && size >= 54)
                head_at = at;
        }
        if (!name_at || !name_size || name_size > (1u << 20))
            continue;
        std::vector<uint8_t> names(name_size);
        if (!read_at(fd, name_at, names.data(), names.size()))
            continue;
        Entry e;
        e.path = path;
        e.index = int(index);
        e.family = name_of(names, 1);
        e.typo = name_of(names, 16);
        e.full = name_of(names, 4);
        if (e.family.empty())
            continue;
        uint8_t os2[64];
        if (os2_at && os2_size >= 64 && read_at(fd, os2_at, os2, 64)) {
            e.weight = be16(os2 + 4) ? be16(os2 + 4) : 400;
            e.italic = (be16(os2 + 62) & 1) != 0;
        }
        uint8_t hd[54];
        if (head_at && read_at(fd, head_at, hd, 54) && (be16(hd + 44) & 2))
            e.italic = true;
        entries().push_back(std::move(e));
    }
    os_fd_close(fd);
}

void scan_dir(const std::string &dir, int depth) {
    std::vector<std::string> names;
    os_listdir(
        dir.c_str(),
        [](const char *name, void *user) {
            static_cast<std::vector<std::string> *>(user)->push_back(name);
            return 0;
        },
        &names);
    std::sort(names.begin(), names.end()); // the same faces in the same order on every run
    for (const std::string &name : names) {
        if (name.empty() || name[0] == '.')
            continue;
        const std::string path = dir + "/" + name;
        OsStat st{};
        if (os_stat(path.c_str(), &st) != 0)
            continue;
        if (st.is_dir) {
            if (depth < 4)
                scan_dir(path, depth + 1);
        } else if (st.is_regular &&
                   (ends_with(name, ".ttf") || ends_with(name, ".ttc") || ends_with(name, ".otf")))
            scan_file(path);
    }
}

void scan() {
    if (g_scanned)
        return;
    g_scanned = true;
    std::string dirs = g_dirs_override;
    if (dirs.empty()) {
        char buf[8192];
        if (os_font_dirs(buf, sizeof buf) == 0)
            dirs = buf;
    }
    size_t start = 0;
    while (start <= dirs.size()) {
        size_t end = dirs.find('\n', start);
        if (end == std::string::npos)
            end = dirs.size();
        if (end > start)
            scan_dir(dirs.substr(start, end - start), 0);
        start = end + 1;
    }
    LOGV("gdi: %zu installed font faces found", entries().size());
}

// The face of an entry, read whole on first use.
const TrueTypeFace *load(const Entry &e) {
    auto key = std::make_pair(e.path, e.index);
    auto it = loaded().find(key);
    if (it != loaded().end())
        return it->second;
    const TrueTypeFace *face = nullptr;
    OsStat st{};
    if (os_stat(e.path.c_str(), &st) == 0 && st.size > 0 && st.size <= (64u << 20)) {
        std::vector<uint8_t> bytes(size_t(st.size));
        const int fd = os_fd_open(e.path.c_str(), OS_O_RDONLY);
        if (fd >= 0) {
            if (read_at(fd, 0, bytes.data(), bytes.size()))
                face = truetype_adopt_file(std::move(bytes), e.index);
            os_fd_close(fd);
        }
    }
    if (!face)
        log_once(("gdi-font-read:" + e.path).c_str(), "gdi: cannot read the font file %s",
                 e.path.c_str());
    loaded()[key] = face;
    return face;
}

// Windows' FontSubstitutes and the font mapper's aliases for names no
// installation has as a family.
const char *substitute(const std::string &name) {
    static const struct {
        const char *from, *to;
    } kAliases[] = {
        {"ms shell dlg", "microsoft sans serif"},
        {"ms shell dlg 2", "tahoma"},
        {"ms sans serif", "microsoft sans serif"},
        {"helv", "microsoft sans serif"},
        {"helvetica", "arial"},
        {"arial cyr", "arial"},
        {"tms rmn", "times new roman"},
        {"times", "times new roman"},
        {"ms serif", "times new roman"},
        {"courier", "courier new"},
    };
    for (const auto &a : kAliases)
        if (name == a.from)
            return a.to;
    return nullptr;
}

} // namespace

bool truetype_system_fonts_enabled() {
    const char *e = recomp_env("SYSTEM_FONTS");
    if (e && *e)
        return atoi(e) != 0;
#ifdef RECOMP_SYSTEM_FONTS_DEFAULT
    return RECOMP_SYSTEM_FONTS_DEFAULT != 0;
#else
    return false;
#endif
}

void truetype_set_font_dirs_for_test(const std::string &dirs) {
    g_dirs_override = dirs;
    g_scanned = false;
    entries().clear();
    loaded().clear();
}

const TrueTypeFace *truetype_system_face(const std::string &name, int32_t weight, bool italic,
                                         bool *synth_bold, bool *synth_italic) {
    if (synth_bold)
        *synth_bold = false;
    if (synth_italic)
        *synth_italic = false;
    scan();
    std::string key = lower_ascii(name);
    const int want = weight > 0 ? weight : 400;
    for (int round = 0; round < 2; ++round) {
        // The family (ID 1); failing that, the typographic family (ID 16).
        std::vector<const Entry *> family;
        for (const Entry &e : entries())
            if (e.family == key)
                family.push_back(&e);
        if (family.empty())
            for (const Entry &e : entries())
                if (e.typo == key)
                    family.push_back(&e);
        if (!family.empty()) {
            const Entry *best = nullptr;
            int best_cost = 0;
            for (const Entry *e : family) {
                const int cost = (e->italic != italic ? 10000 : 0) + std::abs(e->weight - want);
                if (!best || cost < best_cost) {
                    best = e;
                    best_cost = cost;
                }
            }
            if (synth_bold)
                *synth_bold = want >= 550 && best->weight < 550;
            if (synth_italic)
                *synth_italic = italic && !best->italic;
            return load(*best);
        }
        // A face's full name names exactly that face.
        for (const Entry &e : entries())
            if (e.full == key) {
                if (synth_italic)
                    *synth_italic = italic && !e.italic;
                return load(e);
            }
        const char *alias = substitute(key);
        if (!alias)
            break;
        key = alias;
    }
    return nullptr;
}
} // namespace gdi
