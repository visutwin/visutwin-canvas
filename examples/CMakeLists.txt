# Dual-mode listfile, same idiom as engine/CMakeLists.txt: normally pulled in by
# the root project via add_subdirectory, but ALSO openable on its own so an IDE
# can show just the examples instead of the engine, tools and tests as well.
#
# Standalone, this mirrors how visutwin-geo consumes the engine: point
# VISUTWIN_ROOT_DIR at the canvas tree and add_subdirectory its engine/ with an
# explicit binary dir (required — the source sits outside CMAKE_SOURCE_DIR).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    # C as well as CXX: the engine links KTX::ktx, whose imported target carries
    # INTERFACE_COMPILE_FEATURES "c_std_11", and CMake resolves that against the
    # TOP-LEVEL project's languages. The engine's own enable_language(C) comes too
    # late for that, so a standalone configure failed to generate with "No known
    # features for C compiler". The root project declares C CXX for the same reason.
    project(visutwin-canvas-examples LANGUAGES C CXX)
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

# Every example is the same target shape: an executable named visutwin-<name>
# built from src/<name>-example.cpp, the shared ExampleApp host and the
# CameraControls script, linked against the engine and bundled for macOS.
#
#   visutwin_add_example(clearcoat)                        # src/clearcoat-example.cpp
#   visutwin_add_example(foo SOURCE foo-example-alt.cpp)   # when the file name differs
#   visutwin_add_example(bar EXTRA_SOURCES src/barUtil.cpp)
function(visutwin_add_example example_name)
    cmake_parse_arguments(ARG "" "SOURCE" "EXTRA_SOURCES" ${ARGN})

    set(target "visutwin-${example_name}")

    if(ARG_SOURCE)
        set(example_source "${VISUTWIN_ROOT_DIR}/examples/src/${ARG_SOURCE}")
    else()
        set(example_source "${VISUTWIN_ROOT_DIR}/examples/src/${example_name}-example.cpp")
    endif()

    add_executable(${target} MACOSX_BUNDLE
        ${example_source}
        ${VISUTWIN_ROOT_DIR}/examples/exampleApp.cpp
        ${ARG_EXTRA_SOURCES}
    )
    target_link_libraries(${target} PRIVATE visutwin-canvas)
    visutwin_set_macos_bundle(${target} com.visutwin.${example_name})
endfunction()

visutwin_add_example(particles-anim-index)
visutwin_add_example(particles-spark)
visutwin_add_example(parallax-mapping)
visutwin_add_example(falling-shapes)
visutwin_add_example(physics-joints)
visutwin_add_example(clustered-spot-shadows)
visutwin_add_example(mesh-morph)
visutwin_add_example(wide-line)
visutwin_add_example(taa)
visutwin_add_example(area-picker)
visutwin_add_example(render-to-texture)
visutwin_add_example(layers)
visutwin_add_example(glb-loader)
visutwin_add_example(multi-view)
visutwin_add_example(raycast)
visutwin_add_example(transform-translate)
visutwin_add_example(world-to-screen)
visutwin_add_example(annotations)
visutwin_add_example(edge-detect)
visutwin_add_example(orbit)
visutwin_add_example(ambient-occlusion)
visutwin_add_example(ambient-occlusion-davinci SOURCE ambient-occlusion-example-davinci.cpp)
visutwin_add_example(clearcoat)
visutwin_add_example(anisotropy)
visutwin_add_example(procedural-sky)
visutwin_add_example(post-processing)
visutwin_add_example(custom-shader)
visutwin_add_example(clustered-lighting)
visutwin_add_example(clustered-light-cookies)
visutwin_add_example(clustered-omni-shadows)
visutwin_add_example(clustered-area-lights)
visutwin_add_example(lights-non-clustered)
visutwin_add_example(light-physical-units)
visutwin_add_example(shadow-catcher)
visutwin_add_example(volumetric-fog-shafts)
visutwin_add_example(draco-glb)
visutwin_add_example(instancing-glb)
visutwin_add_example(obj-loader)
visutwin_add_example(two-sided-lighting)
visutwin_add_example(gsplat-spherical-harmonics)
visutwin_add_example(light-masks)
visutwin_add_example(opacity)
visutwin_add_example(material-texture-transforms)
visutwin_add_example(transform-rotate)
visutwin_add_example(transform-scale)
visutwin_add_example(lights)
visutwin_add_example(depth-of-field)
visutwin_add_example(dynamic-batching)
visutwin_add_example(blend-trees-2d)
visutwin_add_example(ui-text)
visutwin_add_example(ui-text-markup)
visutwin_add_example(ui-buttons)
visutwin_add_example(world-ui)
visutwin_add_example(input-events)
visutwin_add_example(screen-scaling)
visutwin_add_example(ui-panel)
visutwin_add_example(text-justify)
visutwin_add_example(text-typewriter)
visutwin_add_example(masking)
visutwin_add_example(layout-group)
visutwin_add_example(scroll-view)
visutwin_add_example(common-widgets)
visutwin_add_example(anchors)
visutwin_add_example(image-fit)
visutwin_add_example(drag-and-drop)
visutwin_add_example(render-to-image)
visutwin_add_example(text-auto-font-size)
visutwin_add_example(text-localization)
visutwin_add_example(ui-custom-shader)
visutwin_add_example(ui-particle-system)
visutwin_add_example(shadow-cascades)
visutwin_add_example(volumetric-fog-local-lights)
visutwin_add_example(particles-mesh)
visutwin_add_example(particles-snow)
visutwin_add_example(mesh-decals)
visutwin_add_example(instancing-basic)
visutwin_add_example(reflection-planar-blurred)
visutwin_add_example(anim-stategraph)
visutwin_add_example(lightmap-bake)
visutwin_add_example(lightmap-sources)
visutwin_add_example(reflection-probe-dynamic)
visutwin_add_example(gsplat)
visutwin_add_example(area-light)
visutwin_add_example(refraction)
visutwin_add_example(pcss-dither)
visutwin_add_example(particles)
visutwin_add_example(pcss-local)
