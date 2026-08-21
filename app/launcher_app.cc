#include "launcher_app.hh"

#include <algorithm>
#include <cmath>

#include "host.hh"       // app_shell
#include "keys.hh"       // vk_canvas
#include "msdf.hh"       // vulkan_font_engine
#include "renderer.hh"   // vk_canvas

namespace {

// Timer ids. The host never reads them; see host.hh on whose vocabulary this is.
constexpr int kLongPressTimer = 1;
constexpr int kLongPressMs    = 420;

// A tap that travelled this far was a scroll, not a press. In authored pixels;
// scaled through UiMetrics before use.
constexpr float kTapSlop = 16.0f;

// The whole palette. Pure black, and text that is white without being a light
// source — an OLED panel at 3am is the environment this runs in.
constexpr Color kBackground = {0.00f, 0.00f, 0.00f, 1.00f};
constexpr Color kText       = {0.92f, 0.92f, 0.92f, 1.00f};
constexpr Color kDim        = {0.38f, 0.38f, 0.38f, 1.00f};

// Where the font comes from. Generated into the state dir on first run — a
// bake takes a moment, and doing it on every cold start of a HOME screen is
// exactly the wrong place to spend it.
constexpr const char* kFontAsset = "fonts/ui.otf";

} // namespace

LauncherApp::LauncherApp(std::unique_ptr<AppSource> source)
    : source_(std::move(source)) {}

LauncherApp::~LauncherApp() = default;

bool LauncherApp::create(std::unique_ptr<Host> host) {
    host_ = std::move(host);
    if (!host_ || !host_->init(this)) return false;

    renderer_ = std::make_unique<Renderer>(host_->surfaceProvider(), host_->assetReader());

    auto font = std::make_unique<MsdfFont>();
    if (font->generate(host_->assetReader(), kFontAsset)) {
        renderer_->initMsdf(*font);
        font_ = std::move(font);
    }
    // No font is survivable and not fatal: Canvas falls back to stroked text,
    // which is ugly and legible. A launcher that refuses to start leaves the
    // user with no way to reach the app that would fix it.

    refreshApps();
    host_->showWindow();
    return true;
}

void LauncherApp::run() {
    while (running_) {
        // haveWork is false almost always — see the class comment. A blocked
        // pump() is the point, not an optimisation.
        host_->pump(/*haveWork=*/dirty_);
        if (host_->quitRequested()) break;

        if (source_->packagesChanged()) refreshApps();

        if (dirty_ && surfaceOk_) {
            draw();
            dirty_ = false;
        }
    }
}

// ── State ───────────────────────────────────────────────────────────────────

void LauncherApp::refreshApps() {
    apps_.set(source_->query());
    scroll_ = 0.0f;
    dirty_  = true;
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

void LauncherApp::closeMenu() {
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

// ── Geometry: one source of truth for drawing AND hit-testing ───────────────

float LauncherApp::rowHeight() const { return metrics_.space(56.0f); }

float LauncherApp::listTop() const {
    return (float)host_->safeInsets().top + metrics_.space(28.0f)   // field
           + metrics_.text.header + metrics_.space(24.0f);
}

float LauncherApp::rowTop(int row) const {
    return listTop() + (float)row * rowHeight() - scroll_;
}

int LauncherApp::rowAt(float y) const {
    const float rel = y - listTop() + scroll_;
    if (rel < 0.0f) return -1;
    const int row = (int)(rel / rowHeight());
    return row < (int)apps_.matches().size() ? row : -1;
}

// ── Drawing ─────────────────────────────────────────────────────────────────

void LauncherApp::draw() {
    if (!renderer_) return;

    const SafeInsets in = host_->safeInsets();
    const uint32_t w = renderer_->width();
    const uint32_t h = renderer_->height();
    if (w == 0 || h == 0) return;

    metrics_ = computeUiMetrics((float)std::min(w, h));

    curves_.clear();
    quads_.clear();
    Canvas c(curves_, w, h, nullptr,
             (float)in.top, (float)in.bottom, (float)in.left, (float)in.right);
    if (font_) c.useMsdf(font_.get(), &quads_);

    c.clear(kBackground);

    // The query, as a plain line of text. No box, no underline, no caret glyph
    // borrowed from a desktop: the keyboard is already on screen saying where
    // the text goes.
    const float fieldY = (float)in.top + metrics_.space(28.0f) + metrics_.text.header;
    if (apps_.query().empty())
        c.text("search", c.left() + c.pad(), fieldY, metrics_.text.header, kDim);
    else
        c.text(apps_.query(), c.left() + c.pad(), fieldY, metrics_.text.header, kText);

    // The results. Only the rows that can be on screen are emitted — a phone
    // with 300 apps would otherwise pay for 300 text layouts per frame to show
    // twelve of them.
    const auto& m = apps_.matches();
    const float rowH = rowHeight();
    const float top  = listTop();
    const float bot  = (float)h - (float)in.bottom - (float)host_->keyboardInset();

    const int firstVisible = std::max(0, (int)((scroll_ - (bot - top)) / rowH));
    for (int i = firstVisible; i < (int)m.size(); ++i) {
        const float y = rowTop(i);
        if (y > bot) break;
        if (y + rowH < top - rowH) continue;

        const bool hot = (i == menuRow_);
        c.text(m[(size_t)i]->label, c.left() + c.pad(), y + rowH * 0.7f,
               metrics_.text.body, hot ? kText : kText);

        if (hot) {
            // The long-press menu: two more words, indented under the row. A
            // dialog here would be the only non-text thing in the program.
            const float my = y + rowH + metrics_.text.secondary * 0.9f;
            c.text("info", c.left() + c.pad() * 2.0f, my, metrics_.text.secondary, kDim);
            c.text("uninstall",
                   c.left() + c.pad() * 2.0f + c.textWidth("info    ", metrics_.text.secondary),
                   my, metrics_.text.secondary, kDim);
        }
    }

    if (m.empty() && !apps_.query().empty())
        c.text("no match", c.left() + c.pad(), top + rowH * 0.7f, metrics_.text.body, kDim);

    // The MSDF atlas is baked lazily: laying out a glyph for the first time
    // adds cells to it, and those cells are only on the GPU after an upload.
    // Doing it HERE — after the layout above, before the draw below — is the
    // one point in the frame where the atlas and the quads that index it agree.
    // initMsdf() is cheap when nothing is dirty; it patches the changed pages
    // and returns without touching the pipeline.
    if (font_) renderer_->initMsdf(*font_);

    renderer_->draw(curves_, /*rotation=*/0, {}, {}, quads_);
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
    refreshApps();
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

void LauncherApp::shutdown() { running_ = false; }

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
    pressRow_   = rowAt((float)y);

    if (menuRow_ >= 0) return;              // the menu is open; the up decides
    if (pressRow_ >= 0) host_->startTimer(kLongPressTimer, kLongPressMs);
}

void LauncherApp::onTimer(int timerId) {
    if (timerId != kLongPressTimer) return;
    host_->stopTimer(kLongPressTimer);
    if (pressRow_ < 0 || pressMoved_) return;
    menuRow_  = pressRow_;
    pressRow_ = -1;   // the finger's release must not also launch the app
    dirty_    = true;
}

void LauncherApp::onLButtonUp(int x, int y) {
    host_->stopTimer(kLongPressTimer);

    if (menuRow_ >= 0) {
        const auto& m = apps_.matches();
        const float rowH = rowHeight();
        const float my   = rowTop(menuRow_) + rowH;
        const bool onMenuLine = (float)y >= my && (float)y <= my + rowH;
        if (onMenuLine && menuRow_ < (int)m.size()) {
            const AppEntry& e = *m[(size_t)menuRow_];
            // Two words, split at the midpoint of the row they sit on. Anything
            // finer would need hit rects the drawing code does not produce.
            if ((float)x < (float)renderer_->width() * 0.5f) source_->openInfo(e);
            else                                             source_->uninstall(e);
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
    if (pressRow_ >= 0 && pressRow_ == rowAt((float)y)) openRow(pressRow_);
    pressRow_ = -1;
}

void LauncherApp::onDragEnd(int dx, int dy) {
    // The host already applied its own slop, so reaching here at all means the
    // gesture was a scroll. Cancelling the pending press is the point.
    (void)dx;
    pressMoved_ = true;
    pressRow_   = -1;
    host_->stopTimer(kLongPressTimer);

    scroll_ -= (float)dy;
    const float maxScroll =
        std::max(0.0f, (float)apps_.matches().size() * rowHeight() -
                           ((float)renderer_->height() - listTop()));
    scroll_ = std::min(std::max(scroll_, 0.0f), maxScroll);
    dirty_  = true;
}

void LauncherApp::onMouseWheel(int, int, int delta) {
    onDragEnd(0, delta);
}
