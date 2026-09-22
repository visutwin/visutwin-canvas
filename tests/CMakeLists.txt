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
