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
option(VISUTWIN_BACKEND_VULKAN "Enable Vulkan backend" OFF)

if(APPLE)
    set(VISUTWIN_BACKEND_METAL ON)
else()
    set(VISUTWIN_BACKEND_METAL OFF)
    set(VISUTWIN_BACKEND_VULKAN ON)
endif()

# ── Common dependencies ──────────────────────────────────────────────
find_package(spdlog        CONFIG REQUIRED)
find_package(SDL3          CONFIG REQUIRED)
find_package(tinyobjloader CONFIG REQUIRED)
find_package(draco         CONFIG QUIET)
find_package(Boost         REQUIRED COMPONENTS core)
find_package(assimp        CONFIG REQUIRED)

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
    find_package(Vulkan REQUIRED)
    find_package(VulkanMemoryAllocator CONFIG REQUIRED)
    find_package(vk-bootstrap CONFIG REQUIRED)
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

# Exclude backend sources that are not enabled
if(NOT VISUTWIN_BACKEND_METAL)
    list(FILTER VISUTWIN_CANVAS_SOURCES EXCLUDE REGEX ".*/metal/.*")
endif()
if(NOT VISUTWIN_BACKEND_VULKAN)
    list(FILTER VISUTWIN_CANVAS_SOURCES EXCLUDE REGEX ".*/vulkan/.*")
endif()

add_library(visutwin-canvas STATIC
    ${VISUTWIN_CANVAS_SOURCES}
)

add_library(visutwin::canvas ALIAS visutwin-canvas)

target_include_directories(visutwin-canvas
    PUBLIC
        ${VISUTWIN_ROOT_DIR}/engine/src
        ${VISUTWIN_ROOT_DIR}/engine/lib/stb
        ${TINYGLTF_INCLUDE_DIRS}
        ${NLOHMANN_JSON_INCLUDE_DIRS}
    PRIVATE
        ${VISUTWIN_ROOT_DIR}
)

target_link_libraries(visutwin-canvas
    PUBLIC
        spdlog::spdlog
        SDL3::SDL3
        tinyobjloader::tinyobjloader
        draco::draco
        Boost::core
        assimp::assimp
        imgui::imgui
        implot::implot
)

target_compile_definitions(visutwin-canvas PUBLIC)

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
endif()
