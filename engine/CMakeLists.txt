if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.28)
    project(visutwin-canvas-canvas LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED True)
    set(CMAKE_CXX_EXTENSIONS OFF)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)
endif()

if(NOT DEFINED VISUTWIN_ROOT_DIR)
    get_filename_component(VISUTWIN_ROOT_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
endif()

# ── Installed layout ──────────────────────────────────────────────────
include(GNUInstallDirs)
set(VISUTWIN_CANVAS_VERSION 0.1.0)
set(VISUTWIN_CANVAS_INSTALL_INCLUDE_DIR "${CMAKE_INSTALL_INCLUDEDIR}/visutwin/canvas")
set(VISUTWIN_CANVAS_INSTALL_METALCPP_DIR "${CMAKE_INSTALL_INCLUDEDIR}/visutwin/canvas-metal-cpp")
set(VISUTWIN_CANVAS_INSTALL_SHADER_DIR "${CMAKE_INSTALL_DATADIR}/visutwin-canvas")

# ── Backend selection ─────────────────────────────────────────────────
if(APPLE)
    set(_VISUTWIN_METAL_DEFAULT ON)
    set(_VISUTWIN_VULKAN_DEFAULT OFF)
else()
    set(_VISUTWIN_METAL_DEFAULT OFF)
    set(_VISUTWIN_VULKAN_DEFAULT ON)
endif()

option(VISUTWIN_BACKEND_METAL "Enable Metal backend" ${_VISUTWIN_METAL_DEFAULT})
option(VISUTWIN_BACKEND_VULKAN "Enable Vulkan backend" ${_VISUTWIN_VULKAN_DEFAULT})
# Physics is a seam, not a dependency: the engine defines PhysicsWorld and an
# application supplies one. This option only decides whether the Jolt-backed
# implementation is compiled in alongside it.
option(VISUTWIN_PHYSICS_JOLT "Build the Jolt Physics backend" ON)

# Static by default; several sibling projects consume this engine, and a shared
# build gives each of them one 16 MB dylib to link instead of its own copy of a
# 450 MB Debug archive. vcpkg's osx/linux triplets build the dependencies static,
# so they are absorbed into the dylib rather than becoming runtime requirements.
option(VISUTWIN_CANVAS_SHARED "Build the engine as a shared library" ${BUILD_SHARED_LIBS})

if(VISUTWIN_BACKEND_METAL AND NOT APPLE)
    message(FATAL_ERROR "VISUTWIN_BACKEND_METAL is only supported on Apple platforms")
endif()
if(NOT VISUTWIN_BACKEND_METAL AND NOT VISUTWIN_BACKEND_VULKAN)
    message(FATAL_ERROR "At least one graphics backend must be enabled")
endif()

# ── Common dependencies ──────────────────────────────────────────────
find_package(spdlog        CONFIG REQUIRED)
find_package(SDL3          CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(draco         CONFIG REQUIRED)
find_package(Boost         REQUIRED COMPONENTS core)

foreach(_prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${_prefix}/share/stb/FindStb.cmake")
        list(PREPEND CMAKE_MODULE_PATH "${_prefix}/share/stb")
        break()
    endif()
endforeach()

find_package(Stb MODULE REQUIRED)
set(STB_INCLUDE_DIR "${Stb_INCLUDE_DIR}" CACHE INTERNAL "Path to the vcpkg stb headers")

find_package(assimp        CONFIG REQUIRED)
# ── KTX2 transcoding backend ─────────────────────────────────────────
# The standalone basisu port and libktx each bundle their own copy of
# basis_universal (2.1.0 vs 1.16) and export the same basist:: symbols, so a
# program linking both fails to link. libktx is therefore the default for every
# VisuTwin project: visutwin-geo pulls it in through cesium-native, and an engine
# built the other way cannot be linked into that program at all. The choice is
# compiled in and is reported to consumers as VISUTWIN_CANVAS_KTX2_USE_LIBKTX.
option(VISUTWIN_KTX2_USE_LIBKTX
    "Transcode KTX2 textures via libktx instead of the standalone basisu transcoder" ON)

if(VISUTWIN_KTX2_USE_LIBKTX)
    # KTX::ktx declares a C link interface, so C has to be an enabled language
    # even though the engine itself is C++ only.
    enable_language(C)
    find_package(Ktx        CONFIG REQUIRED)
    set(VISUTWIN_KTX2_BACKEND_LIB KTX::ktx)
else()
    find_package(basisu     CONFIG REQUIRED)
    set(VISUTWIN_KTX2_BACKEND_LIB basisu::basisu_encoder)
endif()

find_package(imgui          CONFIG REQUIRED)
find_package(implot         CONFIG REQUIRED)

find_path(TINYGLTF_INCLUDE_DIRS "tiny_gltf.h")
find_path(NLOHMANN_JSON_INCLUDE_DIRS "nlohmann/json.hpp")

# ── Metal backend setup (Apple only) ─────────────────────────────────
if(VISUTWIN_BACKEND_METAL)
    set(VISUTWIN_CANVAS_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}/cmake")
    if(NOT EXISTS "${VISUTWIN_CANVAS_CMAKE_DIR}/CMakeDetermineMetalCompiler.cmake")
        message(FATAL_ERROR "Missing Metal CMake modules in ${VISUTWIN_CANVAS_CMAKE_DIR}")
    endif()
    list(PREPEND CMAKE_MODULE_PATH "${VISUTWIN_CANVAS_CMAKE_DIR}")

    find_library(METAL_LIBRARY Metal REQUIRED)
    find_library(FOUNDATION_LIBRARY Foundation REQUIRED)
    find_library(QUARTZCORE_LIBRARY QuartzCore REQUIRED)

    include(MetalShaderSupport)
    enable_language(Metal)
endif()

# ── Vulkan backend setup ─────────────────────────────────────────────
if(VISUTWIN_PHYSICS_JOLT)
    find_package(unofficial-joltphysics CONFIG QUIET)
    if(NOT TARGET unofficial::joltphysics::Jolt)
        find_package(Jolt CONFIG QUIET)
    endif()
    if(NOT TARGET unofficial::joltphysics::Jolt AND NOT TARGET Jolt::Jolt)
        message(WARNING
            "VISUTWIN_PHYSICS_JOLT is ON but the joltphysics package was not found; "
            "building without a physics backend")
        set(VISUTWIN_PHYSICS_JOLT OFF)
    endif()
endif()

if(VISUTWIN_BACKEND_VULKAN)
    find_package(VulkanHeaders CONFIG REQUIRED)
    find_package(Vulkan REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    find_package(vk-bootstrap CONFIG REQUIRED)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # Optional runtime compiler for user-provided GLSL. Engine shader modules
    # are always compiled and reflected at build time below.
    find_package(unofficial-shaderc CONFIG QUIET)

    if(NOT Vulkan_GLSLC_EXECUTABLE)
        find_program(Vulkan_GLSLC_EXECUTABLE NAMES glslc REQUIRED)
    endif()
    find_program(VISUTWIN_SPIRV_CROSS_EXECUTABLE
        NAMES spirv-cross
        HINTS
            "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/tools/spirv-cross"
        REQUIRED)
endif()

# ── Source collection ─────────────────────────────────────────────────
file(GLOB_RECURSE VISUTWIN_CANVAS_SOURCES CONFIGURE_DEPENDS
    "${VISUTWIN_ROOT_DIR}/engine/src/*.cpp"
)

# Collect Objective-C++ sources (.mm) for Metal backend
if(VISUTWIN_BACKEND_METAL)
    file(GLOB_RECURSE VISUTWIN_CANVAS_MM_SOURCES CONFIGURE_DEPENDS
        "${VISUTWIN_ROOT_DIR}/engine/src/*.mm"
    )
    list(APPEND VISUTWIN_CANVAS_SOURCES ${VISUTWIN_CANVAS_MM_SOURCES})
endif()

# ── Build-time Vulkan shader bundle ──────────────────────────────────
# GLSL is compiled before C++ and reflected by SPIRV-Cross. The generator
# validates descriptor sets, uniform block sizes, and push constants, then
# embeds both SPIR-V and its reflected layout manifest in one generated header.
if(VISUTWIN_BACKEND_VULKAN)
    set(_VK_SHADER_DIR "${VISUTWIN_ROOT_DIR}/engine/shaders/vulkan")
    set(_VK_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/vulkan")
    set(_VK_SHADER_BUNDLE "${_VK_GENERATED_DIR}/vulkan_shader_bundle.h")
    # EVERY file under the shader directory is a dependency of the bundle: the
    # top-level stages, the chunks forward.frag #includes, and the shared .glsl
    # headers (normal_matrix, shadow_opacity). This used to be a hand-written list
    # of the top-level files only, so editing a chunk or a header did not
    # regenerate the bundle even in a full build — the next run measured the
    # previous binary — and the list had already drifted from the generator's
    # (shadow.frag was compiled but not listed). CONFIGURE_DEPENDS makes Ninja
    # re-glob at build time, so a new chunk file joins without a reconfigure.
    # Nothing outside this directory is #included: the generator's -I paths are
    # this directory and its own work dir, whose files it writes itself.
    file(GLOB_RECURSE _VK_SHADER_SOURCES CONFIGURE_DEPENDS
        "${_VK_SHADER_DIR}/*.vert"
        "${_VK_SHADER_DIR}/*.frag"
        "${_VK_SHADER_DIR}/*.comp"
        "${_VK_SHADER_DIR}/*.glsl"
    )
    add_custom_command(
        OUTPUT "${_VK_SHADER_BUNDLE}"
        COMMAND "${Python3_EXECUTABLE}"
            "${VISUTWIN_ROOT_DIR}/tools/generate_vulkan_shader_bundle.py"
            --glslc "${Vulkan_GLSLC_EXECUTABLE}"
            --spirv-cross "${VISUTWIN_SPIRV_CROSS_EXECUTABLE}"
            --shader-dir "${_VK_SHADER_DIR}"
            --features
                "${VISUTWIN_ROOT_DIR}/engine/src/platform/graphics/shaderFeatures.h"
            --material-fields
                "${VISUTWIN_ROOT_DIR}/engine/src/scene/materials/materialUniformFields.h"
            --output "${_VK_SHADER_BUNDLE}"
            --work-dir "${_VK_GENERATED_DIR}/work"
        DEPENDS
            ${_VK_SHADER_SOURCES}
            "${VISUTWIN_ROOT_DIR}/engine/src/platform/graphics/shaderFeatures.h"
            "${VISUTWIN_ROOT_DIR}/engine/src/scene/materials/materialUniformFields.h"
            "${VISUTWIN_ROOT_DIR}/tools/generate_vulkan_shader_bundle.py"
        COMMENT "Compiling and reflecting Vulkan shader variants"
        VERBATIM)
    add_custom_target(visutwin_vulkan_shaders DEPENDS "${_VK_SHADER_BUNDLE}")
endif()

# Exclude backend sources that are not enabled
if(NOT VISUTWIN_BACKEND_METAL)
    list(FILTER VISUTWIN_CANVAS_SOURCES EXCLUDE REGEX ".*/metal/.*")
endif()
if(NOT VISUTWIN_BACKEND_VULKAN)
    list(FILTER VISUTWIN_CANVAS_SOURCES EXCLUDE REGEX ".*/platform/graphics/vulkan/.*")
endif()

# A file holding a SIMD kernel and the scalar reference it is tested against must
# not let the compiler fuse multiply-add into FMA: GCC and clang both do by
# default (GCC even in strict C++ mode once -mfma is on, which Jolt's exported
# flags turn on), the fused scalar path rounds differently, and the two stop
# agreeing bit for bit.
set_source_files_properties(
    "${VISUTWIN_ROOT_DIR}/engine/src/scene/gsplat/gsplatSortKeys.cpp"
    PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")

# ── Embedded Metal shaders ────────────────────────────────────────────
# Self-contained MSL programs (particle sim/render, gsplat render) live as
# editable .metal files but are embedded into the binary at build time as
# raw-string constants — no runtime filesystem dependency (mirrors the
# areaLightLuts embed, unlike the runtime-loaded ShaderChunks registry).
if(VISUTWIN_BACKEND_METAL)
    set(_MSL_EMBED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders")
    file(MAKE_DIRECTORY "${_MSL_EMBED_DIR}")

    set(_MSL_EMBEDS
        "particle-render.metal|PARTICLE_SHADER_SOURCE"
        "gsplat-render.metal|GSPLAT_SHADER_SOURCE"
    )
    set(_MSL_EMBED_OUTPUTS "")
    foreach(_entry IN LISTS _MSL_EMBEDS)
        string(REPLACE "|" ";" _parts "${_entry}")
        list(GET _parts 0 _msl_name)
        list(GET _parts 1 _msl_var)
        set(_msl_src "${VISUTWIN_ROOT_DIR}/engine/shaders/metal/embedded/${_msl_name}")
        set(_msl_dst "${_MSL_EMBED_DIR}/${_msl_name}.inc")
        add_custom_command(
            OUTPUT "${_msl_dst}"
            COMMAND ${CMAKE_COMMAND}
                -DSRC=${_msl_src} -DDST=${_msl_dst} -DVAR=${_msl_var}
                -P ${VISUTWIN_ROOT_DIR}/tools/embed_msl.cmake
            DEPENDS "${_msl_src}" "${VISUTWIN_ROOT_DIR}/tools/embed_msl.cmake"
            COMMENT "Embedding MSL ${_msl_name}"
            VERBATIM)
        list(APPEND _MSL_EMBED_OUTPUTS "${_msl_dst}")
    endforeach()
    add_custom_target(visutwin_embedded_shaders DEPENDS ${_MSL_EMBED_OUTPUTS})
endif()
if(VISUTWIN_CANVAS_SHARED)
    set(_VISUTWIN_CANVAS_LIBRARY_TYPE SHARED)
else()
    set(_VISUTWIN_CANVAS_LIBRARY_TYPE STATIC)
endif()

add_library(visutwin-canvas ${_VISUTWIN_CANVAS_LIBRARY_TYPE}
    ${VISUTWIN_CANVAS_SOURCES}
)

# Needed either way: static dependencies are absorbed into the dylib in a shared
# build, and a static engine may itself be linked into someone else's shared library.
set_target_properties(visutwin-canvas PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(VISUTWIN_CANVAS_SHARED)
    # Carries metal-cpp's selector globals inside the dylib (see
    # metal/metalCppImplementation.cpp).
    target_compile_definitions(visutwin-canvas PRIVATE VISUTWIN_CANVAS_SHARED_BUILD)
    # Every symbol is exported, as on a static build: the engine has no export
    # macros, so hiding symbols by default would silently drop its whole API.
    # A Windows port would have to introduce them before this works there.
    set_target_properties(visutwin-canvas PROPERTIES
        VERSION ${VISUTWIN_CANVAS_VERSION}
        SOVERSION 0
        MACOSX_RPATH ON)
    if(WIN32)
        message(FATAL_ERROR
            "VISUTWIN_CANVAS_SHARED is not supported on Windows yet: the engine has no "
            "dllexport annotations, so the import library would be empty.")
    endif()
endif()

# Generate the Metal embed headers before compiling and expose their include
# directory only when that backend is part of this build.
if(VISUTWIN_BACKEND_METAL)
    add_dependencies(visutwin-canvas visutwin_embedded_shaders)
    target_include_directories(visutwin-canvas PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()
if(VISUTWIN_BACKEND_VULKAN)
    add_dependencies(visutwin-canvas visutwin_vulkan_shaders)
    target_include_directories(visutwin-canvas PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
endif()

add_library(visutwin::canvas ALIAS visutwin-canvas)
set_target_properties(visutwin-canvas PROPERTIES EXPORT_NAME canvas)

target_include_directories(visutwin-canvas
    PUBLIC
        $<BUILD_INTERFACE:${VISUTWIN_ROOT_DIR}/engine/src>
        $<INSTALL_INTERFACE:${VISUTWIN_CANVAS_INSTALL_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${STB_INCLUDE_DIR}>
        $<BUILD_INTERFACE:${TINYGLTF_INCLUDE_DIRS}>
        $<BUILD_INTERFACE:${NLOHMANN_JSON_INCLUDE_DIRS}>
    PRIVATE
        ${VISUTWIN_ROOT_DIR}
)

# Vulkan-headers shadowing fix.
#
# When a system-wide Vulkan SDK is installed under /usr/local (LunarG SDK on
# macOS, some Linux setups), AppleClang's default-system search list places
# /usr/local/include *before* any -isystem path.  The system vulkan.h may be
# stale relative to vcpkg's version, breaking vk-bootstrap's PFN typedefs.
# We can't reorder default-system paths, but a unique `-I <user>` path is
# searched *before* the system list — so we mirror vcpkg's vulkan headers
# into a build-private directory and add that as a `-I` include.
if(VISUTWIN_BACKEND_VULKAN AND TARGET Vulkan::Headers)
    get_target_property(_VK_HEADERS_DIR Vulkan::Headers INTERFACE_INCLUDE_DIRECTORIES)
    if(_VK_HEADERS_DIR AND EXISTS "${_VK_HEADERS_DIR}/vulkan")
        set(_VK_SHIM_DIR "${CMAKE_BINARY_DIR}/_vulkan_shim")
        file(MAKE_DIRECTORY "${_VK_SHIM_DIR}/vulkan")
        file(GLOB _VK_SHIM_HEADERS
            "${_VK_HEADERS_DIR}/vulkan/*.h"
            "${_VK_HEADERS_DIR}/vulkan/*.hpp")
        file(COPY ${_VK_SHIM_HEADERS} DESTINATION "${_VK_SHIM_DIR}/vulkan")
        # `BEFORE PRIVATE` puts this in the user-include section (-I), which
        # clang searches before its built-in default-system list.  Using a
        # *unique* path avoids being deduplicated against vcpkg's -isystem.
        target_include_directories(visutwin-canvas BEFORE PRIVATE "${_VK_SHIM_DIR}")
    endif()
endif()

target_link_libraries(visutwin-canvas
    PUBLIC
        spdlog::spdlog
        SDL3::SDL3
        tinyobjloader::tinyobjloader
        draco::draco
        Boost::core
        assimp::assimp
        ${VISUTWIN_KTX2_BACKEND_LIB}
        imgui::imgui
        implot::implot
)

target_compile_definitions(visutwin-canvas PUBLIC
    $<$<CONFIG:Debug>:VISUTWIN_DEBUG_GPU_VALIDATION>
)

# Runtime shader-chunk lookup for an INSTALLED engine.
#
# shaderChunks.cpp finds the chunk trees from this file's __FILE__ path and from
# the working directory, which covers every build-tree consumer but finds nothing
# once the library is installed elsewhere. Bake the installed data directory in as
# a last-resort candidate. It is an absolute path, so an installed tree that is
# later moved needs $VISUTWIN_CANVAS_SHADERS instead (also honoured, and checked
# first, so a developer can point a deployed binary at a working tree).
target_compile_definitions(visutwin-canvas PRIVATE
    VISUTWIN_CANVAS_INSTALLED_SHADER_DIR="${CMAKE_INSTALL_PREFIX}/${VISUTWIN_CANVAS_INSTALL_SHADER_DIR}")

if(VISUTWIN_KTX2_USE_LIBKTX)
    target_compile_definitions(visutwin-canvas PRIVATE VISUTWIN_KTX2_USE_LIBKTX)
endif()

# ── Metal backend linking ─────────────────────────────────────────────
if(VISUTWIN_BACKEND_METAL)
    target_compile_definitions(visutwin-canvas PUBLIC VISUTWIN_HAS_METAL)
    target_include_directories(visutwin-canvas PUBLIC
        $<BUILD_INTERFACE:${VISUTWIN_ROOT_DIR}/engine/lib/metal-cpp>
        $<INSTALL_INTERFACE:${VISUTWIN_CANVAS_INSTALL_METALCPP_DIR}>
    )
    target_link_libraries(visutwin-canvas PUBLIC
        "-framework Metal"
        "-framework MetalKit"
        "-framework AppKit"
        "-framework Foundation"
        "-framework QuartzCore"
    )
endif()

# ── Vulkan backend linking ────────────────────────────────────────────
if(VISUTWIN_BACKEND_VULKAN)
    target_compile_definitions(visutwin-canvas PUBLIC VISUTWIN_HAS_VULKAN)
endif()

# ── Physics backend linking ───────────────────────────────────────────
if(VISUTWIN_PHYSICS_JOLT)
    target_compile_definitions(visutwin-canvas PUBLIC VISUTWIN_PHYSICS_JOLT)
    if(TARGET unofficial::joltphysics::Jolt)
        target_link_libraries(visutwin-canvas PUBLIC unofficial::joltphysics::Jolt)
    else()
        target_link_libraries(visutwin-canvas PUBLIC Jolt::Jolt)
    endif()
endif()

if(VISUTWIN_BACKEND_VULKAN)
    target_compile_definitions(visutwin-canvas PUBLIC VISUTWIN_HAS_VULKAN)
    target_link_libraries(visutwin-canvas PUBLIC
        Vulkan::Vulkan
        GPUOpen::VulkanMemoryAllocator
        vk-bootstrap::vk-bootstrap
    )
    if(TARGET unofficial::shaderc::shaderc)
        target_compile_definitions(visutwin-canvas PUBLIC VISUTWIN_HAS_SHADERC)
        target_link_libraries(visutwin-canvas PUBLIC unofficial::shaderc::shaderc)
        message(STATUS "Vulkan runtime shader compilation enabled (shaderc)")
    else()
        message(STATUS "shaderc not found - runtime custom GLSL is disabled")
    endif()
endif()

# ── Install and export ────────────────────────────────────────────────
# Consumers include engine headers as "core/...", "framework/...", so the source
# tree is installed under one subdirectory that becomes the include root. That
# keeps every existing #include working while avoiding generic names in a shared
# include prefix.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(VISUTWIN_CANVAS_SHARED)
    # Consumers of the installed dylib find it through this path.
    set_target_properties(visutwin-canvas PROPERTIES
        INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS visutwin-canvas
    EXPORT VisuTwinCanvasTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${VISUTWIN_CANVAS_INSTALL_INCLUDE_DIR})

install(DIRECTORY "${VISUTWIN_ROOT_DIR}/engine/src/"
    DESTINATION ${VISUTWIN_CANVAS_INSTALL_INCLUDE_DIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.inl")

if(VISUTWIN_BACKEND_METAL)
    install(DIRECTORY "${VISUTWIN_ROOT_DIR}/engine/lib/metal-cpp/"
        DESTINATION ${VISUTWIN_CANVAS_INSTALL_METALCPP_DIR}
        FILES_MATCHING PATTERN "*.h" PATTERN "*.hpp")
endif()

# Runtime-loaded chunk trees, in the layout shaderChunks.cpp expects below its root.
install(DIRECTORY "${VISUTWIN_ROOT_DIR}/engine/shaders/metal/chunks"
    DESTINATION "${VISUTWIN_CANVAS_INSTALL_SHADER_DIR}/engine/shaders/metal")
install(DIRECTORY "${VISUTWIN_ROOT_DIR}/engine/shaders/vulkan/chunks"
    DESTINATION "${VISUTWIN_CANVAS_INSTALL_SHADER_DIR}/engine/shaders/vulkan")

install(FILES "${VISUTWIN_ROOT_DIR}/LICENSE" "${VISUTWIN_ROOT_DIR}/NOTICE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/visutwin-canvas")

if(TARGET unofficial::shaderc::shaderc)
    set(VISUTWIN_HAS_SHADERC ON)
else()
    set(VISUTWIN_HAS_SHADERC OFF)
endif()

configure_package_config_file(
    "${VISUTWIN_ROOT_DIR}/cmake/VisuTwinCanvasConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/VisuTwinCanvasConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/VisuTwinCanvas"
    PATH_VARS VISUTWIN_CANVAS_INSTALL_SHADER_DIR)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/VisuTwinCanvasConfigVersion.cmake"
    VERSION ${VISUTWIN_CANVAS_VERSION}
    COMPATIBILITY SameMinorVersion)

install(EXPORT VisuTwinCanvasTargets
    FILE VisuTwinCanvasTargets.cmake
    NAMESPACE visutwin::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/VisuTwinCanvas")
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/VisuTwinCanvasConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/VisuTwinCanvasConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/VisuTwinCanvas")
