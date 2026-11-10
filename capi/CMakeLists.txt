# The C API: a shared library over the static engine that exports only the vt_* functions of
# capi/include/visutwin/visutwin.h. VISUTWIN_BUILD_C_API turns it on.
add_library(visutwin-canvas-c SHARED src/visutwin.cpp src/metalCppImplementation.cpp)
set_target_properties(visutwin-canvas-c PROPERTIES
    OUTPUT_NAME visutwin_canvas_c
    VERSION 0.1
    SOVERSION 0
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(visutwin-canvas-c PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
add_library(visutwin::canvas-c ALIAS visutwin-canvas-c)
# Installed beside the engine it links: found there wherever the prefix is.
if(APPLE)
    set_target_properties(visutwin-canvas-c PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
    set_target_properties(visutwin-canvas-c PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
set_target_properties(visutwin-canvas-c PROPERTIES EXPORT_NAME canvas-c)
target_compile_definitions(visutwin-canvas-c PRIVATE VT_BUILDING_LIBRARY)
target_link_libraries(visutwin-canvas-c PRIVATE visutwin-canvas)

# The engine is linked in whole and compiled with default visibility, so the linker is told
# to export the vt_* symbols and nothing else.
if(APPLE)
    target_link_options(visutwin-canvas-c PRIVATE
        "LINKER:-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")
elseif(UNIX)
    target_link_options(visutwin-canvas-c PRIVATE
        "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map")
endif()
set_property(TARGET visutwin-canvas-c APPEND PROPERTY LINK_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/exports.txt ${CMAKE_CURRENT_SOURCE_DIR}/exports.map)

# A C program over the API: a window and a small scene built through property names.
add_executable(visutwin-capi-hello examples/hello.c)
set_target_properties(visutwin-capi-hello PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_link_libraries(visutwin-capi-hello PRIVATE visutwin-canvas-c)

# Installed beside the engine, in its package (visutwin::canvas-c), with its header as
# <visutwin/visutwin.h>: what a frontend loads, and what an extension library links. Over a
# shared engine (VISUTWIN_CANVAS_SHARED) an extension built against the same engine shares its
# property table; over a static one this library carries its own engine.
include(GNUInstallDirs)
install(TARGETS visutwin-canvas-c
    EXPORT VisuTwinCanvasTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES include/visutwin/visutwin.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/visutwin)
