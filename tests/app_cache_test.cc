// The app-list cache, round-tripped and abused. Plain assert(), Debug only, no
// framework — the same shape as app_list_test.
//
// It exists because this file is read at the very start of every launch, before
// anything else has had a chance to go wrong, and it is written by a process
// that Android may kill at any instant. A cache that parses garbage into a list
// of apps would put nonsense on the home screen; a cache that throws on a short
// read would stop the launcher from starting at all. Neither is allowed: every
// failure below has to come back as a plain "no cache".
#include "../app/app_cache.hh"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

const char* kPath = "app_cache_test.bin";

std::vector<AppEntry> sample() {
    return {
        {"Camera",      "com.a.camera", "com.a.camera.Main"},
        {"Camera Demo", "com.b.camera", "com.b.camera.Main"},
        {"Café",        "com.c.cafe",   "com.c.cafe.Main"},      // UTF-8 survives
        {"",            "com.d.blank",  "com.d.blank.Main"},     // and so does empty
    };
}

void write_bytes(const std::string& bytes) {
    std::ofstream f(kPath, std::ios::binary | std::ios::trunc);
    f.write(bytes.data(), (std::streamsize)bytes.size());
}

std::string read_bytes() {
    std::ifstream f(kPath, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

int main() {
    std::remove(kPath);

    // ── A missing file is a miss, not a failure ─────────────────────────────
    {
        std::vector<AppEntry> out{{"stale", "x", "y"}};
        assert(!load_app_cache(kPath, out));
        assert(out.empty());   // and it does not leave the caller's list behind
    }

    // ── Round trip ──────────────────────────────────────────────────────────
    {
        const std::vector<AppEntry> in = sample();
        assert(save_app_cache(kPath, in));

        std::vector<AppEntry> out;
        assert(load_app_cache(kPath, out));
        assert(out.size() == in.size());
        for (size_t i = 0; i < in.size(); ++i) assert(out[i] == in[i]);
    }

    // ── An empty list is a valid cache, and not the same as no cache ────────
    {
        assert(save_app_cache(kPath, {}));
        std::vector<AppEntry> out;
        assert(load_app_cache(kPath, out));
        assert(out.empty());
    }

    const std::string good = (save_app_cache(kPath, sample()), read_bytes());

    // ── Truncation, at every length ─────────────────────────────────────────
    //
    // A kill mid-write is the expected failure, and the file it leaves can end
    // anywhere. None of these may parse, and none may crash.
    for (size_t n = 0; n < good.size(); ++n) {
        write_bytes(good.substr(0, n));
        std::vector<AppEntry> out;
        assert(!load_app_cache(kPath, out));
        assert(out.empty());
    }

    // ── Trailing junk ───────────────────────────────────────────────────────
    {
        write_bytes(good + "extra");
        std::vector<AppEntry> out;
        assert(!load_app_cache(kPath, out));
    }

    // ── A foreign file ──────────────────────────────────────────────────────
    {
        std::string bad = good;
        bad[0] = 'X';
        write_bytes(bad);
        std::vector<AppEntry> out;
        assert(!load_app_cache(kPath, out));
    }

    // ── A future version ────────────────────────────────────────────────────
    {
        std::string bad = good;
        bad[4] = (char)99;   // version, little-endian, first byte
        write_bytes(bad);
        std::vector<AppEntry> out;
        assert(!load_app_cache(kPath, out));
    }

    // ── A count that would eat the world ────────────────────────────────────
    {
        std::string bad = good;
        bad[8] = (char)0xFF; bad[9] = (char)0xFF;
        bad[10] = (char)0xFF; bad[11] = (char)0xFF;
        write_bytes(bad);
        std::vector<AppEntry> out;
        assert(!load_app_cache(kPath, out));
        assert(out.empty());
    }

    // ── A rewrite replaces, never appends ───────────────────────────────────
    {
        assert(save_app_cache(kPath, sample()));
        assert(save_app_cache(kPath, {{"Only", "com.only", "com.only.Main"}}));
        std::vector<AppEntry> out;
        assert(load_app_cache(kPath, out));
        assert(out.size() == 1 && out[0].label == "Only");
    }

    std::remove(kPath);
    std::printf("app_cache_test: ok\n");
    return 0;
}
