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
