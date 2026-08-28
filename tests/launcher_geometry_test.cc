// The launcher's geometry, tested the way app_list is: plain assert(), Debug
// only, no framework.
//
// It exists because of a bug that a screenshot could not have caught. The
// long-press menu drew "info" and "uninstall" flush left, one pad apart, while
// the tap that chose between them split the screen down the MIDDLE — so the
// word "uninstall" sat entirely in the half that meant "info", and uninstall
// was reachable only by tapping empty space. Drawing and hit-testing must run
// the same arithmetic; these asserts are what says so.
#include "../app/app_list.hh"
#include "../app/launcher_draw.hh"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

// A stand-in for Canvas::textWidth. The real one needs a font, a renderer and
// an atlas; what the geometry cares about is only that a longer word is wider,
// which a fixed advance per character reproduces exactly.
float fake_width(std::string_view s, float size) {
    return (float)s.size() * size * 0.5f;
}

AppList sample() {
    AppList apps;
    apps.set({
        {"Calendar", "com.a.calendar", "com.a.calendar.Main"},
        {"Camera",   "com.a.camera",   "com.a.camera.Main"},
        {"Clock",    "com.a.clock",    "com.a.clock.Main"},
        {"Files",    "com.a.files",    "com.a.files.Main"},
    });
    return apps;
}

LauncherFrame phone(const AppList& apps, int menuRow) {
    LauncherFrame f;
    f.apps    = &apps;
    f.width   = 1080;
    f.height  = 2400;
    f.menuRow = menuRow;
    return f;
}

} // namespace

int main() {
    const AppList apps = sample();
    const UiMetrics m  = computeUiMetrics(1080.0f);
    const float rowH   = row_height(m);

    // ── Rows, with no menu open ──────────────────────────────────────────────
    {
        const LauncherFrame f = phone(apps, -1);
        for (int i = 0; i < 4; ++i) {
            const float top = row_top(f, m, i);
            assert(row_at(f, m, top + 1.0f)        == i);
            assert(row_at(f, m, top + rowH - 1.0f) == i);
        }
        assert(row_at(f, m, list_top(f, m) - 1.0f) == -1);   // above the list
        assert(row_at(f, m, row_top(f, m, 4) + 1.0f) == -1); // past the last match
    }

    // ── An open menu occupies a band of its own ─────────────────────────────
    //
    // The row under the menu moves down by exactly the menu's height, and the
    // band the menu took over belongs to no row.
    {
        const LauncherFrame f = phone(apps, 1);
        assert(row_at(f, m, row_top(f, m, 1) + 1.0f) == 1);

        const MenuLine ml = menu_line(f, m, fake_width);
        assert(ml.height > 0.0f);
        assert(row_at(f, m, ml.y + rowH * 0.5f) == -1);      // the menu's own band
        assert(row_top(f, m, 2) >= ml.y + ml.height - 0.01f);
        assert(row_at(f, m, row_top(f, m, 2) + 1.0f) == 2);
        assert(row_at(f, m, row_top(f, m, 3) + 1.0f) == 3);
    }

    // ── The menu's two words ────────────────────────────────────────────────
    {
        const LauncherFrame f = phone(apps, 1);
        const MenuLine ml = menu_line(f, m, fake_width);

        // Drawn in reading order, not overlapping, and both on screen.
        assert(ml.infoX > 0.0f);
        assert(ml.uninstallX > ml.infoX + ml.infoW);
        assert(ml.uninstallX + ml.uninstallW < (float)f.width);
        assert(ml.baselineY > ml.y && ml.baselineY < ml.y + ml.height);

        // The regression: the centre of each drawn word must hit that word.
        assert(menu_hit(ml, ml.infoX + ml.infoW * 0.5f, ml.y + ml.height * 0.5f)
               == MenuHit::Info);
        assert(menu_hit(ml, ml.uninstallX + ml.uninstallW * 0.5f, ml.y + ml.height * 0.5f)
               == MenuHit::Uninstall);

        // Every end of both words, too — a tap lands on a letter, not a centre.
        assert(menu_hit(ml, ml.infoX, ml.y + 1.0f) == MenuHit::Info);
        assert(menu_hit(ml, ml.uninstallX, ml.y + 1.0f) == MenuHit::Uninstall);
        assert(menu_hit(ml, ml.uninstallX + ml.uninstallW, ml.y + 1.0f)
               == MenuHit::Uninstall);

        // Off the line vertically, and far off to the right: neither word.
        assert(menu_hit(ml, ml.infoX, ml.y - 1.0f)              == MenuHit::None);
        assert(menu_hit(ml, ml.infoX, ml.y + ml.height + 1.0f)  == MenuHit::None);
        assert(menu_hit(ml, (float)f.width - 1.0f, ml.y + 1.0f) == MenuHit::None);

        // A closed menu is a miss everywhere — LauncherApp relies on this
        // rather than checking menuRow_ a second time.
        const MenuLine none = menu_line(phone(apps, -1), m, fake_width);
        assert(menu_hit(none, 10.0f, 10.0f) == MenuHit::None);
    }

    std::printf("launcher_geometry_test: ok\n");
    return 0;
}
