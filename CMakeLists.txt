# ── navaLauncher (desktop build) ─────────────────────────────────────────────
#
# The phone build is platform/android/CMakeLists.txt, driven by Gradle. THIS
# one exists so the UI can be developed without an install cycle: the same
# LauncherApp, the same filtering and layout, over a Wayland window and a
# hardcoded app list (app/desktop_main.cc).
cmake_minimum_required(VERSION 3.22)
project(nava_launcher CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(NAVA_ROOT     ${CMAKE_CURRENT_SOURCE_DIR})
set(FRAMEWORK_DIR ${NAVA_ROOT}/framework)

# app_shell defaults VK_CANVAS_DIR to ../vk_canvas; ours is checked out under
# its upstream name, so it has to be told. Set BEFORE add_subdirectory.
set(VK_CANVAS_DIR ${FRAMEWORK_DIR}/Vk_Canvas_Lb_LAW CACHE PATH "" FORCE)

set(APP_SHELL_APP_NAME    "navalauncher" CACHE STRING "" FORCE)
set(APP_SHELL_WINDOW_TITLE "navaLauncher" CACHE STRING "" FORCE)
set(APP_SHELL_APP_ID      "io.nava.launcher" CACHE STRING "" FORCE)

add_subdirectory(${VK_CANVAS_DIR}/core           ${CMAKE_BINARY_DIR}/vk_canvas_core)
add_subdirectory(${VK_CANVAS_DIR}/platform/linux ${CMAKE_BINARY_DIR}/vk_canvas_wayland)
add_subdirectory(${FRAMEWORK_DIR}/App_shell      ${CMAKE_BINARY_DIR}/app_shell)

# The text-size floor is derived from the faces the APP ships — list exactly
# the one it loads, or it can render below its own legibility floor.
include(AppShellMinTextSize)
app_shell_generate_min_text_size(${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/ui.otf)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(NavaAssets)

# Slang is not on PATH here, and VceShaders.cmake's own default points at a
# Windows SDK install. Resolved once, so both builds agree on the compiler.
find_program(VCE_SLANGC slangc HINTS /opt/shader-slang/bin $ENV{VULKAN_SDK}/bin REQUIRED)

# app_shell's ui_metrics.hh includes the generated floor, so the LIBRARY needs
# the dependency too — not just the targets that call into it.
add_dependencies(app_shell generate_ui_min_text_size)

add_executable(nava_launcher
    app/desktop_main.cc
    app/launcher_app.cc
    app/app_list.cc)
add_dependencies(nava_launcher generate_ui_min_text_size)
target_include_directories(nava_launcher PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_link_libraries(nava_launcher PRIVATE app_shell app_shell_wayland vk_canvas_wayland)

# The desktop AssetReader resolves relative to the executable, so "beside it"
# is the whole install layout.
# FileAssetReader roots itself at <exe dir>/assets/, not the exe dir itself.
nava_stage_assets(nava_launcher ${CMAKE_BINARY_DIR}/assets)
nava_stage_font(nava_launcher ${CMAKE_BINARY_DIR}/assets)

# ── Tests ────────────────────────────────────────────────────────────────────
#
# Debug-only, plain assert(), no framework — app_shell's convention. It
# COMPILES app_list.cc rather than linking anything, which is what keeps the
# filter honest about having no dependencies.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_executable(app_list_test tests/app_list_test.cc app/app_list.cc)
    target_include_directories(app_list_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/app)
endif()
