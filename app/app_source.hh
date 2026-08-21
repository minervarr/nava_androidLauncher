#pragma once
#include <string>
#include <vector>

#include "app_list.hh"

// ── Where the apps come from ─────────────────────────────────────────────────
//
// The seam that keeps launcher_app.cc portable. On a phone this is the
// PackageManager, reached through os/pm_bridge.cc and a page of JNI; on a
// desktop it is a hardcoded list, which is what makes the whole UI — the
// filtering, the layout, the key handling, the long-press menu — runnable and
// debuggable without an install cycle.
//
// Deliberately four methods and no notion of icons, badges, work profiles or
// shortcuts. A launcher that shows only text needs a label, a package and an
// activity, and every additional thing this interface admitted would be a thing
// the desktop implementation had to invent.
struct AppSource {
    virtual ~AppSource() = default;

    // Everything launchable, sorted by label. Called at startup and again
    // whenever packagesChanged() says the answer has moved.
    virtual std::vector<AppEntry> query() = 0;

    virtual void launch(const AppEntry& app) = 0;
    virtual void openInfo(const AppEntry& app) {}
    virtual void uninstall(const AppEntry& app) {}

    // True at most once per change. Polled from the frame loop rather than
    // pushed, because the platform's notification arrives on another thread and
    // re-querying mid-draw is the race this avoids.
    virtual bool packagesChanged() { return false; }
};
