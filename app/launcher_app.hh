#pragma once
#include <memory>
#include <string>
#include <vector>

#include "app_list.hh"
#include "app_source.hh"
#include "app_view.hh"     // app_shell
#include "canvas.hh"       // vk_canvas
#include "ui_metrics.hh"   // app_shell

class Host;
class Renderer;
class TextFont;

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
    void onSurfaceLost() override;
    bool onSurfaceRecreated() override;

    void onCharPortable(uint32_t codepoint) override;
    void onKeyDownPortable(int keyCode) override;
    void onTextEditPortable(const std::string& text, size_t cursorByte) override;

    void onLButtonDown(int x, int y) override;
    void onLButtonUp(int x, int y) override;
    void onMouseWheel(int x, int y, int delta) override;
    void onDragEnd(int dx, int dy) override;
    void onTimer(int timerId) override;
    void onNavBack() override;

private:
    void draw();
    void refreshApps();
    void setQuery(const std::string& q);   // one place, so the IME and the keys agree
    void clearQuery();                     // …and the one that also empties the IME's buffer
    void openFirst();
    void openRow(int row);
    void closeMenu();

    // Which result a screen y lands on, or -1. The single piece of geometry
    // both drawing and hit-testing read, so a row can never be drawn in one
    // place and tapped in another.
    int rowAt(float y) const;
    float rowTop(int row) const;
    float rowHeight() const;
    float listTop() const;

    std::unique_ptr<Host>      host_;
    std::unique_ptr<Renderer>  renderer_;
    std::unique_ptr<TextFont>  font_;
    std::unique_ptr<AppSource> source_;

    AppList apps_;

    std::vector<float> curves_;   // reused across frames; cleared, never freed
    std::vector<float> quads_;

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
};
