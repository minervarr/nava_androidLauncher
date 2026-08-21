#include "pm_bridge.hh"

#include <jni.h>
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

} // namespace

namespace pm {

void set_app(android_app* app)   { g_app = app; }
void set_waker(void (*wake)())   { g_wake = wake; }

std::vector<AppEntry> query_launcher_apps() {
    std::vector<AppEntry> out;
    if (!g_app) return out;
    JNIEnv* env = env_for(g_app);
    if (!env) return out;

    jobject act = g_app->activity->clazz;
    jclass  cls = env->GetObjectClass(act);
    jmethodID m = env->GetMethodID(cls, "queryLauncherApps", "()[Ljava/lang/String;");
    if (!m) {
        check_exc(env, "queryLauncherApps");
        env->DeleteLocalRef(cls);
        return out;
    }

    // A flat String[] of label/package/activity triples rather than an array of
    // some Parcelable: three GetObjectArrayElement calls per app against one
    // findClass + three GetFieldID for a struct, and nothing here has to agree
    // with a Java type beyond "the length is a multiple of three".
    jobjectArray arr = (jobjectArray)env->CallObjectMethod(act, m);
    if (check_exc(env, "queryLauncherApps") || !arr) {
        env->DeleteLocalRef(cls);
        return out;
    }

    const jsize n = env->GetArrayLength(arr);
    out.reserve((size_t)n / 3);
    for (jsize i = 0; i + 2 < n; i += 3) {
        jstring l = (jstring)env->GetObjectArrayElement(arr, i);
        jstring p = (jstring)env->GetObjectArrayElement(arr, i + 1);
        jstring a = (jstring)env->GetObjectArrayElement(arr, i + 2);
        out.push_back({from_jstring(env, l), from_jstring(env, p), from_jstring(env, a)});
        env->DeleteLocalRef(l);
        env->DeleteLocalRef(p);
        env->DeleteLocalRef(a);
    }

    env->DeleteLocalRef(arr);
    env->DeleteLocalRef(cls);
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

void open_app_info(const std::string& package)      { call_with_string("openAppInfo", package); }
void request_uninstall(const std::string& package)  { call_with_string("requestUninstall", package); }

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
