// The Android entry point, and the only file in the app that includes JNI.
//
// app_shell's CLAUDE.md promises this file is six lines; it is longer only
// because the PackageManager seam has to be wired to the host's waker, and a
// waker is a function pointer that needs somewhere to point.
#include <android_native_app_glue.h>

#include <memory>

#include "../os/pm_bridge.hh"
#include "app_source.hh"
#include "launcher_app.hh"
#include "os/android_host.hh"   // app_shell

namespace {

// AppSource over the real PackageManager. Thin by design: every hard part is
// on the other side of pm_bridge, in Java.
class PmSource : public AppSource {
public:
    std::vector<AppEntry> query() override        { return pm::query_launcher_apps(); }
    void launch(const AppEntry& e) override       { pm::launch_app(e.package, e.activity); }
    void openInfo(const AppEntry& e) override     { pm::open_app_info(e.package); }
    void uninstall(const AppEntry& e) override    { pm::request_uninstall(e.package); }
    bool packagesChanged() override               { return pm::take_packages_changed(); }

    // query() runs on LauncherApp's refresh thread, which JNI attached on its
    // first call. A thread that dies attached takes the process with it.
    void onWorkerExit() override                  { pm::detach_thread(); }
};

AndroidHost* g_host = nullptr;

// Rings the host's doorbell from the UI thread. A pump() asleep in
// ALooper_pollOnce does not wake because a broadcast receiver set a flag; see
// activity_bridge.hh, which learned this the same way.
void wake() { if (g_host) g_host->invalidate(); }

} // namespace

extern "C" void android_main(android_app* state) {
    pm::set_app(state);

    auto host = std::make_unique<AndroidHost>(state,
                                              /*launchExtraKey=*/nullptr,
                                              /*fallback=*/nullptr,
                                              /*requestAllFilesAccess=*/false);
    g_host = host.get();
    pm::set_waker(&wake);

    LauncherApp app(std::make_unique<PmSource>());
    if (app.create(std::move(host))) app.run();

    g_host = nullptr;
    pm::set_waker(nullptr);
}
