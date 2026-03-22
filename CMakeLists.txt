cmake_minimum_required(VERSION 3.28)
project(visutwin-canvas LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED True)
set(CMAKE_CXX_EXTENSIONS OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

set(VISUTWIN_ROOT_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(engine)
add_subdirectory(examples)
add_subdirectory(tools)
