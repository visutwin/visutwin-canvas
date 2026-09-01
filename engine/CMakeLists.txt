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
# program linking both fails to link. Consumers that already pull in libktx —
# via cesium-native, for instance — select it here and get a single copy.
option(VISUTWIN_KTX2_USE_LIBKTX
    "Transcode KTX2 textures via libktx instead of the standalone basisu transcoder" OFF)

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
    set(_VK_SHADER_SOURCES
        "${_VK_SHADER_DIR}/forward.vert"
        "${_VK_SHADER_DIR}/forward_instanced.vert"
        "${_VK_SHADER_DIR}/forward_dynamic_batch.vert"
        "${_VK_SHADER_DIR}/forward_skinned.vert"
        "${_VK_SHADER_DIR}/forward_morphed.vert"
        "${_VK_SHADER_DIR}/forward_skinned_morphed.vert"
        "${_VK_SHADER_DIR}/forward_sky.vert"
        "${_VK_SHADER_DIR}/forward_color.vert"
        "${_VK_SHADER_DIR}/forward_point.vert"
        "${_VK_SHADER_DIR}/forward.frag"
        "${_VK_SHADER_DIR}/shadow_vsm_moments.frag"
        "${_VK_SHADER_DIR}/post_fullscreen.vert"
        "${_VK_SHADER_DIR}/post_compose.frag"
        "${_VK_SHADER_DIR}/post_ssao.frag"
        "${_VK_SHADER_DIR}/post_depth_blur.frag"
        "${_VK_SHADER_DIR}/post_taa.frag"
        "${_VK_SHADER_DIR}/post_coc.frag"
        "${_VK_SHADER_DIR}/post_dof_blur.frag"
        "${_VK_SHADER_DIR}/env_reproject.frag"
        "${_VK_SHADER_DIR}/instance_cull.comp"
        "${_VK_SHADER_DIR}/particle_sim.comp"
        "${_VK_SHADER_DIR}/particle.vert"
        "${_VK_SHADER_DIR}/particle.frag"
        "${_VK_SHADER_DIR}/gsplat.vert"
        "${_VK_SHADER_DIR}/gsplat.frag"
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
            --output "${_VK_SHADER_BUNDLE}"
            --work-dir "${_VK_GENERATED_DIR}/work"
        DEPENDS
            ${_VK_SHADER_SOURCES}
            "${VISUTWIN_ROOT_DIR}/engine/src/platform/graphics/shaderFeatures.h"
            "${VISUTWIN_ROOT_DIR}/tools/generate_vulkan_shader_bundle.py"
        COMMENT "Compiling and reflecting Vulkan shader variants"
        VERBATIM)
    add_custom_target(visutwin_vulkan_shaders DEPENDS "${_VK_SHADER_BUNDLE}")
endif()

# Exclude backend sources that are not enabled
if(NOT VISUTWIN_BACKEND_METAL)
    list(FILTER VISUTWIN_CANVAS_SOURCES EXCLUDE REGEX ".*/metal/.*")
    # The current ImGui overlay is implemented with imgui_impl_metal.
    list(FILTER VISUTWIN_CANVAS_SOURCES EXCLUDE REGEX ".*/viz/overlay/imguiOverlay\\.cpp$")
    # The MiniStats HUD draws through that overlay, so it goes with it.
    list(FILTER VISUTWIN_CANVAS_SOURCES EXCLUDE REGEX ".*/framework/extras/miniStats\\.cpp$")
endif()
if(NOT VISUTWIN_BACKEND_VULKAN)
    list(FILTER VISUTWIN_CANVAS_SOURCES EXCLUDE REGEX ".*/vulkan/.*")
endif()

# ── Embedded Metal shaders ────────────────────────────────────────────
# Self-contained MSL programs (particle sim/render, gsplat render) live as
# editable .metal files but are embedded into the binary at build time as
# raw-string constants — no runtime filesystem dependency (mirrors the
# areaLightLuts embed, unlike the runtime-loaded ShaderChunks registry).
if(VISUTWIN_BACKEND_METAL)
    set(_MSL_EMBED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_shaders")
    file(MAKE_DIRECTORY "${_MSL_EMBED_DIR}")

    set(_MSL_EMBEDS
        "particle-sim.metal|PARTICLE_SIM_KERNEL"
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
add_library(visutwin-canvas STATIC
    ${VISUTWIN_CANVAS_SOURCES}
)

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

target_include_directories(visutwin-canvas
    PUBLIC
        ${VISUTWIN_ROOT_DIR}/engine/src
        ${STB_INCLUDE_DIR}
        ${TINYGLTF_INCLUDE_DIRS}
        ${NLOHMANN_JSON_INCLUDE_DIRS}
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

if(VISUTWIN_KTX2_USE_LIBKTX)
    target_compile_definitions(visutwin-canvas PRIVATE VISUTWIN_KTX2_USE_LIBKTX)
endif()

# ── Metal backend linking ─────────────────────────────────────────────
if(VISUTWIN_BACKEND_METAL)
    target_compile_definitions(visutwin-canvas PUBLIC VISUTWIN_HAS_METAL)
    target_include_directories(visutwin-canvas PUBLIC
        ${VISUTWIN_ROOT_DIR}/engine/lib/metal-cpp
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
