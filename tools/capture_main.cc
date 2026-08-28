// ── Headless screenshots of the launcher ─────────────────────────────────────
//
// Renders the launcher's states off-screen (VK_EXT_headless_surface — no
// window, no compositor, no phone) and writes one PNG each, through
// vk_canvas's own capture runner.
//
// It exists because most of what this program does cannot be photographed on a
// desktop: the long-press menu needs a finger, the keyboard inset needs a real
// IME, and a list long enough to scroll needs a phone with sixty apps on it.
// Here they are all just fields in a LauncherFrame.
//
//   ./nava_capture --out ui-shots --frame 1080x2400
//   ./nava_capture --list
//   ./nava_capture --only menu
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "app_list.hh"
#include "canvas.hh"
#include "capture.hh"      // vk_canvas
#include "launcher_draw.hh"
#include "msdf.hh"
#include "renderer.hh"
#include "ui_metrics.hh"

namespace {

// The font, loaded once and shared by every scenario. A file-scope pointer
// rather than a capture, because CaptureConfig::init and the scenario lambdas
// are separate std::functions and both need it.
MsdfFont* g_font = nullptr;

// A believable phone. Same shape as a real PackageManager answer: sorted by
// label, with the launcher itself already filtered out.
std::vector<AppEntry> phone_apps() {
    static const char* kLabels[] = {
        "APK Explorer & Editor", "Calculator", "Calendar", "Camera",
        "Camera Demo", "Clock", "Contacts", "Deku SMS", "Droid-ify",
        "Fennec", "Files", "FlorisBoard Beta", "Ghost Commander", "Maps",
        "Messages", "mpv", "My Files", "ObtainX", "Phone", "Play Store",
        "Settings", "Signal", "Syncthing", "Termux", "Weather",
    };
    std::vector<AppEntry> out;
    for (const char* label : kLabels) {
        std::string pkg = std::string("com.example.") + label;
        out.push_back({label, pkg, pkg + ".Main"});
    }
    return out;
}

// The collision case: two apps genuinely called the same thing, which is what
// the package suffix exists for.
std::vector<AppEntry> colliding_apps() {
    return {
        {"Camera",   "com.android.camera", "com.android.camera.Main"},
        {"Camera",   "com.vendor.gcam",    "com.vendor.gcam.Main"},
        {"Files",    "com.android.files",  "com.android.files.Main"},
        {"Messages", "com.android.mms",    "com.android.mms.Main"},
        {"Messages", "com.vendor.message", "com.vendor.message.Main"},
        {"Settings", "com.android.settings","com.android.settings.Main"},
    };
}

// Enough apps that the list cannot fit on any screen, for the scroll case.
std::vector<AppEntry> many_apps() {
    std::vector<AppEntry> out;
    for (int i = 0; i < 60; ++i) {
        char label[32];
        std::snprintf(label, sizeof label, "Application %02d", i);
        out.push_back({label, "com.example.app", "com.example.app.Main"});
    }
    return out;
}

// One scenario: build the frame, draw it, hand it to the renderer. The only
// thing that differs between scenarios is the LauncherFrame, which is the whole
// point of separating it from LauncherApp.
void render_frame(Renderer& r, const AppList& apps, int menuRow, float scroll,
                  int keyboardInset) {
    LauncherFrame f;
    f.apps          = &apps;
    f.width         = r.width();
    f.height        = r.height();
    f.menuRow       = menuRow;
    f.scroll        = scroll;
    f.keyboardInset = keyboardInset;
    // A modest cutout, so the shots show the layout the safe area produces
    // rather than a flush-to-the-edge one no phone has.
    f.insetTop      = (int)(f.height * 0.03f);
    f.insetBottom   = (int)(f.height * 0.01f);

    const UiMetrics m = computeUiMetrics((float)(f.width < f.height ? f.width : f.height));

    std::vector<float> curves, quads;
    Canvas c(curves, f.width, f.height, nullptr,
             (float)f.insetTop, (float)f.insetBottom,
             (float)f.insetLeft, (float)f.insetRight);
    if (g_font) c.useMsdf(g_font, &quads);

    draw_launcher(c, f, m);

    // Same ordering rule as the app: upload the lazily-baked atlas cells AFTER
    // layout and BEFORE the draw that indexes them.
    if (g_font) r.initMsdf(*g_font);
    r.draw(curves, /*rotation=*/0, {}, {}, quads);
}

AppList withQuery(std::vector<AppEntry> entries, const std::string& query) {
    AppList apps;
    apps.set(std::move(entries));
    apps.setQuery(query);
    return apps;
}

} // namespace

int main(int argc, char** argv) {
    // Held for the process's lifetime: the scenarios run inside capture_main().
    static AppList empty   = withQuery(phone_apps(), "");
    static AppList matched = withQuery(phone_apps(), "cam");
    static AppList missing = withQuery(phone_apps(), "zzz");
    static AppList longList  = withQuery(many_apps(), "");
    static AppList colliding = withQuery(colliding_apps(), "");

    const std::vector<vkc::Scenario> scenarios = {
        {"00-idle",        [](Renderer& r) { render_frame(r, empty,    -1, 0.0f, 0); }},
        {"10-query",       [](Renderer& r) { render_frame(r, matched,  -1, 0.0f, 0); }},
        {"20-no-match",    [](Renderer& r) { render_frame(r, missing,  -1, 0.0f, 0); }},
        {"30-menu",        [](Renderer& r) { render_frame(r, matched,   1, 0.0f, 0); }},
        // The two that only exist on a phone: the IME covering the bottom half,
        // and a list scrolled into its middle.
        {"40-keyboard",    [](Renderer& r) {
            render_frame(r, empty, -1, 0.0f, (int)(r.height() * 0.45f)); }},
        {"45-duplicates",  [](Renderer& r) { render_frame(r, colliding, -1, 0.0f, 0); }},
        {"50-scrolled",    [](Renderer& r) {
            render_frame(r, longList, -1, 900.0f, (int)(r.height() * 0.45f)); }},
    };

    vkc::CaptureConfig cfg;
    cfg.default_out = "ui-shots";
    cfg.init = [](Renderer& r) {
        static MsdfFont font;
        static FileByteReader assets;   // resolves paths from the working directory
        if (font.generate(assets, "assets/fonts/ui.otf")) {
            r.initMsdf(font);
            g_font = &font;
        } else {
            std::fprintf(stderr,
                "warning: assets/fonts/ui.otf not found — run this from the "
                "build directory. Text will fall back to stroked outlines.\n");
        }
    };

    return vkc::capture_main(argc, argv, scenarios, cfg);
}
