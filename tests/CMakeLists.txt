add_executable(visutwin-graph-node-tests
    graphNodeTests.cpp
)

target_link_libraries(visutwin-graph-node-tests PRIVATE visutwin-canvas)

add_test(NAME graph-node-invariants COMMAND visutwin-graph-node-tests)

add_executable(visutwin-material-uniforms-tests
    materialUniformsTests.cpp
)

target_link_libraries(visutwin-material-uniforms-tests PRIVATE visutwin-canvas)

add_test(NAME material-uniforms COMMAND visutwin-material-uniforms-tests)

add_executable(visutwin-stencil-parameters-tests
    stencilParametersTests.cpp
)

target_link_libraries(visutwin-stencil-parameters-tests PRIVATE visutwin-canvas)

add_test(NAME stencil-parameters COMMAND visutwin-stencil-parameters-tests)

add_executable(visutwin-blend-state-tests
    blendStateTests.cpp
)

target_link_libraries(visutwin-blend-state-tests PRIVATE visutwin-canvas)

add_test(NAME blend-state COMMAND visutwin-blend-state-tests)

add_executable(visutwin-camera-projection-tests
    cameraProjectionTests.cpp
)

target_link_libraries(visutwin-camera-projection-tests PRIVATE visutwin-canvas)

add_test(NAME camera-projection COMMAND visutwin-camera-projection-tests)

if(VISUTWIN_BACKEND_VULKAN)
    add_executable(visutwin-vulkan-smoke
        vulkanSmoke.cpp
    )

    target_link_libraries(visutwin-vulkan-smoke PRIVATE visutwin-canvas)

    add_test(NAME vulkan-validation-smoke COMMAND visutwin-vulkan-smoke)
    set_tests_properties(vulkan-validation-smoke PROPERTIES
        LABELS "vulkan;smoke"
        TIMEOUT 60
    )
endif()

# Golden images (tools/golden_images.py): renders deterministic examples with this
# build's backend and compares them with tests/golden/<backend>/. It needs a GPU, a
# window and the example binaries, so it exists only when the examples are built and
# carries the `golden` label, which every CI test preset excludes; run it with
# `ctest --preset golden` (Metal, the `examples` build). A Vulkan build of the examples
# runs the same script with --backend vulkan (Release only: a Debug Vulkan build
# crashes in the validation layer, see AGENTS.md).
if(VISUTWIN_BUILD_EXAMPLES)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    # The script needs numpy and Pillow, which the interpreter CMake finds first may
    # not have (Homebrew's often does not); say so at configure time, not as a
    # failing test.
    set(VISUTWIN_GOLDEN_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH
        "Python with numpy and Pillow, for the golden-images test")
    execute_process(COMMAND "${VISUTWIN_GOLDEN_PYTHON}" -c "import numpy, PIL"
        RESULT_VARIABLE _visutwin_golden_python_ok OUTPUT_QUIET ERROR_QUIET)
    if(NOT _visutwin_golden_python_ok EQUAL 0)
        message(WARNING "golden-images: ${VISUTWIN_GOLDEN_PYTHON} lacks numpy or Pillow, so the "
            "test will fail. Point it at one that has them, e.g. -DVISUTWIN_GOLDEN_PYTHON=/usr/bin/python3")
    endif()
    if(VISUTWIN_BACKEND_METAL)
        set(_visutwin_golden_backend metal)
    else()
        set(_visutwin_golden_backend vulkan)
    endif()
    add_test(NAME golden-images
        COMMAND "${VISUTWIN_GOLDEN_PYTHON}" "${CMAKE_SOURCE_DIR}/tools/golden_images.py"
            --examples-dir "${CMAKE_BINARY_DIR}/examples"
            --backend ${_visutwin_golden_backend})
    set_tests_properties(golden-images PROPERTIES
        LABELS "golden"
        SKIP_RETURN_CODE 77
        TIMEOUT 900
    )
endif()

add_executable(visutwin-gsplat-sort-tests
    gsplatSortTests.cpp
)

target_link_libraries(visutwin-gsplat-sort-tests PRIVATE visutwin-canvas)

add_test(NAME gsplat-sort-direction COMMAND visutwin-gsplat-sort-tests)

add_executable(visutwin-physics-world-tests
    physicsWorldTests.cpp
)

target_link_libraries(visutwin-physics-world-tests PRIVATE visutwin-canvas)

add_test(NAME physics-world COMMAND visutwin-physics-world-tests)

add_executable(visutwin-omni-face-axis-tests
    omniFaceAxisTests.cpp
)

target_link_libraries(visutwin-omni-face-axis-tests PRIVATE visutwin-canvas)

add_test(NAME omni-face-axes COMMAND visutwin-omni-face-axis-tests)

add_executable(visutwin-sort-distance-tests
    sortDistanceTests.cpp
)

target_link_libraries(visutwin-sort-distance-tests PRIVATE visutwin-canvas)

add_test(NAME sort-distance COMMAND visutwin-sort-distance-tests)

add_executable(visutwin-pixel-format-tests
    pixelFormatTests.cpp
)

target_link_libraries(visutwin-pixel-format-tests PRIVATE visutwin-canvas)

add_test(NAME pixel-format-table COMMAND visutwin-pixel-format-tests)

add_executable(visutwin-simd-math-tests
    simdMathTests.cpp
)

target_link_libraries(visutwin-simd-math-tests PRIVATE visutwin-canvas)

add_test(NAME simd-math COMMAND visutwin-simd-math-tests)

add_executable(visutwin-entity-lifecycle-tests
    entityLifecycleTests.cpp
)

target_link_libraries(visutwin-entity-lifecycle-tests PRIVATE visutwin-canvas)

add_test(NAME entity-lifecycle COMMAND visutwin-entity-lifecycle-tests)

add_executable(visutwin-component-registry-tests
    componentRegistryTests.cpp
)

target_link_libraries(visutwin-component-registry-tests PRIVATE visutwin-canvas)

add_test(NAME component-registry COMMAND visutwin-component-registry-tests)

add_executable(visutwin-batch-split-tests
    batchSplitTests.cpp
)

target_link_libraries(visutwin-batch-split-tests PRIVATE visutwin-canvas)

add_test(NAME batch-split COMMAND visutwin-batch-split-tests)

add_executable(visutwin-component-active-tests
    componentActiveTests.cpp
)

target_link_libraries(visutwin-component-active-tests PRIVATE visutwin-canvas)

add_test(NAME component-active COMMAND visutwin-component-active-tests)

add_executable(visutwin-shadow-map-invalidation-tests
    shadowMapInvalidationTests.cpp
)

target_link_libraries(visutwin-shadow-map-invalidation-tests PRIVATE visutwin-canvas)

add_test(NAME shadow-map-invalidation COMMAND visutwin-shadow-map-invalidation-tests)

add_executable(visutwin-input-device-tests
    inputDeviceTests.cpp
)

target_link_libraries(visutwin-input-device-tests PRIVATE visutwin-canvas)

add_test(NAME input-devices COMMAND visutwin-input-device-tests)

add_executable(visutwin-gsplat-aabb-tests
    gsplatAabbTests.cpp
)

target_link_libraries(visutwin-gsplat-aabb-tests PRIVATE visutwin-canvas)

add_test(NAME gsplat-aabb COMMAND visutwin-gsplat-aabb-tests)

add_executable(visutwin-light-culling-tests
    lightCullingTests.cpp
)

target_link_libraries(visutwin-light-culling-tests PRIVATE visutwin-canvas)

add_test(NAME light-culling COMMAND visutwin-light-culling-tests)

add_executable(visutwin-sort-key-tests
    sortKeyTests.cpp
)

target_link_libraries(visutwin-sort-key-tests PRIVATE visutwin-canvas)

add_test(NAME sort-key COMMAND visutwin-sort-key-tests)

add_executable(visutwin-light-bounds-tests
    lightBoundsTests.cpp
)

target_link_libraries(visutwin-light-bounds-tests PRIVATE visutwin-canvas)

add_test(NAME light-bounds COMMAND visutwin-light-bounds-tests)

add_executable(visutwin-light-texture-atlas-tests
    lightTextureAtlasTests.cpp
)

target_link_libraries(visutwin-light-texture-atlas-tests PRIVATE visutwin-canvas)

add_test(NAME light-texture-atlas COMMAND visutwin-light-texture-atlas-tests)

add_executable(visutwin-gpu-profiler-timing-tests
    gpuProfilerTimingTests.cpp
)

target_link_libraries(visutwin-gpu-profiler-timing-tests PRIVATE visutwin-canvas)

add_test(NAME gpu-profiler-timing COMMAND visutwin-gpu-profiler-timing-tests)

add_executable(visutwin-device-capability-tests
    deviceCapabilityTests.cpp
)

target_link_libraries(visutwin-device-capability-tests PRIVATE visutwin-canvas)

add_test(NAME device-capabilities COMMAND visutwin-device-capability-tests)

add_executable(visutwin-gsplat-sort-keys-tests
    gsplatSortKeysTests.cpp
)

target_link_libraries(visutwin-gsplat-sort-keys-tests PRIVATE visutwin-canvas)

add_test(NAME gsplat-sort-keys COMMAND visutwin-gsplat-sort-keys-tests)

add_executable(visutwin-lightmapper-bvh-tests
    lightmapperBvhTests.cpp
)

target_link_libraries(visutwin-lightmapper-bvh-tests PRIVATE visutwin-canvas)

add_test(NAME lightmapper-bvh COMMAND visutwin-lightmapper-bvh-tests)

add_executable(visutwin-standard-material-workflow-tests
    standardMaterialWorkflowTests.cpp
)

target_link_libraries(visutwin-standard-material-workflow-tests PRIVATE visutwin-canvas)

add_test(NAME standard-material-workflow COMMAND visutwin-standard-material-workflow-tests)

add_executable(visutwin-wide-line-segment-buffer-tests
    wideLineSegmentBufferTests.cpp
)

target_link_libraries(visutwin-wide-line-segment-buffer-tests PRIVATE visutwin-canvas)

add_test(NAME wide-line-segment-buffer COMMAND visutwin-wide-line-segment-buffer-tests)

add_executable(visutwin-primitive-geometry-tests
    primitiveGeometryTests.cpp
)

target_link_libraries(visutwin-primitive-geometry-tests PRIVATE visutwin-canvas)

add_test(NAME primitive-geometry COMMAND visutwin-primitive-geometry-tests)

# glTF animation binding: unnamed nodes take upstream's node_<index> identity and
# targets are name PATHS, so duplicate names in different branches animate their
# own entity. Builds a tinygltf model in memory, so it needs tinygltf's header.
# Animation layer blending: a layer weight is a per-node contribution composed across
# layers (OVERWRITE / ADDITIVE, masks, normalised weights), not an on/off switch.
add_executable(visutwin-anim-layer-blend-tests
    animLayerBlendTests.cpp
)

target_link_libraries(visutwin-anim-layer-blend-tests PRIVATE visutwin-canvas)

add_test(NAME anim-layer-blend COMMAND visutwin-anim-layer-blend-tests)

add_executable(visutwin-glb-animation-binding-tests
    glbAnimationBindingTests.cpp
)

target_include_directories(visutwin-glb-animation-binding-tests PRIVATE ${TINYGLTF_INCLUDE_DIRS})
target_link_libraries(visutwin-glb-animation-binding-tests PRIVATE visutwin-canvas)

add_test(NAME glb-animation-binding COMMAND visutwin-glb-animation-binding-tests)

add_executable(visutwin-glb-point-node-matrix-tests
    glbPointNodeMatrixTests.cpp
)

target_include_directories(visutwin-glb-point-node-matrix-tests PRIVATE ${TINYGLTF_INCLUDE_DIRS})
target_link_libraries(visutwin-glb-point-node-matrix-tests PRIVATE visutwin-canvas)

add_test(NAME glb-point-node-matrix COMMAND visutwin-glb-point-node-matrix-tests)

add_executable(visutwin-stb-image-flip-tests
    stbImageFlipTests.cpp
)

target_link_libraries(visutwin-stb-image-flip-tests PRIVATE visutwin-canvas)

add_test(NAME stb-image-flip COMMAND visutwin-stb-image-flip-tests)

add_executable(visutwin-texture-gpu-size-tests
    textureGpuSizeTests.cpp
)

target_link_libraries(visutwin-texture-gpu-size-tests PRIVATE visutwin-canvas)

add_test(NAME texture-gpu-size COMMAND visutwin-texture-gpu-size-tests)

add_executable(visutwin-morph-bounds-tests
    morphBoundsTests.cpp
)

target_include_directories(visutwin-morph-bounds-tests PRIVATE ${TINYGLTF_INCLUDE_DIRS})
target_link_libraries(visutwin-morph-bounds-tests PRIVATE visutwin-canvas)

add_test(NAME morph-bounds COMMAND visutwin-morph-bounds-tests)

add_executable(visutwin-joint-lifetime-tests
    jointLifetimeTests.cpp
)

target_link_libraries(visutwin-joint-lifetime-tests PRIVATE visutwin-canvas)

add_test(NAME joint-lifetime COMMAND visutwin-joint-lifetime-tests)

add_executable(visutwin-batch-lifetime-tests
    batchLifetimeTests.cpp
)

target_link_libraries(visutwin-batch-lifetime-tests PRIVATE visutwin-canvas)

add_test(NAME batch-lifetime COMMAND visutwin-batch-lifetime-tests)

add_executable(visutwin-glb-material-paths-tests
    glbMaterialPathsTests.cpp
)

target_include_directories(visutwin-glb-material-paths-tests PRIVATE ${TINYGLTF_INCLUDE_DIRS})
target_link_libraries(visutwin-glb-material-paths-tests PRIVATE visutwin-canvas)

add_test(NAME glb-material-paths COMMAND visutwin-glb-material-paths-tests)

add_executable(visutwin-script-lifetime-tests
    scriptLifetimeTests.cpp
)

target_link_libraries(visutwin-script-lifetime-tests PRIVATE visutwin-canvas)

add_test(NAME script-lifetime COMMAND visutwin-script-lifetime-tests)

add_executable(visutwin-picker-tests
    pickerTests.cpp
)

target_link_libraries(visutwin-picker-tests PRIVATE visutwin-canvas)

add_test(NAME picker COMMAND visutwin-picker-tests)
