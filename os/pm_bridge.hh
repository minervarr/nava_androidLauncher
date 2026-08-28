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
// The QUERY is the exception to that shape, and it lives entirely in C++ (see
// pm_bridge.cc). It used to be six readable lines of Java — whose sort called
// loadLabel() on both sides of every comparison, so ~180 apps cost thousands of
// resource-table lookups instead of 180. Doing it here loads each label exactly
// once and sorts the strings we already hold, which is the difference between a
// query that can run at startup and one that cannot.
namespace pm {

// Once, before any JNI entry point can fire — same contract as activity::.
void set_app(android_app* app);

// The doorbell. AndroidHost's waker wakes a pump() blocked in ALooper, which an
// arriving broadcast otherwise would not.
void set_waker(void (*wake)());

// ── Up-calls ────────────────────────────────────────────────────────────────

// Everything with an ACTION_MAIN/CATEGORY_LAUNCHER activity, sorted by label.
// Empty when the PackageManager cannot be reached at all.
//
// Safe to call from a worker thread: it attaches that thread to the VM on its
// first call. The thread MUST call detach_thread() before it exits.
std::vector<AppEntry> query_launcher_apps();

// Detaches the calling thread from the Java VM. Only for a thread that will not
// make another JNI call — dying while still attached is a process abort, and
// the attach is implicit inside query_launcher_apps().
void detach_thread();

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
