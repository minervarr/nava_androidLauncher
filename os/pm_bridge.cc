#include "pm_bridge.hh"

#include <android/log.h>
#include <jni.h>

#include <algorithm>
#include <mutex>
#include <vector>

#include "jni_util.hh"   // framework/Vk_Canvas_Lb_LAW/platform/android
#include "utf16_utf8.hh" // framework/App_shell

using vce::platform::jni::env_for;
using vce::platform::jni::check_exc;

namespace {

android_app* g_app  = nullptr;
void (*g_wake)()    = nullptr;

std::mutex g_mu;
bool       g_packagesChanged = false;

void ring() { if (g_wake) g_wake(); }   // outside the lock, as in activity_bridge

// GetObjectClass gives the RUNTIME class, so a method declared on
// LauncherActivity resolves without this file naming any subclass.
bool call_void(const char* name, const char* sig, jvalue* args) {
    if (!g_app) return false;
    JNIEnv* env = env_for(g_app);
    if (!env) return false;
    jobject act = g_app->activity->clazz;
    jclass  cls = env->GetObjectClass(act);
    jmethodID m = env->GetMethodID(cls, name, sig);
    if (!m) {
        check_exc(env, name);
        env->DeleteLocalRef(cls);
        return false;
    }
    env->CallVoidMethodA(act, m, args);
    const bool bad = check_exc(env, name);
    env->DeleteLocalRef(cls);
    return !bad;
}

jstring to_jstring(JNIEnv* env, const std::string& utf8) {
    // NOT NewStringUTF — modified UTF-8 mangles anything outside the BMP, and
    // an emoji in an app's name is not exotic.
    std::vector<uint16_t> u16 = utf16::to_utf16(utf8);
    return env->NewString(reinterpret_cast<const jchar*>(u16.data()), (jsize)u16.size());
}

std::string from_jstring(JNIEnv* env, jstring s) {
    if (!s) return {};
    const jchar* u = env->GetStringChars(s, nullptr);
    const jsize len = env->GetStringLength(s);
    std::string out = utf16::to_utf8(reinterpret_cast<const uint16_t*>(u), (size_t)len);
    env->ReleaseStringChars(s, u);
    return out;
}

bool call_with_string(const char* name, const std::string& arg) {
    if (!g_app) return false;
    JNIEnv* env = env_for(g_app);
    if (!env) return false;
    jstring js = to_jstring(env, arg);
    jvalue v;
    v.l = js;
    const bool ok = call_void(name, "(Ljava/lang/String;)V", &v);
    env->DeleteLocalRef(js);
    return ok;
}

// The launcher's own package name, so it can filter itself out of its own
// list. Resolved once and kept: it cannot change while the process lives.
const std::string& own_package(JNIEnv* env, jobject activity) {
    static std::string cached;
    static bool asked = false;
    if (!asked) {
        asked = true;
        jclass cls = env->GetObjectClass(activity);
        jmethodID m = env->GetMethodID(cls, "getPackageName", "()Ljava/lang/String;");
        if (m) {
            jstring js = (jstring)env->CallObjectMethod(activity, m);
            if (!check_exc(env, "getPackageName") && js) {
                cached = from_jstring(env, js);
                env->DeleteLocalRef(js);
            }
        } else {
            check_exc(env, "getPackageName");
        }
        env->DeleteLocalRef(cls);
    }
    return cached;
}

// ── The query ───────────────────────────────────────────────────────────────
//
// android.content.pm, reached directly. The shape is:
//
//   PackageManager pm = activity.getPackageManager();
//   Intent i = new Intent(ACTION_MAIN); i.addCategory(CATEGORY_LAUNCHER);
//   for (ResolveInfo ri : pm.queryIntentActivities(i, 0))
//       label = ri.loadLabel(pm), pkg = ri.activityInfo.packageName, …
//
// loadLabel() is the expensive call — it opens another app's resource table —
// so it is made exactly ONCE per app and the sort runs over the results. The
// Java version sorted with a Collator whose comparator called loadLabel() on
// both operands, which is the same work multiplied by 2·log₂N.
//
// Everything looked up by name is cached in the block below: a re-query after a
// package change costs JNI calls only, no reflection.
struct PmIds {
    jclass    activityCls   = nullptr;   // global refs, all of them
    jclass    intentCls     = nullptr;
    jclass    resolveInfoCls= nullptr;
    jclass    activityInfoCls = nullptr;
    jclass    listCls       = nullptr;
    jclass    charSeqCls    = nullptr;
    jmethodID getPackageManager = nullptr;
    jmethodID intentCtor        = nullptr;
    jmethodID addCategory       = nullptr;
    jmethodID queryIntentActivities = nullptr;
    jmethodID loadLabel     = nullptr;
    jmethodID toString      = nullptr;
    jmethodID listSize      = nullptr;
    jmethodID listGet       = nullptr;
    jfieldID  activityInfo  = nullptr;
    jfieldID  packageName   = nullptr;
    jfieldID  activityName  = nullptr;
    bool      ready         = false;
};

PmIds g_ids;
std::once_flag g_idsOnce;

jclass global_class(JNIEnv* env, const char* name) {
    jclass local = env->FindClass(name);
    if (!local) { check_exc(env, name); return nullptr; }
    jclass global = (jclass)env->NewGlobalRef(local);
    env->DeleteLocalRef(local);
    return global;
}

void resolve_ids(JNIEnv* env, jobject activity) {
    PmIds& d = g_ids;
    jclass actCls = env->GetObjectClass(activity);
    d.activityCls = (jclass)env->NewGlobalRef(actCls);
    env->DeleteLocalRef(actCls);

    d.intentCls       = global_class(env, "android/content/Intent");
    d.resolveInfoCls  = global_class(env, "android/content/pm/ResolveInfo");
    d.activityInfoCls = global_class(env, "android/content/pm/ActivityInfo");
    d.listCls         = global_class(env, "java/util/List");
    d.charSeqCls      = global_class(env, "java/lang/CharSequence");
    if (!d.intentCls || !d.resolveInfoCls || !d.activityInfoCls || !d.listCls ||
        !d.charSeqCls)
        return;

    d.getPackageManager = env->GetMethodID(d.activityCls, "getPackageManager",
                                           "()Landroid/content/pm/PackageManager;");
    d.intentCtor  = env->GetMethodID(d.intentCls, "<init>", "(Ljava/lang/String;)V");
    d.addCategory = env->GetMethodID(d.intentCls, "addCategory",
                                     "(Ljava/lang/String;)Landroid/content/Intent;");
    d.loadLabel   = env->GetMethodID(d.resolveInfoCls, "loadLabel",
                                     "(Landroid/content/pm/PackageManager;)Ljava/lang/CharSequence;");
    d.toString    = env->GetMethodID(d.charSeqCls, "toString", "()Ljava/lang/String;");
    d.listSize    = env->GetMethodID(d.listCls, "size", "()I");
    d.listGet     = env->GetMethodID(d.listCls, "get", "(I)Ljava/lang/Object;");
    d.activityInfo = env->GetFieldID(d.resolveInfoCls, "activityInfo",
                                     "Landroid/content/pm/ActivityInfo;");
    d.packageName  = env->GetFieldID(d.activityInfoCls, "packageName", "Ljava/lang/String;");
    // ActivityInfo.name is inherited from PackageItemInfo; GetFieldID walks the
    // superclass chain, so asking ActivityInfo for it is correct.
    d.activityName = env->GetFieldID(d.activityInfoCls, "name", "Ljava/lang/String;");

    // queryIntentActivities is declared on PackageManager, whose class we reach
    // from the instance rather than by name — FindClass would work too, but the
    // instance is already here and cannot be the wrong class.
    if (!d.getPackageManager) { check_exc(env, "getPackageManager"); return; }
    jobject pmObj = env->CallObjectMethod(activity, d.getPackageManager);
    if (check_exc(env, "getPackageManager") || !pmObj) return;
    jclass pmCls = env->GetObjectClass(pmObj);
    d.queryIntentActivities = env->GetMethodID(
        pmCls, "queryIntentActivities",
        "(Landroid/content/Intent;I)Ljava/util/List;");
    env->DeleteLocalRef(pmCls);
    env->DeleteLocalRef(pmObj);

    d.ready = d.getPackageManager && d.intentCtor && d.addCategory &&
              d.queryIntentActivities && d.loadLabel && d.toString &&
              d.listSize && d.listGet && d.activityInfo && d.packageName &&
              d.activityName;
    if (!d.ready) check_exc(env, "resolve_ids");
}

// A sort key: lowercase, and with the Latin-1 accented letters folded onto the
// letter they are drawn from, so "café" still sorts among the C's. This is not
// ICU collation — the Java version used a Collator, and giving that up is the
// price of not calling into Java per comparison. Everything outside Latin-1
// falls back to comparing UTF-8 bytes, which orders each script sensibly within
// itself even though it does not order the scripts against each other the way a
// locale would.
std::string sort_key(const std::string& label) {
    std::string k;
    k.reserve(label.size());
    for (size_t i = 0; i < label.size(); ++i) {
        const unsigned char c = (unsigned char)label[i];
        if (c < 0x80) {
            k.push_back((char)((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c));
            continue;
        }
        // Two-byte UTF-8 in the Latin-1 Supplement: 0xC3 0x80..0xBF is U+00C0..U+00FF.
        if (c == 0xC3 && i + 1 < label.size()) {
            const unsigned cp = 0xC0 + ((unsigned char)label[i + 1] - 0x80);
            static const char* kFold =
                //  À-Ï             Ð-ß              à-ï             ð-ÿ
                //  ß folds to s, and × and ÷ are not letters and stay put.
                "aaaaaaaceeeeiiii" "dnooooo\xD7ouuuuyps"
                "aaaaaaaceeeeiiii" "dnooooo\xF7ouuuuypy";
            if (cp >= 0xC0 && cp <= 0xFF) {
                k.push_back(kFold[cp - 0xC0]);
                ++i;
                continue;
            }
        }
        k.push_back((char)c);
    }
    return k;
}

} // namespace

namespace pm {

void set_app(android_app* app)   { g_app = app; }
void set_waker(void (*wake)())   { g_wake = wake; }

void detach_thread() {
    if (!g_app) return;
    JavaVM* vm = g_app->activity->vm;
    JNIEnv* env = nullptr;
    // Only detach a thread that is actually attached; detaching the glue thread
    // (which env_for() attached for the lifetime of the process) would break
    // every later call.
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) == JNI_OK)
        vm->DetachCurrentThread();
}

std::vector<AppEntry> query_launcher_apps() {
    std::vector<AppEntry> out;
    if (!g_app) return out;
    JNIEnv* env = env_for(g_app);   // attaches this thread if it is the worker's
    if (!env) return out;

    jobject act = g_app->activity->clazz;
    std::call_once(g_idsOnce, [&] { resolve_ids(env, act); });
    if (!g_ids.ready) return out;
    const PmIds& d = g_ids;

    jobject pmObj = env->CallObjectMethod(act, d.getPackageManager);
    if (check_exc(env, "getPackageManager") || !pmObj) return out;

    jstring actionMain = to_jstring(env, "android.intent.action.MAIN");
    jstring catLauncher = to_jstring(env, "android.intent.category.LAUNCHER");
    jobject intent = env->NewObject(d.intentCls, d.intentCtor, actionMain);
    if (check_exc(env, "new Intent") || !intent) {
        env->DeleteLocalRef(actionMain);
        env->DeleteLocalRef(catLauncher);
        env->DeleteLocalRef(pmObj);
        return out;
    }
    env->DeleteLocalRef(env->CallObjectMethod(intent, d.addCategory, catLauncher));
    check_exc(env, "addCategory");

    jobject list = env->CallObjectMethod(pmObj, d.queryIntentActivities, intent, 0);
    if (check_exc(env, "queryIntentActivities") || !list) {
        env->DeleteLocalRef(intent);
        env->DeleteLocalRef(actionMain);
        env->DeleteLocalRef(catLauncher);
        env->DeleteLocalRef(pmObj);
        return out;
    }

    const jint n = env->CallIntMethod(list, d.listSize);
    check_exc(env, "List.size");

    // The launcher is not something to launch from the launcher.
    const std::string selfPkg = own_package(env, act);

    std::vector<std::pair<std::string, AppEntry>> keyed;   // sort key + entry
    keyed.reserve((size_t)n);

    for (jint i = 0; i < n; ++i) {
        jobject ri = env->CallObjectMethod(list, d.listGet, i);
        if (check_exc(env, "List.get") || !ri) continue;

        jobject ai = env->GetObjectField(ri, d.activityInfo);
        if (!ai) { env->DeleteLocalRef(ri); continue; }

        jstring jpkg = (jstring)env->GetObjectField(ai, d.packageName);
        std::string pkg = from_jstring(env, jpkg);
        if (pkg == selfPkg) {
            if (jpkg) env->DeleteLocalRef(jpkg);
            env->DeleteLocalRef(ai);
            env->DeleteLocalRef(ri);
            continue;
        }

        jstring jact = (jstring)env->GetObjectField(ai, d.activityName);

        // The one expensive call, made once per app and never inside a compare.
        jobject label = env->CallObjectMethod(ri, d.loadLabel, pmObj);
        check_exc(env, "loadLabel");
        std::string labelStr;
        if (label) {
            jstring js = (jstring)env->CallObjectMethod(label, d.toString);
            if (!check_exc(env, "CharSequence.toString") && js) {
                labelStr = from_jstring(env, js);
                env->DeleteLocalRef(js);
            }
            env->DeleteLocalRef(label);
        }
        if (labelStr.empty()) labelStr = pkg;   // nameless is still launchable

        AppEntry e;
        e.label    = std::move(labelStr);
        e.package  = std::move(pkg);
        e.activity = from_jstring(env, jact);
        keyed.emplace_back(sort_key(e.label), std::move(e));

        if (jpkg) env->DeleteLocalRef(jpkg);
        if (jact) env->DeleteLocalRef(jact);
        env->DeleteLocalRef(ai);
        env->DeleteLocalRef(ri);
    }

    env->DeleteLocalRef(list);
    env->DeleteLocalRef(intent);
    env->DeleteLocalRef(actionMain);
    env->DeleteLocalRef(catLauncher);
    env->DeleteLocalRef(pmObj);

    // Stable, so two apps sharing a folded label keep the platform's order
    // between them rather than an arbitrary one.
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    out.reserve(keyed.size());
    for (auto& kv : keyed) out.push_back(std::move(kv.second));
    __android_log_print(ANDROID_LOG_INFO, "navaLauncher",
                        "query: %zu apps (resolver returned %d)", out.size(), (int)n);
    return out;
}

bool launch_app(const std::string& package, const std::string& activity) {
    if (!g_app) return false;
    JNIEnv* env = env_for(g_app);
    if (!env) return false;
    jstring jp = to_jstring(env, package);
    jstring ja = to_jstring(env, activity);
    jvalue args[2];
    args[0].l = jp;
    args[1].l = ja;
    const bool ok = call_void("launchApp", "(Ljava/lang/String;Ljava/lang/String;)V", args);
    env->DeleteLocalRef(jp);
    env->DeleteLocalRef(ja);
    return ok;
}

// Logged because these two are the only user actions in the program whose
// effect is another app's window: when nothing appears there is no way, from
// the launcher's own screen, to tell a missed tap from a refused Intent.
void open_app_info(const std::string& package) {
    const bool ok = call_with_string("openAppInfo", package);
    __android_log_print(ok ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "navaLauncher",
                        "openAppInfo(%s) %s", package.c_str(), ok ? "sent" : "FAILED");
}

void request_uninstall(const std::string& package) {
    const bool ok = call_with_string("requestUninstall", package);
    __android_log_print(ok ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "navaLauncher",
                        "requestUninstall(%s) %s", package.c_str(), ok ? "sent" : "FAILED");
}

bool take_packages_changed() {
    std::lock_guard<std::mutex> lock(g_mu);
    const bool changed = g_packagesChanged;
    g_packagesChanged = false;
    return changed;
}

} // namespace pm

// ── JNI entry points ────────────────────────────────────────────────────────
//
// Runs on the Android UI thread, from LauncherActivity's package broadcast
// receiver. Touches the locked flag and nothing else.
extern "C" {

JNIEXPORT void JNICALL
Java_io_nava_launcher_LauncherActivity_nativeOnPackagesChanged(JNIEnv*, jclass) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_packagesChanged = true;
    }
    ring();
}

} // extern "C"
