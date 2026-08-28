#include "app_list.hh"

#include <algorithm>
#include <cstddef>

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

}  // namespace

// Same folding rule the filter uses — see find_ci below on why ASCII-only.
static bool same_label(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

namespace {

// Byte offset of the first occurrence of `needle` in an ALREADY-FOLDED
// haystack, or npos. The needle is folded by the caller, once per keystroke,
// and the haystack once per app list — which is the whole point: the old
// version folded both, byte by byte, on every comparison of every keystroke.
//
// ASCII-only folding, on purpose. Real Unicode case folding needs a table this
// program has no other use for, and the failure it prevents — typing "É" and
// not finding "Éclair" — is one keystroke away from working, because the query
// is a substring match and "clair" finds it anyway.
size_t find_folded(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    if (needle.size() > hay.size()) return std::string::npos;
    const size_t last = hay.size() - needle.size();
    for (size_t i = 0; i <= last; ++i) {
        if (hay[i] != needle[0]) continue;
        if (std::equal(needle.begin(), needle.end(), hay.begin() + (long)i)) return i;
    }
    return std::string::npos;
}

// Lower is better. Three tiers, and the middle one is the reason this is not
// just a substring test: someone reaching for "Play Store" types "st", and a
// mid-word hit in another app's name must not come first.
enum Rank { kLabelPrefix = 0, kWordPrefix = 1, kMidWord = 2, kNoMatch = 3 };

int rank(const std::string& folded, const std::string& query) {
    const size_t at = find_folded(folded, query);
    if (at == std::string::npos) return kNoMatch;
    if (at == 0) return kLabelPrefix;
    const char prev = folded[at - 1];
    if (prev == ' ' || prev == '-' || prev == '_' || prev == '.') return kWordPrefix;
    return kMidWord;
}

std::string fold(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(lower(c));
    return out;
}

}  // namespace

// Gives every entry that shares its label with another one its package, so the
// two lines can be told apart. Case-insensitive, because two lines a reader
// cannot distinguish are a collision whatever their capitalisation.
//
// O(n^2) over the labels, deliberately: n is the number of installed apps (18
// on the device this was written against, a few hundred at the extreme), it
// runs once per query of the PackageManager, and a hash map here would cost
// more to read than the loop it replaced.
static void mark_duplicate_labels(std::vector<AppEntry>& entries) {
    for (AppEntry& e : entries) e.disambiguator.clear();

    for (size_t i = 0; i < entries.size(); ++i) {
        for (size_t j = i + 1; j < entries.size(); ++j) {
            if (!same_label(entries[i].label, entries[j].label)) continue;
            entries[i].disambiguator = entries[i].package;
            entries[j].disambiguator = entries[j].package;
        }
    }
}

void AppList::set(std::vector<AppEntry> entries) {
    entries_ = std::move(entries);
    mark_duplicate_labels(entries_);
    // Folded once here, read on every keystroke afterwards. An app list changes
    // when something is installed; a query changes several times a second.
    folded_.clear();
    folded_.reserve(entries_.size());
    for (const AppEntry& e : entries_) folded_.push_back(fold(e.label));
    reapply(/*narrowing=*/false);
}

void AppList::setQuery(const std::string& query) {
    // Typing NARROWS: "cam" can only match apps that already matched "ca", so
    // the previous result set is a complete and much smaller candidate list.
    // Backspacing, or any other edit, has to start over.
    const bool narrowing = !query_.empty() && query.size() > query_.size() &&
                           query.compare(0, query_.size(), query_) == 0;
    query_       = query;
    queryFolded_ = fold(query_);
    reapply(narrowing);
}

void AppList::reapply(bool narrowing) {
    if (query_.empty()) {
        matches_.clear();
        matches_.reserve(entries_.size());
        for (const AppEntry& e : entries_) matches_.push_back(&e);
        return;
    }

    // One pass per tier rather than a sort: it is stable by construction, so
    // the platform's ordering survives inside each tier without carrying a
    // secondary key around.
    if (narrowing) {
        // Candidates are the current matches, which the loop below is about to
        // overwrite — so they are taken out of the way first. scratch_ is a
        // member purely so this costs no allocation per keystroke.
        scratch_.swap(matches_);
        matches_.clear();
        for (int tier = kLabelPrefix; tier <= kMidWord; ++tier)
            for (const AppEntry* e : scratch_)
                if (rank(folded_[(size_t)(e - entries_.data())], queryFolded_) == tier)
                    matches_.push_back(e);
        return;
    }

    matches_.clear();
    matches_.reserve(entries_.size());
    for (int tier = kLabelPrefix; tier <= kMidWord; ++tier)
        for (size_t i = 0; i < entries_.size(); ++i)
            if (rank(folded_[i], queryFolded_) == tier) matches_.push_back(&entries_[i]);
}
