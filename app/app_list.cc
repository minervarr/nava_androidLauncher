#include "app_list.hh"

#include <algorithm>
#include <cstddef>

namespace {

char lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// Byte offset of the first case-insensitive occurrence of `needle`, or npos.
//
// ASCII-only folding, on purpose. Real Unicode case folding needs a table this
// program has no other use for, and the failure it prevents — typing "É" and
// not finding "Éclair" — is one keystroke away from working, because the query
// is a substring match and "clair" finds it anyway.
size_t find_ci(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    if (needle.size() > hay.size()) return std::string::npos;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && lower(hay[i + j]) == lower(needle[j])) ++j;
        if (j == needle.size()) return i;
    }
    return std::string::npos;
}

// Lower is better. Three tiers, and the middle one is the reason this is not
// just a substring test: someone reaching for "Play Store" types "st", and a
// mid-word hit in another app's name must not come first.
enum Rank { kLabelPrefix = 0, kWordPrefix = 1, kMidWord = 2, kNoMatch = 3 };

int rank(const std::string& label, const std::string& query) {
    const size_t at = find_ci(label, query);
    if (at == std::string::npos) return kNoMatch;
    if (at == 0) return kLabelPrefix;
    const char prev = label[at - 1];
    if (prev == ' ' || prev == '-' || prev == '_' || prev == '.') return kWordPrefix;
    return kMidWord;
}

}  // namespace

void AppList::set(std::vector<AppEntry> entries) {
    entries_ = std::move(entries);
    reapply();
}

void AppList::setQuery(const std::string& query) {
    query_ = query;
    reapply();
}

void AppList::reapply() {
    matches_.clear();
    matches_.reserve(entries_.size());

    if (query_.empty()) {
        for (const AppEntry& e : entries_) matches_.push_back(&e);
        return;
    }

    // One pass per tier rather than a sort: it is stable by construction, so
    // the platform's ordering survives inside each tier without carrying a
    // secondary key around.
    for (int tier = kLabelPrefix; tier <= kMidWord; ++tier)
        for (const AppEntry& e : entries_)
            if (rank(e.label, query_) == tier) matches_.push_back(&e);
}
