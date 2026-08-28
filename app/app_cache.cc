#include "app_cache.hh"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

// "NavaLauncher App Cache". Version is bumped when the layout below changes;
// an older binary reading a newer file must MISS rather than misparse, which is
// the whole reason the number is here.
constexpr char     kMagic[4]  = {'N', 'L', 'A', 'C'};
constexpr uint32_t kVersion   = 1;

// A cap that is generous for the thing being described and still refuses to
// allocate a gigabyte because four bytes of a damaged file said so.
constexpr uint32_t kMaxCount  = 100000;
constexpr uint32_t kMaxString = 64 * 1024;

void put_u32(std::string& out, uint32_t v) {
    // Little-endian by hand rather than by memcpy of a uint32_t: the file is
    // read back by the same program on the same device, but a format that
    // depends on the compiler's idea of layout is a format that breaks the
    // first time it is opened somewhere else.
    out.push_back((char)(v & 0xFF));
    out.push_back((char)((v >> 8) & 0xFF));
    out.push_back((char)((v >> 16) & 0xFF));
    out.push_back((char)((v >> 24) & 0xFF));
}

void put_str(std::string& out, const std::string& s) {
    put_u32(out, (uint32_t)s.size());
    out += s;
}

// A cursor over the loaded bytes. Every read is bounds-checked and sets `bad`
// on the first overrun, so the parse below can be written straight through and
// checked once at the end.
struct Reader {
    const std::string& buf;
    size_t             at  = 0;
    bool               bad = false;

    uint32_t u32() {
        if (bad || at + 4 > buf.size()) { bad = true; return 0; }
        const uint32_t v = (uint8_t)buf[at] | ((uint32_t)(uint8_t)buf[at + 1] << 8) |
                           ((uint32_t)(uint8_t)buf[at + 2] << 16) |
                           ((uint32_t)(uint8_t)buf[at + 3] << 24);
        at += 4;
        return v;
    }

    std::string str() {
        const uint32_t n = u32();
        if (bad || n > kMaxString || at + n > buf.size()) { bad = true; return {}; }
        std::string s = buf.substr(at, n);
        at += n;
        return s;
    }
};

}  // namespace

bool load_app_cache(const std::string& path, std::vector<AppEntry>& out) {
    out.clear();

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (buf.size() < sizeof(kMagic) + 8) return false;
    if (std::memcmp(buf.data(), kMagic, sizeof(kMagic)) != 0) return false;

    Reader r{buf, sizeof(kMagic), false};
    if (r.u32() != kVersion) return false;

    const uint32_t count = r.u32();
    if (r.bad || count > kMaxCount) return false;

    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        AppEntry e;
        e.label    = r.str();
        e.package  = r.str();
        e.activity = r.str();
        if (r.bad) { out.clear(); return false; }
        out.push_back(std::move(e));
    }
    // Trailing junk is a damaged file, not a shorter one: an entry count that
    // does not consume the whole payload means the write was interrupted or the
    // format moved, and either way the cache is not what it claims to be.
    if (r.at != buf.size()) { out.clear(); return false; }
    return true;
}

bool save_app_cache(const std::string& path, const std::vector<AppEntry>& in) {
    std::string buf;
    buf.append(kMagic, sizeof(kMagic));
    put_u32(buf, kVersion);
    put_u32(buf, (uint32_t)in.size());
    for (const AppEntry& e : in) {
        put_str(buf, e.label);
        put_str(buf, e.package);
        put_str(buf, e.activity);
    }

    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(buf.data(), (std::streamsize)buf.size());
        if (!f) { f.close(); std::remove(tmp.c_str()); return false; }
    }
    // rename() over an existing file is atomic on every filesystem this runs
    // on, which is what makes a half-written cache impossible to read.
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        std::remove(tmp.c_str());
        return false;
    }
    return true;
}
