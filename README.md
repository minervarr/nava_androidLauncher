# navaLauncher

An Android launcher that is a text field and a list of words. Black background,
white text, no icons, no clock, no favourites. Press Home, the keyboard is
already up; type, and the list narrows; press Enter, and the first line opens.
Long-press a line for `info` / `uninstall`.

Built on `framework/App_shell` (the Host/AppView seam and its Android host) and
`framework/Vk_Canvas_Lb_LAW` (the Vulkan canvas and MSDF text).

## Layout

```
app/     app_list.*      the filter — portable, tested, knows nothing about Android
         launcher_app.*  the AppView: state, layout, input
         app_source.hh   where apps come from (PackageManager, or a fake list)
         android_main.cc / desktop_main.cc
os/      pm_bridge.*     the JNI seam to the PackageManager
platform/android/        Gradle module, manifest, LauncherActivity.java
assets/fonts/ui.otf      the only asset that is not generated
```

## Building

```sh
./build.sh                       # debug desktop
./build.sh debug all --test      # …plus the APK and the unit tests
./build.sh release android       # the signed APK
./build.sh --help
```

Run with no arguments at a terminal and it asks for the type and the target.
Debug builds into `build/`, release into `build-release/`, so one never throws
away the other's tree. What the script does by hand, and why the order matters:

The desktop build must be configured FIRST, even if you only want the APK: it
generates `ui_min_text_size.gen.h` with a tool that has to run on the build
machine, and the Android build copies the result.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -G Ninja
cmake --build build
./build/app_list_test          # the filter's unit tests
./build/launcher_geometry_test # rows, scrolling, the long-press menu's targets
./build/nava_launcher          # the real UI, over a fake app list
./build/nava_launcher ca       # …starting with the field pre-filled
```

```sh
cd platform/android && gradle assembleDebug
adb install -r app/build/outputs/apk/debug/app-arm64-v8a-debug.apk
adb shell cmd package set-home-activity io.nava.launcher/.LauncherActivity
```

Shaders are compiled from `.slang` by `slangc`, found via `/opt/shader-slang/bin`
or `$VULKAN_SDK/bin`. Do not substitute the `.spv` files committed in the
submodules — see `cmake/NavaAssets.cmake`.
