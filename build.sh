#!/usr/bin/env bash
# ── One way to build this thing ───────────────────────────────────────────────
#
#   ./build.sh                      debug desktop
#   ./build.sh release android      the signed APK
#   ./build.sh debug all --install  both, then push the APK to the phone
#
# It exists because the two halves of this repo are built by different tools in
# a fixed ORDER: the desktop CMake configure generates ui_min_text_size.gen.h
# with a program that has to RUN on this machine, and the Android build only
# reads it (platform/android/CMakeLists.txt:40). Building the APK from a clean
# checkout therefore fails with a message about a missing header, which is a
# thing to be handled once, here, rather than remembered every time.
set -euo pipefail

readonly ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly ANDROID_DIR="$ROOT/platform/android"

# The SDK and NDK are root-owned installs on this machine and are not on PATH,
# so Gradle has to be told. Respects the environment when it already says.
export ANDROID_HOME="${ANDROID_HOME:-/opt/android-sdk}"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-/opt/android-ndk}"

BUILD_TYPE=""
TARGET=""
DO_INSTALL=0
DO_TEST=0
DO_CLEAN=0
GRADLE_FLAGS=()

usage() {
    cat <<'EOF'
usage: ./build.sh [debug|release] [desktop|android|all] [options]

  debug            unoptimised; also builds the tests and the headless
                   capture tool, which are Debug-only targets (default)
  release          optimised; the APK is signed from keystore.properties

  desktop          the Wayland build, ./build/nava_launcher (default)
  android          the APK, for arm64-v8a and x86_64
  all              both

options:
  --install        adb install -r the arm64 APK when it is built
  --test           run the unit tests after a desktop build
  --clean          delete the build trees first
  --offline        pass --offline to Gradle (no network)
  -h, --help       this

With no arguments at all and a terminal attached, it asks.
EOF
}

# ── Arguments ────────────────────────────────────────────────────────────────

for arg in "$@"; do
    case "$arg" in
        debug|Debug)     BUILD_TYPE=Debug   ;;
        release|Release) BUILD_TYPE=Release ;;
        desktop|android|all) TARGET="$arg"  ;;
        --install)  DO_INSTALL=1 ;;
        --test)     DO_TEST=1    ;;
        --clean)    DO_CLEAN=1   ;;
        --offline)  GRADLE_FLAGS+=(--offline) ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "build.sh: unknown argument '$arg'" >&2; usage >&2; exit 2 ;;
    esac
done

# Asking is for a person at a terminal. A script, a hook or CI gets the
# defaults instead of a hang on a read that no one will answer.
if [[ -z "$BUILD_TYPE" && -z "$TARGET" && -t 0 && -t 1 ]]; then
    read -rp "build type [debug]/release: " reply
    case "${reply,,}" in release|r) BUILD_TYPE=Release ;; *) BUILD_TYPE=Debug ;; esac
    read -rp "target [desktop]/android/all: " reply
    case "${reply,,}" in
        android|a) TARGET=android ;;
        all)       TARGET=all     ;;
        *)         TARGET=desktop ;;
    esac
fi

BUILD_TYPE="${BUILD_TYPE:-Debug}"
TARGET="${TARGET:-desktop}"

# One tree per build type. A Ninja tree carries CMAKE_BUILD_TYPE in its cache,
# so a shared directory would mean `release` silently throwing away the Debug
# tree the tests and the capture tool live in — and README.md points at ./build
# by name, so Debug is the one that keeps it.
if [[ "$BUILD_TYPE" == Debug ]]; then
    readonly BUILD_DIR="$ROOT/build"
else
    readonly BUILD_DIR="$ROOT/build-release"
fi

say() { printf '\n\033[1m── %s\033[0m\n' "$*"; }

# ── Clean ────────────────────────────────────────────────────────────────────

if (( DO_CLEAN )); then
    say "cleaning"
    rm -rf "$BUILD_DIR"
    rm -rf "$ANDROID_DIR/app/build" "$ANDROID_DIR/build"
fi

# ── Desktop ──────────────────────────────────────────────────────────────────

configure_desktop() {   # $1 = build dir, $2 = build type
    local existing=""
    if [[ -f "$1/CMakeCache.txt" ]]; then
        existing="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$1/CMakeCache.txt")"
    fi
    if [[ "$existing" != "$2" ]]; then
        say "configuring desktop ($2) in ${1#$ROOT/}"
        cmake -S "$ROOT" -B "$1" -DCMAKE_BUILD_TYPE="$2" -G Ninja
    fi
}

if [[ "$TARGET" == desktop || "$TARGET" == all ]]; then
    configure_desktop "$BUILD_DIR" "$BUILD_TYPE"
fi

if [[ "$TARGET" == desktop || "$TARGET" == all ]]; then
    say "building desktop ($BUILD_TYPE)"
    cmake --build "$BUILD_DIR"
    echo "  $BUILD_DIR/nava_launcher"
    if [[ "$BUILD_TYPE" == Debug ]]; then echo "  $BUILD_DIR/nava_capture"; fi
fi

if (( DO_TEST )); then
    if [[ "$BUILD_TYPE" != Debug ]]; then
        echo "build.sh: the tests are Debug-only targets; skipping --test" >&2
    else
        say "tests"
        cmake --build "$BUILD_DIR" --target app_list_test app_cache_test launcher_geometry_test
        "$BUILD_DIR/app_list_test"
        "$BUILD_DIR/app_cache_test"
        "$BUILD_DIR/launcher_geometry_test"
    fi
fi

# ── Android ──────────────────────────────────────────────────────────────────

if [[ "$TARGET" == android || "$TARGET" == all ]]; then
    # An unsigned release APK installs on no device, and Gradle produces one
    # without complaining. Saying so before the build beats finding out after.
    if [[ "$BUILD_TYPE" == Release && ! -f "$ANDROID_DIR/keystore.properties" ]]; then
        echo "build.sh: platform/android/keystore.properties is missing — a release" >&2
        echo "          APK built without it is unsigned and will not install." >&2
        exit 1
    fi

    # The generated header the APK build reads but cannot make — see the top of
    # this file. It does not depend on the build type, so a Debug configure of
    # ./build produces it for a release APK just as well, and that is the path
    # platform/android/CMakeLists.txt defaults to.
    if [[ ! -f "$ROOT/build/generated/ui_min_text_size.gen.h" ]]; then
        configure_desktop "$ROOT/build" Debug
    fi

    local_task="assemble${BUILD_TYPE}"
    say "building APK ($BUILD_TYPE)"
    ( cd "$ANDROID_DIR" && gradle "$local_task" "${GRADLE_FLAGS[@]}" )

    apk_dir="$ANDROID_DIR/app/build/outputs/apk/${BUILD_TYPE,,}"
    apk="$apk_dir/app-arm64-v8a-${BUILD_TYPE,,}.apk"
    ls -la "$apk_dir"/*.apk

    if (( DO_INSTALL )); then
        say "installing"
        if ! command -v adb >/dev/null; then
            echo "build.sh: adb is not on PATH" >&2; exit 1
        fi
        if [[ -z "$(adb devices | sed '1d;/^$/d')" ]]; then
            echo "build.sh: no device in 'adb devices' — plug the phone in and" >&2
            echo "          re-run, or install by hand:" >&2
            echo "          adb install -r $apk" >&2
            exit 1
        fi
        adb install -r "$apk"
        # Not automatic: making something the HOME screen is the user's
        # decision, and the command is one line when they want it.
        echo "  to make it the home screen:"
        echo "    adb shell cmd package set-home-activity io.nava.launcher/.LauncherActivity"
    fi
fi

say "done"
