#pragma once
#include <android_native_app_glue.h>

#include <string>
#include <vector>

#include "../app/app_list.hh"

// ── Talking to LauncherActivity ──────────────────────────────────────────────
//
// The one thing app_shell cannot do for a launcher: ask the PackageManager what
// is installed, and start one of the answers. Modelled on app_shell's
// os/activity_bridge.hh in every respect, including the asymmetry —
//
//   UP   (here -> Java)  blocking JNI calls, made from the app thread.
//   DOWN (Java -> here)  a package appeared or vanished; Java sets a flag under
//                        a lock and rings the waker, and the app collects it
//                        from inside pump(). Never a direct call.
//
// Why not JNI straight into android.content.pm from C++? Because the query is
// four chained calls, each returning a Java object that has to be released, and
// a labels-need-the-locale sort at the end. That is a page of fragile JNI to
// replace six readable lines of Java, and every one of the objects involved is
// one the Java side already has to hand.
namespace pm {

// Once, before any JNI entry point can fire — same contract as activity::.
void set_app(android_app* app);

// The doorbell. AndroidHost's waker wakes a pump() blocked in ALooper, which an
// arriving broadcast otherwise would not.
void set_waker(void (*wake)());

// ── Up-calls ────────────────────────────────────────────────────────────────

// Everything with an ACTION_MAIN/CATEGORY_LAUNCHER activity, already sorted by
// label on the Java side — it is the side that knows the user's locale.
// Empty when the activity does not answer, which is what a consumer whose
// Activity does not extend LauncherActivity gets.
std::vector<AppEntry> query_launcher_apps();

// Starts the app. `activity` is the launcher activity's fully-qualified name,
// so the component is explicit and no disambiguation dialog can appear.
bool launch_app(const std::string& package, const std::string& activity);

// Settings > Apps > this one.
void open_app_info(const std::string& package);

// Asks the SYSTEM to uninstall; the confirmation dialog is Android's, and a
// launcher must not pretend otherwise. Silently does nothing for a system app,
// which cannot be uninstalled at all.
void request_uninstall(const std::string& package);

// ── Down-calls, collected ───────────────────────────────────────────────────

// True once if a package was installed, removed or changed since the last call.
// A latch, not a queue: the answer to "what is installed now?" is a fresh
// query_launcher_apps(), so counting the events that led here would be work
// with nothing to spend it on.
bool take_packages_changed();

} // namespace pm
