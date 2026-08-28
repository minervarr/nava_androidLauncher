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

    // The package, but only when another entry carries the SAME label — two
    // identical lines are the one case a text-only launcher cannot survive.
    // Filled in by AppList::set(); empty for the overwhelming majority.
    // Not part of operator== below: it is derived from the list, not queried
    // from the system, so comparing it would be comparing a conclusion.
    std::string disambiguator;

    // So a freshly queried list can be compared against the one on screen. A
    // refresh that found nothing new must not cost a redraw or a disk write,
    // and "nothing new" is exactly this.
    bool operator==(const AppEntry& o) const {
        return label == o.label && package == o.package && activity == o.activity;
    }
    bool operator!=(const AppEntry& o) const { return !(*this == o); }
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
    // `narrowing` says the new query extends the old one, so only the current
    // matches can still match and the entry list need not be walked at all.
    void reapply(bool narrowing);

    std::vector<AppEntry>        entries_;
    // entries_[i]'s label, lowercased. Parallel to entries_ and rebuilt only by
    // set(), so a keystroke never folds a label again.
    std::vector<std::string>     folded_;
    std::vector<const AppEntry*> matches_;
    std::vector<const AppEntry*> scratch_;   // narrowing's candidate list
    std::string                  query_;
    std::string                  queryFolded_;
};
