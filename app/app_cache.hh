#pragma once
#include <string>
#include <vector>

#include "app_list.hh"

// ── What the launcher knew last time ─────────────────────────────────────────
//
// The list of installed apps, on disk, so the first frame after a cold start
// can be drawn before the system has been asked anything at all. Asking is not
// free — a PackageManager query loads a label out of every installed app's
// resource table — and it is the one piece of startup work whose answer is
// almost always identical to yesterday's.
//
// The cache is therefore a PREDICTION, never the truth: LauncherApp draws it
// immediately and a background thread re-queries regardless, replacing the list
// if the two disagree. That is why nothing here validates freshness, and why a
// corrupt or truncated file is simply a miss — the real answer is already on
// its way, and refusing to start over a bad cache would be strictly worse than
// starting empty.
//
// Deliberately free of Android: it is std::string in, std::string out, which is
// what lets tests/app_cache_test.cc round-trip it on a desktop.

// Reads `path` into `out` (cleared first). False on a missing file, a foreign
// or future format, or a tail that does not parse — `out` is left empty.
bool load_app_cache(const std::string& path, std::vector<AppEntry>& out);

// Writes `in` to `path`, via a temporary file and a rename, so a process death
// mid-write leaves the previous cache intact rather than half of a new one.
bool save_app_cache(const std::string& path, const std::vector<AppEntry>& in);
