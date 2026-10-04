# visutwin_add_unit_test(<target> <test name> <source>
#     [TINYGLTF] [DEFINITIONS <definition>...] [LABELS <label>...] [TIMEOUT <seconds>])
#
# One test executable linked against the engine, registered as one ctest test. TINYGLTF
# adds tinygltf's include directory, for a test that builds a glTF model in memory.
function(visutwin_add_unit_test target testName source)
    cmake_parse_arguments(PARSE_ARGV 3 ARG "TINYGLTF" "TIMEOUT" "DEFINITIONS;LABELS")
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE visutwin-canvas)
    if(ARG_TINYGLTF)
        target_include_directories(${target} PRIVATE ${TINYGLTF_INCLUDE_DIRS})
    endif()
    if(ARG_DEFINITIONS)
        target_compile_definitions(${target} PRIVATE ${ARG_DEFINITIONS})
    endif()
    add_test(NAME ${testName} COMMAND ${target})
    if(ARG_LABELS)
        set_tests_properties(${testName} PROPERTIES LABELS "${ARG_LABELS}")
    endif()
    if(ARG_TIMEOUT)
        set_tests_properties(${testName} PROPERTIES TIMEOUT ${ARG_TIMEOUT})
    endif()
endfunction()

visutwin_add_unit_test(visutwin-graph-node-tests graph-node-invariants graphNodeTests.cpp)
visutwin_add_unit_test(visutwin-material-uniforms-tests material-uniforms materialUniformsTests.cpp)
visutwin_add_unit_test(visutwin-stencil-parameters-tests stencil-parameters stencilParametersTests.cpp)
visutwin_add_unit_test(visutwin-blend-state-tests blend-state blendStateTests.cpp)
visutwin_add_unit_test(visutwin-camera-projection-tests camera-projection cameraProjectionTests.cpp)

# GPU instance culling on a real device of this build's backend (tests/gpuInstanceCullTests.cpp).
# It needs a GPU and a window, so it carries the `gpu` label, which the CI test presets
# exclude; run it with `ctest --preset gpu`, or `ctest -L gpu` in a Vulkan build.
visutwin_add_unit_test(visutwin-gpu-instance-cull-tests gpu-instance-cull gpuInstanceCullTests.cpp LABELS gpu TIMEOUT 60)
# The orientation of an env atlas baked from an equirect, read back from the real device
# (tests/envAtlasOrientationTests.cpp); `gpu` label for the same reason.
visutwin_add_unit_test(visutwin-env-atlas-orientation-tests env-atlas-orientation envAtlasOrientationTests.cpp
    LABELS gpu TIMEOUT 60)

if(VISUTWIN_BACKEND_VULKAN)
    visutwin_add_unit_test(visutwin-vulkan-smoke vulkan-validation-smoke vulkanSmoke.cpp
        LABELS vulkan smoke TIMEOUT 60)
endif()

# Golden images (tools/golden_images.py): renders deterministic examples with this
# build's backend and compares them with tests/golden/<backend>/<ratio>x/, the set for the
# back buffer's pixel ratio. It needs a GPU, a
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

visutwin_add_unit_test(visutwin-gsplat-sort-tests gsplat-sort-direction gsplatSortTests.cpp)
visutwin_add_unit_test(visutwin-physics-world-tests physics-world physicsWorldTests.cpp)
visutwin_add_unit_test(visutwin-omni-face-axis-tests omni-face-axes omniFaceAxisTests.cpp)
visutwin_add_unit_test(visutwin-sort-distance-tests sort-distance sortDistanceTests.cpp)
visutwin_add_unit_test(visutwin-pixel-format-tests pixel-format-table pixelFormatTests.cpp)
visutwin_add_unit_test(visutwin-texture-sampler-state-tests texture-sampler-state textureSamplerStateTests.cpp)
visutwin_add_unit_test(visutwin-simd-math-tests simd-math simdMathTests.cpp)
visutwin_add_unit_test(visutwin-entity-lifecycle-tests entity-lifecycle entityLifecycleTests.cpp)
visutwin_add_unit_test(visutwin-component-registry-tests component-registry componentRegistryTests.cpp)
visutwin_add_unit_test(visutwin-component-instance-list-tests component-instance-list componentInstanceListTests.cpp)
visutwin_add_unit_test(visutwin-primitive-mesh-sharing-tests primitive-mesh-sharing primitiveMeshSharingTests.cpp)
visutwin_add_unit_test(visutwin-local-shadow-caster-tests local-shadow-casters localShadowCasterTests.cpp)
add_test(NAME local-shadow-casters-non-clustered COMMAND visutwin-local-shadow-caster-tests --non-clustered)

visutwin_add_unit_test(visutwin-palette-sharing-tests palette-sharing paletteSharingTests.cpp)
visutwin_add_unit_test(visutwin-shader-disk-cache-tests shader-disk-cache shaderDiskCacheTests.cpp)
visutwin_add_unit_test(visutwin-forward-shader-memo-tests forward-shader-memo forwardShaderMemoTests.cpp)
visutwin_add_unit_test(visutwin-glb-image-decode-tests glb-image-decode glbImageDecodeTests.cpp)
visutwin_add_unit_test(visutwin-batch-split-tests batch-split batchSplitTests.cpp)
visutwin_add_unit_test(visutwin-component-active-tests component-active componentActiveTests.cpp)
visutwin_add_unit_test(visutwin-shadow-map-invalidation-tests shadow-map-invalidation shadowMapInvalidationTests.cpp)
visutwin_add_unit_test(visutwin-input-device-tests input-devices inputDeviceTests.cpp)
visutwin_add_unit_test(visutwin-gsplat-aabb-tests gsplat-aabb gsplatAabbTests.cpp)
visutwin_add_unit_test(visutwin-light-culling-tests light-culling lightCullingTests.cpp)
visutwin_add_unit_test(visutwin-sort-key-tests sort-key sortKeyTests.cpp)
visutwin_add_unit_test(visutwin-light-bounds-tests light-bounds lightBoundsTests.cpp)
visutwin_add_unit_test(visutwin-light-texture-atlas-tests light-texture-atlas lightTextureAtlasTests.cpp)
visutwin_add_unit_test(visutwin-clustered-atlas-visibility-tests clustered-atlas-visibility clusteredAtlasVisibilityTests.cpp)
visutwin_add_unit_test(visutwin-gpu-profiler-timing-tests gpu-profiler-timing gpuProfilerTimingTests.cpp)
visutwin_add_unit_test(visutwin-device-capability-tests device-capabilities deviceCapabilityTests.cpp)
visutwin_add_unit_test(visutwin-gsplat-sort-keys-tests gsplat-sort-keys gsplatSortKeysTests.cpp)
visutwin_add_unit_test(visutwin-lightmapper-bvh-tests lightmapper-bvh lightmapperBvhTests.cpp)
visutwin_add_unit_test(visutwin-lightmap-filters-tests lightmap-filters lightmapFiltersTests.cpp)
visutwin_add_unit_test(visutwin-light-radiance-tests light-radiance lightRadianceTests.cpp)
visutwin_add_unit_test(visutwin-standard-material-workflow-tests standard-material-workflow standardMaterialWorkflowTests.cpp)
visutwin_add_unit_test(visutwin-wide-line-segment-buffer-tests wide-line-segment-buffer wideLineSegmentBufferTests.cpp)
visutwin_add_unit_test(visutwin-primitive-geometry-tests primitive-geometry primitiveGeometryTests.cpp)

# Animation layer blending: a layer weight is a per-node contribution composed across
# layers (OVERWRITE / ADDITIVE, masks, normalised weights), not an on/off switch.
visutwin_add_unit_test(visutwin-anim-layer-blend-tests anim-layer-blend animLayerBlendTests.cpp)
# glTF animation binding: unnamed nodes take a node_<index> identity and
# targets are name PATHS, so duplicate names in different branches animate their
# own entity. Builds a tinygltf model in memory, so it needs tinygltf's header.
visutwin_add_unit_test(visutwin-glb-animation-binding-tests glb-animation-binding glbAnimationBindingTests.cpp TINYGLTF)
visutwin_add_unit_test(visutwin-glb-point-node-matrix-tests glb-point-node-matrix glbPointNodeMatrixTests.cpp TINYGLTF)
visutwin_add_unit_test(visutwin-stb-image-flip-tests stb-image-flip stbImageFlipTests.cpp)
visutwin_add_unit_test(visutwin-texture-gpu-size-tests texture-gpu-size textureGpuSizeTests.cpp)
visutwin_add_unit_test(visutwin-morph-bounds-tests morph-bounds morphBoundsTests.cpp TINYGLTF)
visutwin_add_unit_test(visutwin-joint-lifetime-tests joint-lifetime jointLifetimeTests.cpp)
visutwin_add_unit_test(visutwin-batch-lifetime-tests batch-lifetime batchLifetimeTests.cpp)
visutwin_add_unit_test(visutwin-batch-mirror-tests batch-mirror batchMirrorTests.cpp)
visutwin_add_unit_test(visutwin-glb-material-paths-tests glb-material-paths glbMaterialPathsTests.cpp TINYGLTF)
visutwin_add_unit_test(visutwin-glb-point-cloud-tests glb-point-cloud glbPointCloudTests.cpp TINYGLTF)
visutwin_add_unit_test(visutwin-raycast-fallback-tests raycast-fallback raycastFallbackTests.cpp)
visutwin_add_unit_test(visutwin-mirrored-body-tests mirrored-body mirroredBodyTests.cpp)
visutwin_add_unit_test(visutwin-rigid-body-state-tests rigid-body-state rigidBodyStateTests.cpp)
visutwin_add_unit_test(visutwin-entity-clone-tests entity-clone entityCloneTests.cpp)
visutwin_add_unit_test(visutwin-glb-camera-light-tests glb-camera-light glbCameraLightTests.cpp TINYGLTF)
visutwin_add_unit_test(visutwin-glb-extensions-tests glb-extensions glbExtensionsTests.cpp TINYGLTF)
visutwin_add_unit_test(visutwin-glb-vertex-attribute-tests glb-vertex-attributes glbVertexAttributeTests.cpp TINYGLTF)
visutwin_add_unit_test(visutwin-triage-contracts-tests triage-contracts triageContractsTests.cpp TINYGLTF)
visutwin_add_unit_test(visutwin-frame-graph-tests frame-graph frameGraphTests.cpp)
visutwin_add_unit_test(visutwin-shader-composition-tests shader-composition shaderCompositionTests.cpp)
visutwin_add_unit_test(visutwin-camera-frame-stop-tests camera-frame-stop cameraFrameStopTests.cpp)
visutwin_add_unit_test(visutwin-cluster-grid-sharing-tests cluster-grid-sharing clusterGridSharingTests.cpp)
visutwin_add_unit_test(visutwin-ktx2-target-tests ktx2-target ktx2TargetTests.cpp TINYGLTF DEFINITIONS VISUTWIN_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
visutwin_add_unit_test(visutwin-obj-stl-round-trip-tests obj-stl-round-trip objStlRoundTripTests.cpp)
visutwin_add_unit_test(visutwin-frame-stats-tests frame-stats frameStatsTests.cpp)
visutwin_add_unit_test(visutwin-curve-tests curve curveTests.cpp)
visutwin_add_unit_test(visutwin-element-layout-tests element-layout elementLayoutTests.cpp)
visutwin_add_unit_test(visutwin-element-input-tests element-input elementInputTests.cpp)
visutwin_add_unit_test(visutwin-image-element-geometry-tests image-element-geometry imageElementGeometryTests.cpp)
visutwin_add_unit_test(visutwin-msdf-text-tests msdf-text msdfTextTests.cpp)
visutwin_add_unit_test(visutwin-markup-tests markup markupTests.cpp)
visutwin_add_unit_test(visutwin-script-lifetime-tests script-lifetime scriptLifetimeTests.cpp)
visutwin_add_unit_test(visutwin-annotation-tests annotation annotationTests.cpp)
visutwin_add_unit_test(visutwin-script-phase-tests script-phases scriptPhaseTests.cpp)
visutwin_add_unit_test(visutwin-ui-visual-sharing-tests ui-visual-sharing uiVisualSharingTests.cpp)
visutwin_add_unit_test(visutwin-render2d-tests render2d render2dTests.cpp)
visutwin_add_unit_test(visutwin-mini-stats-text-tests mini-stats-text miniStatsTextTests.cpp)
visutwin_add_unit_test(visutwin-picker-tests picker pickerTests.cpp)
visutwin_add_unit_test(visutwin-particle-color-graph-tests particle-color-graph particleColorGraphTests.cpp)
visutwin_add_unit_test(visutwin-upstream-small-ports-tests upstream-small-ports upstreamSmallPortsTests.cpp)
visutwin_add_unit_test(visutwin-layout-calculator-tests layout-calculator layoutCalculatorTests.cpp)
visutwin_add_unit_test(visutwin-ui-layout-scroll-tests ui-layout-scroll uiLayoutScrollTests.cpp)
visutwin_add_unit_test(visutwin-text-fit-tests text-fit textFitTests.cpp DEFINITIONS VISUTWIN_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
visutwin_add_unit_test(visutwin-i18n-tests i18n i18nTests.cpp DEFINITIONS VISUTWIN_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")
visutwin_add_unit_test(visutwin-gizmo-tests gizmo gizmoTests.cpp)
visutwin_add_unit_test(visutwin-event-handler-off-tests event-handler-off eventHandlerOffTests.cpp)
visutwin_add_unit_test(visutwin-engine-update-tests engine-update engineUpdateTests.cpp)
visutwin_add_unit_test(visutwin-texture-outlives-device-tests texture-outlives-device textureOutlivesDeviceTests.cpp)
visutwin_add_unit_test(visutwin-resources-outlive-device-tests resources-outlive-device resourcesOutliveDeviceTests.cpp)
