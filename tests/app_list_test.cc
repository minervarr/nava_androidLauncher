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

    // ── Typing narrows through the previous result set ──────────────────────
    //
    // setQuery() takes a shortcut when the new query EXTENDS the old one: only
    // the current matches can still match, so the entry list is not walked. The
    // shortcut must be invisible — every query below has to give the same
    // answer whether it was reached one keystroke at a time or in one go.
    {
        AppList typed = sample(), direct = sample();
        for (const std::string& q : {"c", "ca", "cam", "came"}) {
            typed.setQuery(q);
            direct.set(sample().all());   // a fresh list: no previous matches
            direct.setQuery(q);
            assert(labels(typed) == labels(direct));
        }
        assert(labels(typed) == (std::vector<std::string>{"Camera"}));

        // …and backspacing, which is NOT a narrowing, must widen again.
        typed.setQuery("cam");
        assert(labels(typed) == (std::vector<std::string>{"Camera"}));
        typed.setQuery("ca");
        assert(labels(typed) == (std::vector<std::string>{"Calendar", "Camera"}));
        typed.setQuery("");
        assert(typed.matches().size() == 6);
    }

    // Narrowing preserves the tiers, rather than the order it inherited: "s"
    // ranks Signal (label prefix) above Messages (mid-word), and "si" keeps
    // that while dropping the rest.
    {
        AppList apps = sample();
        apps.setQuery("s");
        assert(labels(apps)[0] == "Signal");
        apps.setQuery("si");
        assert(labels(apps) == (std::vector<std::string>{"Signal"}));
    }

    // A pointer handed out by matches() points into the list's own entries —
    // the narrowing path recovers each entry's folded label by index, so a
    // wrong index here would rank one app by another's name.
    {
        AppList apps = sample();
        apps.setQuery("p");
        apps.setQuery("pl");
        assert(apps.matches().size() == 1);
        assert(apps.matches()[0]->package == "com.android.vending");
    }

    // ── Two apps with the same name ─────────────────────────────────────────
    //
    // A phone really has these: a system Camera and a vendor Camera, two
    // browsers both called "Browser", the same app in a work profile. Two
    // identical lines with no way to tell which is which is the one case where
    // a text-only launcher stops being usable, so the package breaks the tie.
    {
        AppList apps;
        apps.set({
            {"Camera", "com.android.camera", "com.android.camera.Main"},
            {"Camera", "com.vendor.camera",  "com.vendor.camera.Main"},
            {"Clock",  "com.android.clock",  "com.android.clock.Main"},
        });
        apps.setQuery("");
        const auto& m = apps.matches();
        assert(m.size() == 3);
        // Only the colliding ones carry it.
        assert(m[0]->disambiguator == "com.android.camera");
        assert(m[1]->disambiguator == "com.vendor.camera");
        assert(m[2]->disambiguator.empty());
    }

    // The comparison is on the label alone: a package that merely CONTAINS
    // another's name is not a collision.
    {
        AppList apps;
        apps.set({
            {"Camera",      "com.a", "com.a.Main"},
            {"Camera Demo", "com.b", "com.b.Main"},
        });
        assert(apps.all()[0].disambiguator.empty());
        assert(apps.all()[1].disambiguator.empty());
    }

    // Case-insensitively equal labels still collide — "Camera" and "camera"
    // are two identical lines as far as a reader is concerned.
    {
        AppList apps;
        apps.set({
            {"camera", "com.a", "com.a.Main"},
            {"Camera", "com.b", "com.b.Main"},
        });
        assert(!apps.all()[0].disambiguator.empty());
        assert(!apps.all()[1].disambiguator.empty());
    }

    std::puts("app_list_test: ok");
    return 0;
}
