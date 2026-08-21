// The desktop build. Not a port and not a demo: it is how the launcher's UI is
// developed at all — the filtering, the layout, the key handling and the
// long-press menu are the same objects here as on the phone, and an install
// cycle is a minute long while this is a second.
//
// What it cannot do is launch anything, because there is nothing to launch.
#include <cstdio>
#include <memory>

#include "app_main.hh"   // app_shell: app_shell_main()
#include "app_source.hh"
#include "host.hh"
#include "launcher_app.hh"

namespace {

class FakeSource : public AppSource {
public:
    std::vector<AppEntry> query() override {
        return {
            {"Calculator", "com.fake.calc",     "com.fake.calc.Main"},
            {"Calendar",   "com.fake.cal",      "com.fake.cal.Main"},
            {"Camera",     "com.fake.camera",   "com.fake.camera.Main"},
            {"Clock",      "com.fake.clock",    "com.fake.clock.Main"},
            {"Files",      "com.fake.files",    "com.fake.files.Main"},
            {"Firefox",    "org.mozilla.fake",  "org.mozilla.fake.App"},
            {"Maps",       "com.fake.maps",     "com.fake.maps.Main"},
            {"Messages",   "com.fake.mms",      "com.fake.mms.Main"},
            {"Play Store", "com.fake.vending",  "com.fake.vending.Main"},
            {"Settings",   "com.fake.settings", "com.fake.settings.Main"},
            {"Signal",     "org.fake.signal",   "org.fake.signal.Main"},
            {"Telephone",  "com.fake.dialer",   "com.fake.dialer.Main"},
        };
    }
    void launch(const AppEntry& e) override    { std::printf("launch    %s\n", e.package.c_str()); }
    void openInfo(const AppEntry& e) override  { std::printf("app info  %s\n", e.package.c_str()); }
    void uninstall(const AppEntry& e) override { std::printf("uninstall %s\n", e.package.c_str()); }
};

} // namespace

int app_shell_main(int argc, char** argv) {
    LauncherApp app(std::make_unique<FakeSource>());
    if (!app.create(make_host())) return 1;
    if (argc > 1) app.setInitialQuery(argv[1]);   // see LauncherApp::setInitialQuery
    app.run();
    return 0;
}
