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

# Every example is the same target shape, named as upstream's example browser names it: a
# CATEGORY and an example NAME, both kebab-case folder names of upstream's tree. The source is
# src/<category>/<name>.cpp, the program lands at <build>/examples/<category>/<name>(.app), the
# target is visutwin-<category>-<name> (target names must be unique across categories), and
# VISUTWIN_EXAMPLE_CATEGORY / VISUTWIN_EXAMPLE_NAME tell ExampleApp who it is: the window
# title, the bundle id and the pipeline cache name all come from them.
#
#   visutwin_add_example(materials clear-coat)              # src/materials/clear-coat.cpp
#   visutwin_add_example(misc foo EXTRA_SOURCES src/misc/fooUtil.cpp)
function(visutwin_add_example category example_name)
    cmake_parse_arguments(ARG "" "" "EXTRA_SOURCES" ${ARGN})

    set(target "visutwin-${category}-${example_name}")
    add_executable(${target} MACOSX_BUNDLE
        ${VISUTWIN_ROOT_DIR}/examples/src/${category}/${example_name}.cpp
        ${VISUTWIN_ROOT_DIR}/examples/exampleApp.cpp
        ${ARG_EXTRA_SOURCES}
    )
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME ${example_name}
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/${category}
        FOLDER "examples/${category}"
    )
    target_include_directories(${target} PRIVATE ${VISUTWIN_ROOT_DIR}/examples)
    target_compile_definitions(${target} PRIVATE
        VISUTWIN_EXAMPLE_CATEGORY="${category}"
        VISUTWIN_EXAMPLE_NAME="${example_name}"
    )
    target_link_libraries(${target} PRIVATE visutwin-canvas)
    visutwin_set_macos_bundle(${target} com.visutwin.${category}.${example_name})
endfunction()

visutwin_add_example(animation blend-trees-1d)
visutwin_add_example(animation blend-trees-2d-cartesian)
visutwin_add_example(animation blend-trees-2d-directional)
visutwin_add_example(animation component-properties)
visutwin_add_example(animation crowd)
visutwin_add_example(animation events)
visutwin_add_example(animation layer-masks)
visutwin_add_example(animation locomotion)
visutwin_add_example(animation tween)

visutwin_add_example(camera-frame ambient-occlusion)
visutwin_add_example(camera-frame ambient-occlusion-davinci)
visutwin_add_example(camera-frame anti-aliasing)
visutwin_add_example(camera-frame auto-exposure)
visutwin_add_example(camera-frame custom-compose-shader)
visutwin_add_example(camera-frame depth-of-field)
visutwin_add_example(camera-frame dynamic-sky)
visutwin_add_example(camera-frame hdr)
visutwin_add_example(camera-frame ink-outline)
visutwin_add_example(camera-frame lens-flare)
visutwin_add_example(camera-frame post-processing)
visutwin_add_example(camera-frame taa)
visutwin_add_example(camera-frame volumetric-fog)
visutwin_add_example(camera-frame volumetric-fog-local-lights)
visutwin_add_example(camera-frame volumetric-fog-shafts)

visutwin_add_example(camera fly)
visutwin_add_example(camera multi)
visutwin_add_example(camera orbit)

visutwin_add_example(compute edge-detect)
visutwin_add_example(compute histogram)
visutwin_add_example(compute indirect-dispatch)
visutwin_add_example(compute indirect-draw)
visutwin_add_example(compute particles)
visutwin_add_example(compute scene-maps)
visutwin_add_example(compute texture-gen)
visutwin_add_example(compute vertex-update)

visutwin_add_example(debug frustum-culling)
visutwin_add_example(debug grid)
visutwin_add_example(debug lines)
visutwin_add_example(debug wire-shapes)

visutwin_add_example(demos car-showcase)

visutwin_add_example(gaussian-splatting-fx shadows)

visutwin_add_example(gaussian-splatting simple)
visutwin_add_example(gaussian-splatting simple-sh)

visutwin_add_example(gizmos bounding-box)
visutwin_add_example(gizmos camera)
visutwin_add_example(gizmos icon)
visutwin_add_example(gizmos light)
visutwin_add_example(gizmos shape)
visutwin_add_example(gizmos transform-rotate)
visutwin_add_example(gizmos transform-scale)
visutwin_add_example(gizmos transform-translate)

visutwin_add_example(graphics-advanced layers)
visutwin_add_example(graphics-advanced multi-draw)
visutwin_add_example(graphics-advanced multi-draw-instanced)
visutwin_add_example(graphics-advanced portal)

visutwin_add_example(graphics area-picker)
visutwin_add_example(graphics batching-dynamic)
visutwin_add_example(graphics clipping-planes)
visutwin_add_example(graphics dithered-transparency)
visutwin_add_example(graphics hierarchy)
visutwin_add_example(graphics instancing-basic)
visutwin_add_example(graphics instancing-custom)
visutwin_add_example(graphics instancing-glb)
visutwin_add_example(graphics instancing-gooch)
visutwin_add_example(graphics mesh-decals)
visutwin_add_example(graphics mesh-deformation)
visutwin_add_example(graphics mesh-generation)
visutwin_add_example(graphics mesh-morph)
visutwin_add_example(graphics mesh-morph-many)
visutwin_add_example(graphics model-asset)
visutwin_add_example(graphics model-textured-box)
visutwin_add_example(graphics multi-view)
visutwin_add_example(graphics order-independent-transparency)
visutwin_add_example(graphics outlines-colored)
visutwin_add_example(graphics procedural-sky)
visutwin_add_example(graphics reflection-box)
visutwin_add_example(graphics reflection-cubemap)
visutwin_add_example(graphics reflection-planar)
visutwin_add_example(graphics reflection-planar-blurred)
visutwin_add_example(graphics render-asset)
visutwin_add_example(graphics shapes)
visutwin_add_example(graphics sky)
visutwin_add_example(graphics texture-basis)
visutwin_add_example(graphics visibility-range)
visutwin_add_example(graphics water)
visutwin_add_example(graphics wide-line)
visutwin_add_example(graphics wide-lines-dynamic)
visutwin_add_example(graphics wide-lines-styles)

visutwin_add_example(input gamepad)
visutwin_add_example(input keyboard)
visutwin_add_example(input mouse)

visutwin_add_example(lights-and-shadows area-lights)
visutwin_add_example(lights-and-shadows clustered-area-lights)
visutwin_add_example(lights-and-shadows clustered-light-cookies)
visutwin_add_example(lights-and-shadows clustered-lighting)
visutwin_add_example(lights-and-shadows clustered-omni-shadows)
visutwin_add_example(lights-and-shadows clustered-spot-shadows)
visutwin_add_example(lights-and-shadows light-physical-units)
visutwin_add_example(lights-and-shadows lights)
visutwin_add_example(lights-and-shadows lights-baked)
visutwin_add_example(lights-and-shadows lights-baked-a-o)
visutwin_add_example(lights-and-shadows lights-non-clustered)
visutwin_add_example(lights-and-shadows shadow-bounds)
visutwin_add_example(lights-and-shadows shadow-cascades)
visutwin_add_example(lights-and-shadows shadow-catcher)
visutwin_add_example(lights-and-shadows shadow-soft)

visutwin_add_example(loaders draco-glb)
visutwin_add_example(loaders glb)
visutwin_add_example(loaders obj)

visutwin_add_example(materials anisotropy-disc)
visutwin_add_example(materials anisotropy-lamp)
visutwin_add_example(materials anisotropy-rotation)
visutwin_add_example(materials anisotropy-strength)
visutwin_add_example(materials clear-coat)
visutwin_add_example(materials diffuse-transmission)
visutwin_add_example(materials dispersion)
visutwin_add_example(materials material-anisotropic)
visutwin_add_example(materials material-physical)
visutwin_add_example(materials material-refraction)
visutwin_add_example(materials material-translucent-specular)
visutwin_add_example(materials material-transparency)
visutwin_add_example(materials normals-and-tangents)
visutwin_add_example(materials parallax-mapping)
visutwin_add_example(materials transmission-roughness)

visutwin_add_example(misc annotations)
visutwin_add_example(misc hello-world)

visutwin_add_example(particles anim-index)
visutwin_add_example(particles impact-effects)
visutwin_add_example(particles mesh)
visutwin_add_example(particles random-sprites)
visutwin_add_example(particles snow)
visutwin_add_example(particles spark)

visutwin_add_example(physics compound-collision)
visutwin_add_example(physics falling-shapes)
visutwin_add_example(physics joints)
visutwin_add_example(physics mesh-collider-scaling)
visutwin_add_example(physics offset-collision)
visutwin_add_example(physics raycast)

visutwin_add_example(render-targets custom-msaa-resolve)
visutwin_add_example(render-targets integer-textures)
visutwin_add_example(render-targets msaa-depth-fog)
visutwin_add_example(render-targets multi-render-targets)
visutwin_add_example(render-targets render-pass-camera)
visutwin_add_example(render-targets render-pass-procedural-texture)
visutwin_add_example(render-targets render-pass-shader-quad)
visutwin_add_example(render-targets render-to-texture)
visutwin_add_example(render-targets texture-array-impostors)
visutwin_add_example(render-targets volume-texture)

visutwin_add_example(shaders grab-pass)
visutwin_add_example(shaders ground-fog)
visutwin_add_example(shaders shader-burn)
visutwin_add_example(shaders shader-material)
visutwin_add_example(shaders shader-toon)
visutwin_add_example(shaders shader-wobble)

visutwin_add_example(test alpha-to-coverage)
visutwin_add_example(test attenuation)
visutwin_add_example(test contact-hardening-shadows)
visutwin_add_example(test dual-source-blending)
visutwin_add_example(test gsplat-shadow-orientation)
visutwin_add_example(test independent-blending)
visutwin_add_example(test light-masks)
visutwin_add_example(test lightmap-sources)
visutwin_add_example(test material-instance-overrides)
visutwin_add_example(test material-texture-transforms)
visutwin_add_example(test opacity)
visutwin_add_example(test primitive-mode)
visutwin_add_example(test render-target-orientation)
visutwin_add_example(test scene-depth-read)
visutwin_add_example(test specular)
visutwin_add_example(test taa-alpha)
visutwin_add_example(test text-markup)
visutwin_add_example(test texture-copy)
visutwin_add_example(test texture-formats)
visutwin_add_example(test texture-read)
visutwin_add_example(test two-sided-lighting)

visutwin_add_example(user-interface anchors)
visutwin_add_example(user-interface buttons)
visutwin_add_example(user-interface common-widgets)
visutwin_add_example(user-interface custom-shader)
visutwin_add_example(user-interface drag-and-drop)
visutwin_add_example(user-interface image-fit)
visutwin_add_example(user-interface input-events)
visutwin_add_example(user-interface layout-group)
visutwin_add_example(user-interface masking)
visutwin_add_example(user-interface panel)
visutwin_add_example(user-interface particle-system)
visutwin_add_example(user-interface render-to-image)
visutwin_add_example(user-interface screen-scaling)
visutwin_add_example(user-interface scroll-view)
visutwin_add_example(user-interface text)
visutwin_add_example(user-interface text-auto-font-size)
visutwin_add_example(user-interface text-justify)
visutwin_add_example(user-interface text-localization)
visutwin_add_example(user-interface text-markup)
visutwin_add_example(user-interface text-typewriter)
visutwin_add_example(user-interface world-to-screen)
visutwin_add_example(user-interface world-ui)
