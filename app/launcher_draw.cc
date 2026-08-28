#include "launcher_draw.hh"

#include <algorithm>

#include "canvas.hh"   // vk_canvas

namespace {

// The whole palette. Pure black, and text that is white without being a light
// source — an OLED panel at 3am is the environment this runs in.
constexpr Color kBackground = {0.00f, 0.00f, 0.00f, 1.00f};
constexpr Color kText       = {0.92f, 0.92f, 0.92f, 1.00f};
constexpr Color kDim        = {0.38f, 0.38f, 0.38f, 1.00f};

// Canvas's content geometry, recomputed from the frame. Hit-testing has no
// Canvas, and both sides must agree on where the centre line is — see the
// TextWidth comment in the header.
float content_left(const LauncherFrame& f) { return (float)f.insetLeft; }
float content_width(const LauncherFrame& f) {
    return (float)f.width - (float)f.insetLeft - (float)f.insetRight;
}
// Everything on screen is laid out around this. A text-only launcher has no
// columns, no icons and no second element per row to align against, so a left
// margin is an arbitrary choice; the centre is not.
float content_center(const LauncherFrame& f) {
    return content_left(f) + content_width(f) * 0.5f;
}
float content_pad(const LauncherFrame& f) { return content_width(f) * 0.025f; }

// The gap between the two menu words, as a run of spaces — the same string the
// drawing used to advance by.
constexpr const char* kMenuGap = "    ";

} // namespace

// Grown with the type: rows carry text.title now, and 56 authored pixels that
// comfortably held text.body left the larger glyphs nearly touching.
float row_height(const UiMetrics& m) { return m.space(76.0f); }

// How far below the top edge the query line sits.
//
// 28 authored pixels put it inside the strip Android reserves for the
// status-bar pull-down gesture, and the system swallows every touch there: on
// the S23 a tap at y=230 opened the notification shade and never reached the
// app, while the same tap at y=320 did. That made the query line — the one
// thing on this screen a user would press to start typing — untappable, and no
// amount of correct hit-testing inside the app could have fixed it.
//
// safeInsets() cannot express this: the gesture strip is not a cutout, and
// app_shell deliberately reports only the cutout (see its CLAUDE.md). So it is
// a margin, sized to clear the strip on the phones that have the deepest one.
constexpr float kQueryTopMargin = 150.0f;

float list_top(const LauncherFrame& f, const UiMetrics& m) {
    return (float)f.insetTop + m.space(kQueryTopMargin) + m.text.header +
           m.space(24.0f);
}

FieldBand field_band(const LauncherFrame& f, const UiMetrics& m) {
    FieldBand b;
    b.y      = (float)f.insetTop;
    b.height = list_top(f, m) - b.y;
    return b;
}

// An open menu pushes everything under it down by one row's worth, because
// that is where its two words are drawn. Without this the row below the menu
// is drawn — and tapped — straight through it.
float menu_shift(const LauncherFrame& f, const UiMetrics& m, int row) {
    return (f.menuRow >= 0 && row > f.menuRow) ? row_height(m) : 0.0f;
}

float row_top(const LauncherFrame& f, const UiMetrics& m, int row) {
    return list_top(f, m) + (float)row * row_height(m) + menu_shift(f, m, row) -
           f.scroll;
}

// The bottom of the band the list may occupy. The keyboard is not a safe inset,
// so it has to be subtracted separately — see app_shell's CLAUDE.md on why the
// two are never summed into one field.
static float list_bottom(const LauncherFrame& f) {
    return (float)f.height - (float)f.insetBottom - (float)f.keyboardInset;
}

int row_at(const LauncherFrame& f, const UiMetrics& m, float y) {
    if (!f.apps) return -1;
    // Nothing is DRAWN below the keyboard, so nothing may be TAPPED there
    // either. Without this the rows that exist but never made it onto the
    // screen still answered taps: with the IME up, a touch in the lower half
    // launched an app the user could not see — and the tap that was meant to
    // mean "let me type" was the worst of them.
    if (y > list_bottom(f)) return -1;

    const float rel = y - list_top(f, m) + f.scroll;
    if (rel < 0.0f) return -1;
    int row = (int)(rel / row_height(m));
    if (f.menuRow >= 0 && row > f.menuRow) {
        if (row == f.menuRow + 1) return -1;   // the menu's own band
        row -= 1;
    }
    return row < (int)f.apps->matches().size() ? row : -1;
}

MenuLine menu_line(const LauncherFrame& f, const UiMetrics& m, const TextWidth& tw) {
    MenuLine ml;
    if (f.menuRow < 0 || !tw) return ml;
    ml.height    = row_height(m);
    ml.y         = row_top(f, m, f.menuRow) + ml.height;

    // Centred as a PAIR under the row they belong to, so the two words read as
    // one line rather than as two independently centred ones. Laid out ONCE,
    // here; draw_launcher() renders these numbers rather than recomputing
    // them, which is what keeps the tap and the glyph together.
    ml.infoW      = tw("info", m.text.body);
    ml.uninstallW = tw("uninstall", m.text.body);
    const float gap   = tw(kMenuGap, m.text.body);
    const float total = ml.infoW + gap + ml.uninstallW;

    ml.infoX      = content_center(f) - total * 0.5f;
    ml.uninstallX = ml.infoX + ml.infoW + gap;
    ml.baselineY  = ml.y + m.text.body * 0.9f;
    return ml;
}

MenuHit menu_hit(const MenuLine& ml, float x, float y) {
    if (ml.height <= 0.0f) return MenuHit::None;
    if (y < ml.y || y > ml.y + ml.height) return MenuHit::None;

    // The gap is split down the middle, and each word keeps a word's worth of
    // slack on its outer edge.
    const float mid   = (ml.infoX + ml.infoW + ml.uninstallX) * 0.5f;
    const float slack = ml.infoW;
    if (x < ml.infoX - slack) return MenuHit::None;
    if (x < mid)              return MenuHit::Info;
    if (x <= ml.uninstallX + ml.uninstallW + slack) return MenuHit::Uninstall;
    return MenuHit::None;
}

void draw_launcher(Canvas& c, const LauncherFrame& f, const UiMetrics& m) {
    c.clear(kBackground);
    if (!f.apps) return;

    // The query, as a plain line of text. No box, no underline, no caret glyph
    // borrowed from a desktop: the keyboard is already on screen saying where
    // the text goes.
    // Left-aligned, unlike everything below it. The list is a column of
    // choices and centring it reads as one; the query is a line being TYPED,
    // and text that grows from the middle outwards as you type does not.
    const float cx     = content_center(f);
    const float fieldX = c.left() + c.pad();
    const float fieldY = (float)f.insetTop + m.space(kQueryTopMargin) + m.text.header;
    if (f.apps->query().empty())
        c.text("Search", fieldX, fieldY, m.text.header, kDim);
    else
        c.text(f.apps->query(), fieldX, fieldY, m.text.header, kText);

    const auto& matches = f.apps->matches();
    const MenuLine ml = menu_line(f, m, [&c](std::string_view s, float size) {
        return c.textWidth(s, size);
    });
    const float rowH = row_height(m);
    const float top  = list_top(f, m);
    // The keyboard is not an inset the safe area knows about, so the bottom of
    // the usable list is the drawable minus the cutout minus the IME.
    const float bot  = (float)f.height - (float)f.insetBottom - (float)f.keyboardInset;

    // The list scrolls UNDER the query, which therefore has to be protected
    // from it. Without this a scrolled row is drawn straight over the search
    // text — the headless capture of a sixty-app list is what showed it.
    // The clip is the safety net (tile granularity, ~16px); the loop below
    // skips whole rows, which is what actually keeps the layout cheap.
    c.setClip(c.left(), top, c.w(), bot - top);   // x, y, w, h

    // Only the rows that can be on screen are emitted — a phone with 300 apps
    // would otherwise pay for 300 text layouts per frame to show twelve.
    // -2 rather than -1: an open menu shifts the rows under it down by one, so
    // the first row that can be on screen is one earlier than the offset alone
    // suggests. Cheap insurance; the break below is what bounds the loop.
    const int firstVisible = std::max(0, (int)(f.scroll / rowH) - 2);
    for (int i = firstVisible; i < (int)matches.size(); ++i) {
        const float y = row_top(f, m, i);
        if (y > bot) break;
        if (y + rowH < top) continue;

        const AppEntry& e = *matches[(size_t)i];
        const float baseline = y + rowH * 0.68f;

        if (e.disambiguator.empty()) {
            c.textCentered(e.label, cx, baseline, m.text.title, kText);
        } else {
            // Label and package are centred as ONE run, not centred separately:
            // two independently centred pieces would put the package under the
            // label's middle and read as a second row.
            const float labelW = c.textWidth(e.label, m.text.title);
            const float gap    = m.space(14.0f);
            const float pkgW   = c.textWidth(e.disambiguator, m.text.secondary);
            const float startX = cx - (labelW + gap + pkgW) * 0.5f;
            c.text(e.label, startX, baseline, m.text.title, kText);
            // Dim and a size down: there to be read when two lines are
            // identical, and to be ignored the rest of the time.
            c.text(e.disambiguator, startX + labelW + gap, baseline,
                   m.text.secondary, kDim);
        }

        if (i == f.menuRow) {
            // The long-press menu: two more words, indented under the row. A
            // dialog here would be the only non-text thing in the program.
            // Positions come from menu_line() — the same call the tap uses.
            c.text("info", ml.infoX, ml.baselineY, m.text.body, kDim);
            c.text("uninstall", ml.uninstallX, ml.baselineY, m.text.body, kDim);
        }
    }

    c.clearClip();

    if (matches.empty() && !f.apps->query().empty())
        c.textCentered("no match", cx, top + rowH * 0.68f, m.text.title, kDim);
}
