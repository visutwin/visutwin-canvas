cmake_minimum_required(VERSION 3.28)
project(visutwin-canvas LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(VISUTWIN_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

option(VISUTWIN_BUILD_EXAMPLES "Build the VisuTwin Canvas example applications" OFF)

# Sanitizers for this project's own targets (engine, tests, examples, tools), as a
# semicolon list: "address;undefined" is what the `sanitize` preset and CI use. vcpkg's
# dependencies stay uninstrumented, which AddressSanitizer tolerates. Undefined
# behaviour ABORTS rather than printing and carrying on, so a test that trips it fails.
set(VISUTWIN_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address;undefined (empty = none)")
if(VISUTWIN_SANITIZE)
    list(JOIN VISUTWIN_SANITIZE "," _visutwin_sanitizers)
    add_compile_options(-fsanitize=${_visutwin_sanitizers} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${_visutwin_sanitizers})
    if("undefined" IN_LIST VISUTWIN_SANITIZE)
        add_compile_options(-fno-sanitize-recover=undefined)
    endif()
    message(STATUS "VisuTwin: building with sanitizers ${_visutwin_sanitizers}")
endif()

# Warnings for this project's own targets (vcpkg's dependencies are prebuilt, and
# their headers arrive as system includes). -Wmissing-field-initializers is off
# because partial aggregate initialisation (`AnimStateDesc{name}`) is an idiom here,
# not an omission. Turning these on found a virtual-less RenderPass base, a dead
# Metal vertex-layout stub and a dozen settings stored and never read.
option(VISUTWIN_WARNINGS "Build with -Wall -Wextra" ON)
option(VISUTWIN_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
if(VISUTWIN_WARNINGS AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Wall;-Wextra;-Wno-missing-field-initializers>")
    if(VISUTWIN_WARNINGS_AS_ERRORS)
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX,OBJCXX>:-Werror>")
    endif()
endif()

add_subdirectory(engine)
if(VISUTWIN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
add_subdirectory(tools)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
