#pragma once
#include <string>
#include <vector>

// ── The launcher's only model ────────────────────────────────────────────────
//
// Every app the system will let us start, and the subset of them the current
// query matches. Deliberately knows nothing about Android: it is handed labels
// and package names as strings, which is exactly what makes it the one part of
// this program that can be built and tested on a desktop.
//
// The ORDER of the entries is the platform's, not ours — Java sorts by label
// before it hands them over, because it is the side that knows the user's
// locale and collation rules. Ties in the ranking below therefore preserve it.
struct AppEntry {
    std::string label;     // what the user sees, and the only thing searched
    std::string package;   // com.example.app
    std::string activity;  // the launcher activity's fully-qualified name
};

class AppList {
public:
    // Replaces every entry — what a fresh PackageManager query produces, and
    // what a package being installed or removed triggers. Re-applies the
    // current query, so the visible list never goes stale behind the text.
    void set(std::vector<AppEntry> entries);

    // Case-insensitive; an empty query matches everything.
    void setQuery(const std::string& query);
    const std::string& query() const { return query_; }

    // Ranked best-first. Pointers into this object's own storage: valid until
    // the next set(), which is the only call that can move them.
    const std::vector<const AppEntry*>& matches() const { return matches_; }

    // What Enter opens. Null when nothing matched.
    const AppEntry* first() const { return matches_.empty() ? nullptr : matches_.front(); }

    const std::vector<AppEntry>& all() const { return entries_; }

private:
    void reapply();

    std::vector<AppEntry>        entries_;
    std::vector<const AppEntry*> matches_;
    std::string                  query_;
};
