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
target_include_directories(visutwin-canvas-c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
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
