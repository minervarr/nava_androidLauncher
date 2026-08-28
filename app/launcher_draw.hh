#pragma once
#include <cstdint>
#include <functional>
#include <string_view>

#include "app_list.hh"
#include "ui_metrics.hh"   // app_shell

class Canvas;

// ── The launcher's appearance, separated from its lifetime ───────────────────
//
// Everything drawing needs, and nothing else — no Host, no Renderer, no window.
// LauncherApp fills this in from its host each frame; the headless capture tool
// fills it in from a literal, which is the only reason it can produce a picture
// of a state that would otherwise require a phone, a keyboard and a finger.
//
// The geometry helpers live here too, next to the code that draws with them.
// They used to be private methods beside the drawing, which is the arrangement
// that lets a row be drawn in one place and tapped in another: hit-testing is
// the same arithmetic, and it must be the same code.
struct LauncherFrame {
    const AppList* apps = nullptr;

    uint32_t width  = 0;   // the drawable, in pixels
    uint32_t height = 0;

    // The display cutout. Never a system bar, never the IME — see safe_area.hh.
    int insetTop = 0, insetBottom = 0, insetLeft = 0, insetRight = 0;
    // How much of the bottom the on-screen keyboard is covering.
    int keyboardInset = 0;

    float scroll  = 0.0f;  // pixels the list is shifted up by
    int   menuRow = -1;    // the row whose info/uninstall line is showing, or -1
};

// One row's height, and where the list starts. Both scale with the metrics,
// which is what makes the same layout work on a 720p phone and a 1440p one.
float row_height(const UiMetrics& m);
float list_top(const LauncherFrame& f, const UiMetrics& m);
float row_top(const LauncherFrame& f, const UiMetrics& m, int row);

// The band the query line occupies: from the top of the drawable down to where
// the list starts. Tapping it re-opens the keyboard, so it has to be geometry
// both sides can read rather than a number the tap handler invents.
struct FieldBand { float y = 0.0f, height = 0.0f; };
FieldBand field_band(const LauncherFrame& f, const UiMetrics& m);

// Which result a screen y lands on, or -1 for none. The open menu's own band
// belongs to the menu, not to a row, and answers -1 here.
int row_at(const LauncherFrame& f, const UiMetrics& m, float y);

// Measures a run of text at a size — Canvas::textWidth, passed as a callable.
// It is what lets hit-testing run the SAME arithmetic as drawing without
// owning a Canvas, which is the property the menu lost when it hit-tested
// against a hardcoded half-screen split while drawing both words flush left.
using TextWidth = std::function<float(std::string_view, float)>;

// Where the two words of the long-press menu sit, so a tap can be tested
// against exactly what was drawn. Text geometry, not touch targets: the pixels
// the glyphs occupy. menu_hit() below is what widens them for a finger.
struct MenuLine {
    float y = 0.0f, height = 0.0f;   // the line's vertical band
    float baselineY = 0.0f;          // where both words sit
    float infoX = 0.0f, infoW = 0.0f;
    float uninstallX = 0.0f, uninstallW = 0.0f;
};
MenuLine menu_line(const LauncherFrame& f, const UiMetrics& m, const TextWidth& tw);

// What a tap at (x, y) hit. A word of `text.body` is a few millimetres
// tall, so the bands are widened to the whole line vertically and to the gap
// between the words horizontally — an exact glyph rect would be untappable.
enum class MenuHit { None, Info, Uninstall };
MenuHit menu_hit(const MenuLine& ml, float x, float y);

// Draws one complete frame into `c`. Emits nothing else: no clear of the quad
// buffers, no renderer call — the caller owns both, because the capture tool
// and the app hand their output to different places.
void draw_launcher(Canvas& c, const LauncherFrame& f, const UiMetrics& m);
