# Dual-mode listfile, same idiom as engine/CMakeLists.txt: normally pulled in by
# the root project via add_subdirectory, but ALSO openable on its own so an IDE
# can show just the examples instead of the engine, tools and tests as well.
#
# Standalone, this mirrors how visutwin-geo consumes the engine: point
# VISUTWIN_ROOT_DIR at the canvas tree and add_subdirectory its engine/ with an
# explicit binary dir (required — the source sits outside CMAKE_SOURCE_DIR).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(visutwin-canvas-examples LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED True)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)

    get_filename_component(VISUTWIN_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
    add_subdirectory("${VISUTWIN_ROOT_DIR}/engine"
        "${CMAKE_BINARY_DIR}/_deps/visutwin-canvas-engine")
endif()

if(NOT DEFINED VISUTWIN_ROOT_DIR)
    get_filename_component(VISUTWIN_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# Asset directory selection:
#   assets-private/ — private test assets (gitignored, not for public distribution)
#   assets/         — freely licensed assets for open-source examples
# Prefer private assets if present, fall back to public.
if(EXISTS "${VISUTWIN_ROOT_DIR}/assets-private")
    set(VISUTWIN_ASSET_DIR "${VISUTWIN_ROOT_DIR}/assets-private")
else()
    set(VISUTWIN_ASSET_DIR "${VISUTWIN_ROOT_DIR}/assets")
endif()

function(visutwin_set_macos_bundle target_name bundle_id)
    set_target_properties(${target_name} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${VISUTWIN_ROOT_DIR}/Info.plist"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${bundle_id}
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${bundle_id}
    )
    target_compile_definitions(${target_name} PRIVATE ASSET_DIR="${VISUTWIN_ASSET_DIR}")
endfunction()

add_executable(visutwin-taa MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/taa-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-taa PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-taa com.visutwin.taa)

add_executable(visutwin-area-picker MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/area-picker-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-area-picker PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-area-picker com.visutwin.area-picker)

add_executable(visutwin-render-to-texture MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/render-to-texture-example.cpp
)
target_link_libraries(visutwin-render-to-texture PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-render-to-texture com.visutwin.render-to-texture)

add_executable(visutwin-layers MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/layers-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-layers PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-layers com.visutwin.layers)

add_executable(visutwin-glb-loader MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/glb-loader-example.cpp
)
target_link_libraries(visutwin-glb-loader PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-glb-loader com.visutwin.glb-loader)

add_executable(visutwin-multi-view MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/multi-view-example.cpp
)
target_link_libraries(visutwin-multi-view PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-multi-view com.visutwin.multi-view)

add_executable(visutwin-raycast MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/raycast-example.cpp
)
target_link_libraries(visutwin-raycast PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-raycast com.visutwin.raycast)

add_executable(visutwin-gizmo-translate MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/gizmo-translate-example.cpp
)
target_link_libraries(visutwin-gizmo-translate PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-gizmo-translate com.visutwin.gizmo-translate)

add_executable(visutwin-world-to-screen MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/world-to-screen-example.cpp
)
target_link_libraries(visutwin-world-to-screen PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-world-to-screen com.visutwin.world-to-screen)

add_executable(visutwin-edge-detect MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/edge-detect-example.cpp
)
target_link_libraries(visutwin-edge-detect PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-edge-detect com.visutwin.edge-detect)

add_executable(visutwin-orbit MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/orbit-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-orbit PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-orbit com.visutwin.orbit)

# Port of the upstream PlayCanvas ambient-occlusion example (laboratory scene).
# PlayCanvas ports are the primary examples, so this one owns the plain name.
add_executable(visutwin-ambient-occlusion MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/ambient-occlusion-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-ambient-occlusion PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-ambient-occlusion com.visutwin.ambient-occlusion)

# The original VisuTwin variant of the same feature: da Vinci workshop scene,
# with a Leonardo bust and a 3D colour LUT on top of the AO demonstration.
add_executable(visutwin-ambient-occlusion-davinci MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/ambient-occlusion-example-davinci.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-ambient-occlusion-davinci PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-ambient-occlusion-davinci
    com.visutwin.ambient-occlusion-davinci)

# --- Tier 1: PlayCanvas-parity examples for already-supported engine features ---
add_executable(visutwin-clearcoat MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/clearcoat-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-clearcoat PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-clearcoat com.visutwin.clearcoat)

add_executable(visutwin-anisotropy MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/anisotropy-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-anisotropy PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-anisotropy com.visutwin.anisotropy)

add_executable(visutwin-procedural-sky MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/procedural-sky-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-procedural-sky PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-procedural-sky com.visutwin.procedural-sky)

add_executable(visutwin-post-processing MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/post-processing-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-post-processing PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-post-processing com.visutwin.post-processing)

add_executable(visutwin-custom-shader MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/custom-shader-example.cpp
)
target_link_libraries(visutwin-custom-shader PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-custom-shader com.visutwin.custom-shader)

add_executable(visutwin-clustered-lighting MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/clustered-lighting-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-clustered-lighting PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-clustered-lighting com.visutwin.clustered-lighting)

# --- Tier 2: PlayCanvas-parity examples for already-supported engine features ---
add_executable(visutwin-transform-rotate MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/transform-rotate-example.cpp
)
target_link_libraries(visutwin-transform-rotate PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-transform-rotate com.visutwin.transform-rotate)

add_executable(visutwin-transform-scale MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/transform-scale-example.cpp
)
target_link_libraries(visutwin-transform-scale PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-transform-scale com.visutwin.transform-scale)

add_executable(visutwin-lights MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/lights-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-lights PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-lights com.visutwin.lights)

add_executable(visutwin-depth-of-field MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/depth-of-field-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-depth-of-field PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-depth-of-field com.visutwin.depth-of-field)

add_executable(visutwin-dynamic-batching MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/dynamic-batching-example.cpp
)
target_link_libraries(visutwin-dynamic-batching PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-dynamic-batching com.visutwin.dynamic-batching)

add_executable(visutwin-blend-trees-2d MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/blend-trees-2d-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-blend-trees-2d PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-blend-trees-2d com.visutwin.blend-trees-2d)

add_executable(visutwin-ui-text MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/ui-text-example.cpp
)
target_link_libraries(visutwin-ui-text PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-ui-text com.visutwin.ui-text)

add_executable(visutwin-shadow-cascades MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/shadow-cascades-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-shadow-cascades PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-shadow-cascades com.visutwin.shadow-cascades)

add_executable(visutwin-mesh-decals MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/mesh-decals-example.cpp
)
target_link_libraries(visutwin-mesh-decals PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-mesh-decals com.visutwin.mesh-decals)

add_executable(visutwin-instancing-basic MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/instancing-basic-example.cpp
)
target_link_libraries(visutwin-instancing-basic PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-instancing-basic com.visutwin.instancing-basic)

add_executable(visutwin-reflection-planar-blurred MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/reflection-planar-blurred-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-reflection-planar-blurred PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-reflection-planar-blurred com.visutwin.reflection-planar-blurred)

add_executable(visutwin-anim-stategraph MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/anim-stategraph-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-anim-stategraph PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-anim-stategraph com.visutwin.anim-stategraph)

add_executable(visutwin-lightmap-bake MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/lightmap-bake-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-lightmap-bake PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-lightmap-bake com.visutwin.lightmap-bake)

add_executable(visutwin-reflection-probe MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/reflection-probe-example.cpp
)
target_link_libraries(visutwin-reflection-probe PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-reflection-probe com.visutwin.reflection-probe)

add_executable(visutwin-reflection-probe-dynamic MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/reflection-probe-dynamic-example.cpp
)
target_link_libraries(visutwin-reflection-probe-dynamic PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-reflection-probe-dynamic com.visutwin.reflection-probe-dynamic)

add_executable(visutwin-gsplat MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/gsplat-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-gsplat PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-gsplat com.visutwin.gsplat)

add_executable(visutwin-area-light MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/area-light-example.cpp
)
target_link_libraries(visutwin-area-light PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-area-light com.visutwin.area-light)

add_executable(visutwin-refraction MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/refraction-example.cpp
)
target_link_libraries(visutwin-refraction PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-refraction com.visutwin.refraction)

add_executable(visutwin-outline-viewcube MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/outline-viewcube-example.cpp
)
target_link_libraries(visutwin-outline-viewcube PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-outline-viewcube com.visutwin.outline-viewcube)

add_executable(visutwin-pcss-dither MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/pcss-dither-example.cpp
)
target_link_libraries(visutwin-pcss-dither PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-pcss-dither com.visutwin.pcss-dither)

add_executable(visutwin-shader-chunks MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/shader-chunks-example.cpp
)
target_link_libraries(visutwin-shader-chunks PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-shader-chunks com.visutwin.shader-chunks)

add_executable(visutwin-particles MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/particles-example.cpp
)
target_link_libraries(visutwin-particles PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-particles com.visutwin.particles)

add_executable(visutwin-pcss-local MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/pcss-local-example.cpp
)
target_link_libraries(visutwin-pcss-local PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-pcss-local com.visutwin.pcss-local)

add_executable(visutwin-morph-anim MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/morph-anim-example.cpp
)
target_link_libraries(visutwin-morph-anim PRIVATE visutwin-canvas)
target_compile_definitions(visutwin-morph-anim PRIVATE ASSET_DIR="${VISUTWIN_ASSET_DIR}")
visutwin_set_macos_bundle(visutwin-morph-anim com.visutwin.morph-anim)
