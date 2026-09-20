# SPDX-License-Identifier: Apache-2.0
# Copyright 2025-2026 Arnis Lektauers
#
# Builds the engine from a LOCAL checkout rather than a published archive: this
# is a sibling project that moves with its consumers, so there is no released
# tarball to hash. VISUTWIN_CANVAS_SOURCE_DIR selects the checkout and defaults
# to a sibling of the port tree (../../../ from this file, i.e. the canvas root).
#
# Pass it explicitly when the layout differs:
#   -DVCPKG_OVERLAY_PORTS=<canvas>/vcpkg-overlays/ports
#   -DVISUTWIN_CANVAS_SOURCE_DIR=<canvas>
#
# Because the source is a working tree rather than a fixed revision, vcpkg
# cannot tell one build from another: bump "version" in this port's vcpkg.json
# (or clear the binary cache) after changing engine sources, or consumers will
# keep the previously built package.
if(NOT DEFINED VISUTWIN_CANVAS_SOURCE_DIR)
    get_filename_component(VISUTWIN_CANVAS_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
endif()

if(NOT EXISTS "${VISUTWIN_CANVAS_SOURCE_DIR}/engine/CMakeLists.txt")
    message(FATAL_ERROR
        "visutwin-canvas sources not found at ${VISUTWIN_CANVAS_SOURCE_DIR}. "
        "Set VISUTWIN_CANVAS_SOURCE_DIR to the root of the visutwin-canvas checkout.")
endif()

set(SOURCE_PATH "${VISUTWIN_CANVAS_SOURCE_DIR}")

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        jolt               VISUTWIN_PHYSICS_JOLT
        vulkan             VISUTWIN_BACKEND_VULKAN
    INVERTED_FEATURES
        basisu-transcoder  VISUTWIN_KTX2_USE_LIBKTX
)

# The engine is the deliverable; examples and tests are not part of the package.
vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
        -DVISUTWIN_CANVAS_SHARED=ON
        -DVISUTWIN_BUILD_EXAMPLES=OFF
        -DBUILD_TESTING=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME VisuTwinCanvas CONFIG_PATH lib/cmake/VisuTwinCanvas)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" "${SOURCE_PATH}/NOTICE")
