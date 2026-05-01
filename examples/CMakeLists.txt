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

add_executable(visutwin-texture-stream MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/texture-stream-example.cpp
)
target_link_libraries(visutwin-texture-stream PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-texture-stream com.visutwin.texture-stream)

add_executable(visutwin-ambient-occlusion MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/ambient-occlusion-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-ambient-occlusion PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-ambient-occlusion com.visutwin.ambient-occlusion)

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

add_executable(visutwin-instancing-culled MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/instancing-culled-example.cpp
)
target_link_libraries(visutwin-instancing-culled PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-instancing-culled com.visutwin.instancing-culled)

add_executable(visutwin-assimp-loader MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/assimp-loader-example.cpp
)
target_link_libraries(visutwin-assimp-loader PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-assimp-loader com.visutwin.assimp-loader)

add_executable(visutwin-reflection-planar-blurred MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/reflection-planar-blurred-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-reflection-planar-blurred PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-reflection-planar-blurred com.visutwin.reflection-planar-blurred)

add_executable(visutwin-animation MACOSX_BUNDLE
    ${VISUTWIN_ROOT_DIR}/examples/src/animation-example.cpp
    ${VISUTWIN_ROOT_DIR}/examples/src/cameraControls.cpp
)
target_link_libraries(visutwin-animation PRIVATE visutwin-canvas)
visutwin_set_macos_bundle(visutwin-animation com.visutwin.animation)

add_executable(visutwin-env-reproject-test
    ${VISUTWIN_ROOT_DIR}/examples/src/env-reproject-test-example.cpp
)
target_link_libraries(visutwin-env-reproject-test PRIVATE visutwin-canvas)
target_compile_definitions(visutwin-env-reproject-test PRIVATE ASSET_DIR="${VISUTWIN_ASSET_DIR}")
