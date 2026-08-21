// app_list, tested the way app_shell tests things: plain assert(), Debug only,
// no framework. The filter is the one piece of this launcher that is pure data
// in, data out — so it is the one piece that can be tested without a phone.
#include "../app/app_list.hh"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

static AppList sample() {
    AppList apps;
    apps.set({
        {"Calendar",  "com.android.calendar", "com.android.calendar.Main"},
        {"Camera",    "com.android.camera",   "com.android.camera.Main"},
        {"Firefox",   "org.mozilla.firefox",  "org.mozilla.firefox.App"},
        {"Messages",  "com.android.mms",      "com.android.mms.Main"},
        {"Play Store","com.android.vending",  "com.android.vending.Main"},
        {"Signal",    "org.thoughtcrime",     "org.thoughtcrime.Main"},
    });
    return apps;
}

static std::vector<std::string> labels(const AppList& apps) {
    std::vector<std::string> out;
    for (const AppEntry* e : apps.matches()) out.push_back(e->label);
    return out;
}

int main() {
    // An empty query shows everything, in the order the platform gave it.
    {
        AppList apps = sample();
        apps.setQuery("");
        assert(apps.matches().size() == 6);
        assert(labels(apps)[0] == "Calendar");
        assert(labels(apps)[5] == "Signal");
    }

    // Case-insensitive substring.
    {
        AppList apps = sample();
        apps.setQuery("FIRE");
        assert(labels(apps) == std::vector<std::string>{"Firefox"});
    }

    // A prefix of the label beats a match in the middle of one.
    {
        AppList apps = sample();
        apps.setQuery("ca");
        // "Calendar" and "Camera" start with it; "Play Store" does not match at
        // all; nothing else contains "ca".
        assert(labels(apps) == (std::vector<std::string>{"Calendar", "Camera"}));
    }

    // A word boundary inside the label beats a match mid-word.
    {
        AppList apps = sample();
        apps.setQuery("st");
        // "Play Store" matches at a word start; nothing else contains "st".
        assert(labels(apps) == std::vector<std::string>{"Play Store"});
    }

    {
        AppList apps = sample();
        apps.setQuery("s");
        // Word-start matches first ("Signal", "Play Store"), then mid-word
        // ("Messages"). Ties keep the platform's order.
        assert(labels(apps) == (std::vector<std::string>{"Signal", "Play Store", "Messages"}));
    }

    // No match is not an error, it is an empty list.
    {
        AppList apps = sample();
        apps.setQuery("zzz");
        assert(apps.matches().empty());
        assert(apps.first() == nullptr);
    }

    // first() is what Enter opens.
    {
        AppList apps = sample();
        apps.setQuery("sig");
        assert(apps.first() != nullptr);
        assert(apps.first()->package == "org.thoughtcrime");
    }

    // Re-setting the entries (a package was installed) re-applies the query.
    {
        AppList apps = sample();
        apps.setQuery("sig");
        apps.set({{"Signal", "org.thoughtcrime", "org.thoughtcrime.Main"},
                  {"Sigma",  "com.sigma",        "com.sigma.Main"}});
        assert(labels(apps) == (std::vector<std::string>{"Signal", "Sigma"}));
    }

    std::puts("app_list_test: ok");
    return 0;
}
