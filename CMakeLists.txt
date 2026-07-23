cmake_minimum_required(VERSION 3.28)
project(visutwin-canvas LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(VISUTWIN_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

option(VISUTWIN_BUILD_EXAMPLES "Build the VisuTwin Canvas example applications" OFF)

add_subdirectory(engine)
if(VISUTWIN_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
add_subdirectory(tools)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
