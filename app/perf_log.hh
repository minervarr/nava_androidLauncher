#pragma once
#include <chrono>
#include <cstdio>

#ifdef __ANDROID__
#include <android/log.h>
#endif

// ── "Where did the time go?" ─────────────────────────────────────────────────
//
// A launcher is judged on the milliseconds between pressing Home and being able
// to type, and every one of them is spent in code that looks instantaneous when
// read. This exists so the answer comes from the device instead of from a guess:
//   adb logcat -s navaLauncher
//
// Permanent, not a debugging leftover. One log line per phase costs nothing
// against work measured in tens of milliseconds, and the alternative — adding
// the timing back whenever a start feels slow — is how the 490 ms font bake
// went unnoticed for as long as it did.
namespace perf {

inline void log_ms(const char* what, double ms) {
#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "navaLauncher", "%s: %.1f ms", what, ms);
#else
    std::fprintf(stderr, "[perf] %s: %.1f ms\n", what, ms);
#endif
}

// Times a scope and logs on destruction. Named for what it measures, because
// that name is all a logcat line carries.
class Span {
public:
    explicit Span(const char* what) : what_(what), t0_(Clock::now()) {}
    ~Span() { log_ms(what_, ms()); }

    double ms() const {
        return std::chrono::duration<double, std::milli>(Clock::now() - t0_).count();
    }

private:
    using Clock = std::chrono::steady_clock;
    const char*       what_;
    Clock::time_point t0_;
};

}  // namespace perf
