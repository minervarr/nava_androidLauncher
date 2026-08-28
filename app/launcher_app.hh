#pragma once
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app_list.hh"
#include "app_source.hh"
#include "app_view.hh"      // app_shell
#include "launcher_draw.hh"
#include "ui_metrics.hh"    // app_shell

class Host;
class Renderer;
class MsdfFont;

// ── The launcher ─────────────────────────────────────────────────────────────
//
// One screen, and it is a text field with a list under it. Black background,
// white text, no icons, no clock, no favourites. Type, and the list narrows;
// press Enter, and the first line opens.
//
// create() and run() are the APP's, not app_shell's — the frame loop belongs
// here because only this program knows what counts as work, and for a launcher
// the answer is "almost never": it draws when the query changes, when a finger
// moves, and at no other time. A launcher that renders at 60fps to show six
// unchanging words is a launcher that empties a battery from the home screen.
class LauncherApp : public AppView {
public:
    explicit LauncherApp(std::unique_ptr<AppSource> source);
    ~LauncherApp() override;

    // Builds the host, the renderer and the font. False means there is nothing
    // to run and the process should exit non-zero.
    bool create(std::unique_ptr<Host> host);
    void run();

    Host* host() { return host_.get(); }

    // Starts with the field already filled. Only the desktop build uses it —
    // there is no keyboard injection on this machine, so a command line is how
    // the filtered state gets looked at at all.
    void setInitialQuery(const std::string& q) { apps_.setQuery(q); }

    // ── AppView ─────────────────────────────────────────────────────────────
    void onHostResized() override;
    void onHostLayoutInvalidated() override;
    void onHostExposed() override;
    void shutdown() override;

    void onHostReady() override;
    void onHostFocusGained() override;
    void onSurfaceLost() override;
    bool onSurfaceRecreated() override;

    void onCharPortable(uint32_t codepoint) override;
    void onKeyDownPortable(int keyCode) override;
    void onTextEditPortable(const std::string& text, size_t cursorByte) override;

    void onAppEvent(int id, intptr_t p1, intptr_t p2) override;

    void onLButtonDown(int x, int y) override;
    void onLButtonUp(int x, int y) override;
    void onMouseWheel(int x, int y, int delta) override;
    void onDragEnd(int dx, int dy) override;
    void onTimer(int timerId) override;
    void onNavBack() override;

private:
    void draw();

    // Asks the worker thread for a fresh list. Returns immediately; the answer
    // arrives later through onAppEvent(). Called at startup and whenever the
    // platform says a package appeared or vanished.
    void requestRefresh();
    // The worker's loop, and the main-thread half that adopts what it produced.
    void refreshLoop();
    void adoptRefresh();
    // Asks the worker to finish and joins it. Idempotent, and called from both
    // shutdown() and the destructor: run() can also end through the host's
    // quitRequested(), which never reaches shutdown(), and a std::thread that
    // is still joinable when it is destroyed takes the process down with it.
    void stopWorker();
    void setQuery(const std::string& q);   // one place, so the IME and the keys agree
    void clearQuery();                     // …and the one that also empties the IME's buffer
    void showKeyboard();                   // re-raise the IME over the current query
    void openFirst();
    void openRow(int row);
    void closeMenu();

    // Scrolls the list and clamps it to its ends. The only place scroll_ moves:
    // a stroke's increments arrive as wheel deltas, and the onDragEnd() summary
    // of the same stroke must not apply them a second time.
    void scrollBy(float dy);

    // Everything drawing and hit-testing need, rebuilt from the host. Both go
    // through launcher_draw.hh's helpers, so a row cannot be drawn in one place
    // and tapped in another.
    LauncherFrame frame() const;

    // Where the app list and the font atlas are remembered between runs.
    std::string appCachePath() const;

    // The long-press menu's geometry, measured with the real font. Zero-height
    // when no menu is open, which menu_hit() reads as a miss.
    MenuLine menuLine() const;

    std::unique_ptr<Host>      host_;
    std::unique_ptr<Renderer>  renderer_;
    std::unique_ptr<MsdfFont> font_;
    std::unique_ptr<AppSource> source_;

    AppList apps_;

    std::vector<float> curves_;   // reused across frames; cleared, never freed
    std::vector<float> quads_;

    // When the process started counting, for the one measurement that matters:
    // how long the user waited to see the launcher.
    std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();

    UiMetrics metrics_;
    float     scroll_      = 0.0f;   // pixels the list is shifted up by
    bool      running_     = true;
    bool      dirty_       = true;
    bool      surfaceOk_   = true;

    // The long-press menu: two words under the row they belong to. Not a
    // dialog, because a dialog would be the only non-text thing on screen.
    int  menuRow_          = -1;
    int  pressRow_         = -1;
    bool pressMoved_       = false;
    int  pressX_ = 0, pressY_ = 0;
    // The release that ENDED a long press must not also be read as a choice in
    // the menu that press just opened — the finger is still over the row, which
    // is a miss, and a miss closes the menu. Set when the timer fires, spent by
    // the next release.
    bool swallowUp_        = false;

    // ── The refresh thread ──────────────────────────────────────────────────
    //
    // Querying the platform for every installed app is the single most
    // expensive thing this program does, and it must never be the reason a
    // keystroke waits. One thread does it, always off the main thread, and
    // hands the result back through Host::postAppEvent() — the same doorbell
    // that wakes a pump() sleeping in the kernel.
    std::thread             worker_;
    std::mutex              workMu_;
    std::condition_variable workCv_;
    bool                    workWanted_ = false;   // a query has been asked for
    bool                    workQuit_   = false;
    std::vector<AppEntry>   fresh_;                // filled by the worker
    bool                    freshReady_ = false;
};
