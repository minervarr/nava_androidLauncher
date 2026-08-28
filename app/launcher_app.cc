#include "launcher_app.hh"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "app_cache.hh"
#include "app_paths.hh"  // app_shell — stateDir()
#include "canvas.hh"     // vk_canvas
#include "host.hh"       // app_shell
#include "keys.hh"       // vk_canvas
#include "msdf.hh"       // vulkan_font_engine
#include "perf_log.hh"
#include "renderer.hh"   // vk_canvas

namespace {

// Timer ids. The host never reads them; see host.hh on whose vocabulary this is.
constexpr int kLongPressTimer = 1;
constexpr int kLongPressMs    = 420;

// A tap that travelled this far was a scroll, not a press. In authored pixels;
// scaled through UiMetrics before use.
constexpr float kTapSlop = 16.0f;

// Where the font comes from, and where its baked atlas is kept.
//
// MsdfFont, and the choice was MEASURED rather than reasoned. The engine ships
// a second TextFont — RasterFont, a per-size glyph cache — and text_font.hh
// argues it is the right one for a UI with a few fixed sizes and no zoom, which
// describes this screen exactly. It was tried here, on the phone, and it is
// slower: rasterizing ~200 glyphs at three sizes costs ~22 ms on every cold
// start, while READING a pre-baked MTSDF atlas off disk costs ~6 ms. First
// frame: 132-143 ms on MTSDF, 145-170 ms on the raster cache (S23 Ultra,
// warm device, four runs each).
//
// The reasoning behind raster_font.hh is not wrong — it is about a CJK library
// too big for MTSDF's sheet, and about cell area, neither of which this
// launcher has. What it does not account for is a cache file: rasterizing is
// cheap PER GLYPH, and reading an already-baked sheet is cheaper still.
//
// So the cache path below is load-bearing, not an optimisation on top of an
// optimisation. Without it MsdfFont bakes ~490 ms of msdfgen on every start —
// which is where this whole thread began — and RasterFont would win easily.
constexpr const char* kFontAsset = "fonts/ui.otf";
constexpr const char* kFontCache = "ui.msdfcache";

// The app list, as it was last time the platform was asked. See app_cache.hh.
constexpr const char* kAppCache  = "apps.cache";

// postAppEvent id: the worker thread has a list ready. The host never reads
// it; see host.hh on whose vocabulary this is.
constexpr int kAppsRefreshed = 1;

} // namespace

LauncherApp::LauncherApp(std::unique_ptr<AppSource> source)
    : source_(std::move(source)) {}

LauncherApp::~LauncherApp() { stopWorker(); }

bool LauncherApp::create(std::unique_ptr<Host> host) {
    host_ = std::move(host);
    {
        // Mostly a WAIT, not work: on Android init() blocks until the system
        // hands over a window. Measured anyway, so the rest of the numbers can
        // be read as "after Android was ready" rather than confused with it.
        perf::Span span("host init");
        if (!host_ || !host_->init(this)) return false;
    }

    {
        // Vulkan bring-up: instance, device, swapchain, render pass, pipelines.
        perf::Span span("renderer init");
        renderer_ = std::make_unique<Renderer>(host_->surfaceProvider(), host_->assetReader());
    }

    auto font = std::make_unique<MsdfFont>();
    bool haveFont = false;
    {
        perf::Span span("font atlas");
        // Loads the baked atlas when one is there and bakes (then saves) when
        // it is not — the engine's own cache, which this app was not using.
        const std::string cache = app_paths::stateDir() + kFontCache;
        haveFont = font->generate(host_->assetReader(), kFontAsset, cache.c_str());
    }
    if (haveFont) {
        // Uploading a 4096-wide atlas and building the text pipeline. The other
        // half of what a cold start costs once the bake is off the table.
        perf::Span span("msdf upload");
        renderer_->initMsdf(*font);
        font_ = std::move(font);
    }
    // No font is survivable and not fatal: Canvas falls back to stroked text,
    // which is ugly and legible. A launcher that refuses to start leaves the
    // user with no way to reach the app that would fix it.

    // The list comes off disk first and the platform is asked afterwards, in
    // the background. This is the whole startup story: the first frame owes
    // nothing to the PackageManager.
    {
        perf::Span span("app cache load");
        std::vector<AppEntry> cached;
        if (load_app_cache(appCachePath(), cached) && !cached.empty())
            apps_.set(std::move(cached));
    }
    requestRefresh();

    host_->showWindow();
    return true;
}

void LauncherApp::run() {
    while (running_) {
        // haveWork is false almost always — see the class comment. A blocked
        // pump() is the point, not an optimisation.
        host_->pump(/*haveWork=*/dirty_);
        if (host_->quitRequested()) break;

        if (source_->packagesChanged()) requestRefresh();

        if (dirty_ && surfaceOk_) {
            draw();
            dirty_ = false;
        }
    }
}

// ── State ───────────────────────────────────────────────────────────────────

std::string LauncherApp::appCachePath() const {
    return app_paths::stateDir() + kAppCache;
}

void LauncherApp::requestRefresh() {
    if (!worker_.joinable()) worker_ = std::thread([this] { refreshLoop(); });
    {
        std::lock_guard<std::mutex> lock(workMu_);
        // A second request while one is in flight only re-arms the flag. The
        // platform announces package changes in BURSTS — enabling a component
        // fires one per component — and a burst must cost one extra query, not
        // one per broadcast.
        workWanted_ = true;
    }
    workCv_.notify_one();
}

void LauncherApp::refreshLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lock(workMu_);
            workCv_.wait(lock, [this] { return workWanted_ || workQuit_; });
            if (workQuit_) break;
            workWanted_ = false;
        }

        perf::Span span("app query");
        std::vector<AppEntry> apps = source_->query();

        {
            std::lock_guard<std::mutex> lock(workMu_);
            fresh_      = std::move(apps);
            freshReady_ = true;
        }
        // Wakes a pump() asleep in the kernel; onAppEvent() runs on the main
        // thread, which is the only thread allowed near apps_.
        host_->postAppEvent(kAppsRefreshed);
    }
    source_->onWorkerExit();
}

void LauncherApp::adoptRefresh() {
    std::vector<AppEntry> apps;
    {
        std::lock_guard<std::mutex> lock(workMu_);
        if (!freshReady_) return;
        freshReady_ = false;
        apps        = std::move(fresh_);
        fresh_.clear();
    }
    if (apps.empty()) return;   // a query that failed says nothing about reality

    // Nothing to do is the common case — the list changes when an app is
    // installed, which is a few times a month, and re-applying it would throw
    // away the user's scroll position and redraw for no reason.
    if (apps == apps_.all()) return;

    apps_.set(std::move(apps));
    scroll_ = 0.0f;
    closeMenu();
    dirty_ = true;
    save_app_cache(appCachePath(), apps_.all());
}

void LauncherApp::onAppEvent(int id, intptr_t, intptr_t) {
    if (id == kAppsRefreshed) adoptRefresh();
}

void LauncherApp::setQuery(const std::string& q) {
    if (q == apps_.query()) return;
    apps_.setQuery(q);
    scroll_ = 0.0f;      // a new query is a new list; keeping the old offset
    closeMenu();         // would scroll past the end of it
    dirty_ = true;
}

// Empties the field, and — the part setQuery() cannot do — the IME's own buffer
// with it.
//
// On Android the authoritative text lives in AppShellActivity's off-screen
// EditText, not here: this object only ever learns what that buffer says
// through onTextEditPortable(). Clearing our copy alone left the launcher
// showing a blank field over a keyboard still holding "ap", so the next
// keystroke resurrected it. showKeyboard() re-seeds that buffer, which is what
// actually makes the field empty.
void LauncherApp::clearQuery() {
    setQuery("");
    host_->showKeyboard("", 0);
}

// Raises the IME over whatever the query currently is. Seeding it with the
// existing text is what makes this safe to call at any moment: the EditText on
// the Java side is the authoritative buffer, so re-opening it with a stale or
// empty string would silently rewrite what the user typed.
void LauncherApp::showKeyboard() {
    host_->showKeyboard(apps_.query(), apps_.query().size());
}

void LauncherApp::closeMenu() {
    swallowUp_ = false;
    if (menuRow_ < 0) return;
    menuRow_ = -1;
    dirty_   = true;
}

void LauncherApp::openFirst() {
    if (const AppEntry* e = apps_.first()) {
        source_->launch(*e);
        // Cleared on the way OUT as well as on the way back in. Here, so the
        // frame left behind the launched app is already blank; in
        // onSurfaceRecreated(), because that is the callback that actually
        // fires on the return trip.
        clearQuery();
    }
}

void LauncherApp::openRow(int row) {
    const auto& m = apps_.matches();
    if (row < 0 || row >= (int)m.size()) return;
    source_->launch(*m[(size_t)row]);
    clearQuery();
}

// ── The state drawing and hit-testing share ─────────────────────────────────

LauncherFrame LauncherApp::frame() const {
    LauncherFrame f;
    f.apps    = &apps_;
    f.width   = renderer_ ? renderer_->width()  : 0;
    f.height  = renderer_ ? renderer_->height() : 0;
    f.scroll  = scroll_;
    f.menuRow = menuRow_;

    const SafeInsets in = host_->safeInsets();
    f.insetTop    = in.top;
    f.insetBottom = in.bottom;
    f.insetLeft   = in.left;
    f.insetRight  = in.right;
    f.keyboardInset = host_->keyboardInset();
    return f;
}

// Measures text exactly as draw() does — same font, same MSDF metrics — so the
// menu's tap targets land on its glyphs. A scratch Canvas is the cheapest way
// to reach Canvas::textWidth() without a frame: it appends nothing, because
// nothing is drawn into it.
MenuLine LauncherApp::menuLine() const {
    const LauncherFrame f = frame();
    std::vector<float> scratch;
    Canvas c(scratch, f.width, f.height, nullptr,
             (float)f.insetTop, (float)f.insetBottom,
             (float)f.insetLeft, (float)f.insetRight);
    if (font_) c.useMsdf(font_.get(), nullptr);
    return menu_line(f, metrics_, [&c](std::string_view sv, float size) {
        return c.textWidth(sv, size);
    });
}

// ── Drawing ─────────────────────────────────────────────────────────────────

void LauncherApp::draw() {
    if (!renderer_) return;
    // Only the FIRST one is worth a line: it is the number a user feels, and
    // every frame after it is a redraw of an app already on screen.
    static bool firstFrame = true;
    const LauncherFrame f = frame();
    if (f.width == 0 || f.height == 0) return;

    metrics_ = computeUiMetrics((float)std::min(f.width, f.height));

    curves_.clear();
    quads_.clear();
    Canvas c(curves_, f.width, f.height, nullptr,
             (float)f.insetTop, (float)f.insetBottom,
             (float)f.insetLeft, (float)f.insetRight);
    if (font_) c.useMsdf(font_.get(), &quads_);

    draw_launcher(c, f, metrics_);

    // A glyph this screen had never laid out before — a CJK app name, an
    // accent outside Latin-1 — is recorded by layout() rather than silently
    // dropped. Baking it HERE means it is missing for exactly this one frame
    // and correct from the next, which is also why dirty_ is set again.


    // The MSDF atlas is baked lazily: laying out a glyph for the first time
    // adds cells to it, and those cells are only on the GPU after an upload.
    // Doing it HERE — after the layout above, before the draw below — is the
    // one point in the frame where the atlas and the quads that index it agree.
    // initMsdf() is cheap when nothing is dirty; it patches the changed pages
    // and returns without touching the pipeline.
    if (font_) renderer_->initMsdf(*font_);

    renderer_->draw(curves_, /*rotation=*/0, {}, {}, quads_);

    if (firstFrame) {
        firstFrame = false;
        perf::log_ms("first frame drawn",
                     std::chrono::duration<double, std::milli>(
                         std::chrono::steady_clock::now() - started_).count());
    }
}

// ── Host callbacks ──────────────────────────────────────────────────────────

void LauncherApp::onHostResized() {
    if (renderer_) renderer_->notifyResized();
    dirty_ = true;
}

void LauncherApp::onHostLayoutInvalidated() { dirty_ = true; }
void LauncherApp::onHostExposed()           { dirty_ = true; }

void LauncherApp::onHostReady() {
    // Autofocus, every time. This screen IS the search field, so arriving here
    // with a hidden keyboard would mean one wasted tap on every single launch.
    // Fires again after a resume on Android, which is precisely when it is
    // needed: the user pressed Home to start something.
    clearQuery();
    // No refresh here. create() already seeded the list from the cache and
    // asked the worker for a fresh one; doing it again bought a second full
    // PackageManager query on every cold start and nothing else.
}

// The first moment the input system will listen, which is NOT the first moment
// the app can draw: Android hands over the surface, takes a frame, and only
// then gives the window focus. A showSoftInput() issued before that is answered
// with "Ignoring showSoftInput() ... is not served" in logcat and nothing on
// screen — which is what left the launcher sitting on its home screen with no
// keyboard even though onHostReady() had asked for one.
void LauncherApp::onHostFocusGained() {
    showKeyboard();
}

void LauncherApp::onSurfaceLost() {
    // Android took the window when the user left. CPU state stays, GPU state
    // does not; drawing before it comes back is drawing into nothing.
    surfaceOk_ = false;
    renderer_.reset();
}

bool LauncherApp::onSurfaceRecreated() {
    renderer_ = std::make_unique<Renderer>(host_->surfaceProvider(), host_->assetReader());
    if (font_) renderer_->initMsdf(*font_);   // the atlas died with the device

    // Coming back from a launched app arrives HERE, and nowhere else that the
    // app can act on: onHostReady() is one-shot (AndroidHost guards it with
    // hostReadySignalled_), so it fires on the first launch and never again.
    // Pressing Home must always land on an empty field with the keyboard up,
    // so the state that produces that is re-established here.
    clearQuery();
    closeMenu();
    scroll_ = 0.0f;

    surfaceOk_ = true;
    dirty_     = true;
    return true;
}

void LauncherApp::shutdown() {
    running_ = false;
    stopWorker();
}

void LauncherApp::stopWorker() {
    if (!worker_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(workMu_);
        workQuit_ = true;
    }
    workCv_.notify_one();
    // A query already in flight is waited out rather than abandoned: it is
    // holding JNI local references and a thread attachment, and tearing that
    // down from underneath it is how a clean exit becomes a crash report.
    worker_.join();
}

// ── Input ───────────────────────────────────────────────────────────────────

void LauncherApp::onTextEditPortable(const std::string& text, size_t) {
    // The IME owns the buffer; we adopt it wholesale. Never append — see
    // AppView's comment on why a composing IME's next state is not a suffix of
    // its last one.
    setQuery(text);
}

void LauncherApp::onCharPortable(uint32_t cp) {
    // Desktop only; Android's characters arrive through onTextEditPortable.
    if (cp < 0x20) return;
    std::string q = apps_.query();
    if (cp < 0x80) {
        q.push_back((char)cp);
    } else if (cp < 0x800) {
        q.push_back((char)(0xC0 | (cp >> 6)));
        q.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        q.push_back((char)(0xE0 | (cp >> 12)));
        q.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        q.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        q.push_back((char)(0xF0 | (cp >> 18)));
        q.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        q.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        q.push_back((char)(0x80 | (cp & 0x3F)));
    }
    setQuery(q);
}

void LauncherApp::onKeyDownPortable(int keyCode) {
    if (keyCode == key::Enter) {
        openFirst();
    } else if (keyCode == key::Escape) {
        if (menuRow_ >= 0) closeMenu();
        else               clearQuery();
    } else if (keyCode == key::Backspace) {
        // Desktop only, and only whole codepoints: dropping one byte of a
        // multi-byte character leaves the query invalid UTF-8.
        std::string q = apps_.query();
        while (!q.empty() && (q.back() & 0xC0) == 0x80) q.pop_back();
        if (!q.empty()) q.pop_back();
        setQuery(q);
    }
}

void LauncherApp::onNavBack() {
    // Back on a HOME screen must not finish the activity — there is nothing
    // behind it. It clears the query, and clearing an empty query does nothing.
    if (menuRow_ >= 0) closeMenu();
    else               clearQuery();
}

void LauncherApp::onLButtonDown(int x, int y) {
    pressX_     = x;
    pressY_     = y;
    pressMoved_ = false;
    swallowUp_  = false;
    pressRow_   = row_at(frame(), metrics_, (float)y);

    if (menuRow_ >= 0) return;              // the menu is open; the up decides
    if (pressRow_ >= 0) host_->startTimer(kLongPressTimer, kLongPressMs);
}

void LauncherApp::onTimer(int timerId) {
    if (timerId != kLongPressTimer) return;
    host_->stopTimer(kLongPressTimer);
    if (pressRow_ < 0 || pressMoved_) return;
    menuRow_   = pressRow_;
    pressRow_  = -1;   // the finger's release must not also launch the app
    swallowUp_ = true; // …nor be read as a tap on the menu it just opened
    dirty_     = true;
}

void LauncherApp::onLButtonUp(int x, int y) {
    host_->stopTimer(kLongPressTimer);

    // The lift that completed the long press. The menu is on screen under the
    // finger, but this release chose nothing.
    if (swallowUp_) {
        swallowUp_ = false;
        pressRow_  = -1;
        return;
    }

    if (menuRow_ >= 0) {
        const auto& matches = apps_.matches();
        const MenuHit hit = menu_hit(menuLine(), (float)x, (float)y);
        if (hit != MenuHit::None && menuRow_ < (int)matches.size()) {
            const AppEntry& e = *matches[(size_t)menuRow_];
            if (hit == MenuHit::Info) source_->openInfo(e);
            else                      source_->uninstall(e);
        }
        closeMenu();
        return;
    }

    const float slop = metrics_.space(kTapSlop);
    if (pressMoved_ || std::abs((float)x - pressX_) > slop ||
        std::abs((float)y - pressY_) > slop) {
        pressRow_ = -1;
        return;
    }
    if (pressRow_ >= 0 && pressRow_ == row_at(frame(), metrics_, (float)y)) {
        openRow(pressRow_);
    } else if (pressRow_ < 0) {
        // A tap that hit no row. On a screen whose only other element is the
        // query line, the one thing it can mean is "I want to type" — most
        // often after the keyboard was dismissed with the system's own back
        // gesture, which leaves the launcher on screen with no way back to an
        // IME. Every empty pixel re-opens it, not just the query line: aiming
        // at a word of text is a needless demand on a home screen.
        perf::log_ms("tap: no row, raising keyboard", 0.0);
        showKeyboard();
    }
    pressRow_ = -1;
}

void LauncherApp::onDragEnd(int dx, int dy) {
    // The host already applied its own slop, so reaching here at all means the
    // gesture was a scroll (or the system cancelled it). Cancelling the pending
    // press is the whole job: the stroke's movement already arrived as wheel
    // deltas, and scrolling by this summary as well would apply it twice — the
    // list would jump by the stroke's length at the end of every swipe.
    (void)dx;
    (void)dy;
    pressMoved_ = true;
    pressRow_   = -1;
    swallowUp_  = true;   // this release ends a gesture, not a press
    host_->stopTimer(kLongPressTimer);
}

void LauncherApp::scrollBy(float dy) {
    scroll_ -= dy;
    const LauncherFrame f = frame();
    // The open menu occupies one row's worth of the list's height, and scrolling
    // must be able to reach past it.
    const float rows = (float)apps_.matches().size() + (menuRow_ >= 0 ? 1.0f : 0.0f);
    const float maxScroll =
        std::max(0.0f, rows * row_height(metrics_) -
                           ((float)f.height - list_top(f, metrics_)));
    scroll_ = std::min(std::max(scroll_, 0.0f), maxScroll);
    dirty_  = true;
}

void LauncherApp::onMouseWheel(int, int, int delta) {
    scrollBy((float)delta);
}
