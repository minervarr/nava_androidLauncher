# ── nava_stage_assets(<target> <out_dir>) ────────────────────────────────────
#
# Puts the shaders and the font where the AssetReader will look for them:
# <exe dir>/assets on a desktop, the APK's assets/ on Android.
#
# The shaders are COMPILED from the .slang sources, not copied from the .spv
# files the two submodules commit. That is not a preference — the committed
# msdf_vert.spv predates the atlas-page attribute vk_canvas's text emitter now
# writes (it declares locations 0..2; the current vertex layout has four), and
# binding it renders every glyph as a solid white block, which is exactly what
# it did here before this was compiled properly.
#
# NAVA_ROOT must point at the repository root; the desktop and Android builds
# are rooted in different directories, so neither CMAKE_SOURCE_DIR would mean
# the same thing in both.
function(nava_stage_assets TARGET OUT_DIR)
    set(_vkc  ${VK_CANVAS_DIR})
    set(_font ${_vkc}/first_party/vulkan_font_engine)

    include(${_vkc}/cmake/VceShaders.cmake)

    # The font engine's half: the compute rasteriser, the composite pass, and
    # the MSDF text pipeline.
    vce_compile_slang(nava_shaders_font ${OUT_DIR}/shaders ${_font}/shaders_src
        composite_vert composite_frag tiling coverage msdf_vert msdf_frag)

    # vk_canvas's own: the vector overlay, images, and the SDF shape fast path.
    vce_compile_slang(nava_shaders_canvas ${OUT_DIR}/shaders ${_vkc}/shaders_src
        overlay_vert overlay_frag image_vert image_frag shape_vert shape_frag)

    add_dependencies(${TARGET} nava_shaders_font nava_shaders_canvas)
endfunction()

# ── nava_stage_font(<target> <out_dir>) ──────────────────────────────────────
#
# The DESKTOP half of the same job. Android does not use it: assets/ is listed
# as an extra assets.srcDir in app/build.gradle instead, because AGP merges
# assets BEFORE it runs the native build, so anything a POST_BUILD step copies
# into the module arrives too late to be packaged.
function(nava_stage_font TARGET OUT_DIR)
    add_custom_command(TARGET ${TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory ${OUT_DIR}/fonts
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${NAVA_ROOT}/assets/fonts/ui/ui.otf ${OUT_DIR}/fonts/ui.otf
        COMMENT "Staging font into ${OUT_DIR}")
endfunction()
