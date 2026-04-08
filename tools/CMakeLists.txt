cmake_minimum_required(VERSION 3.28)

# --- generate-env-atlas tool ---
# Offline tool for generating RGBP env-atlas PNGs from HDR sources.
# Links against the engine library for EnvLighting.

add_executable(generate-env-atlas generate-env-atlas.cpp)

target_link_libraries(generate-env-atlas PRIVATE visutwin-canvas)

# stb headers come from the vcpkg stb package
target_include_directories(generate-env-atlas PRIVATE
    ${VISUTWIN_ROOT_DIR}/engine/src
    ${STB_INCLUDE_DIR}
)
