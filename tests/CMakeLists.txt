add_executable(visutwin-graph-node-tests
    graphNodeTests.cpp
)

target_link_libraries(visutwin-graph-node-tests PRIVATE visutwin-canvas)

add_test(NAME graph-node-invariants COMMAND visutwin-graph-node-tests)

add_executable(visutwin-stencil-parameters-tests
    stencilParametersTests.cpp
)

target_link_libraries(visutwin-stencil-parameters-tests PRIVATE visutwin-canvas)

add_test(NAME stencil-parameters COMMAND visutwin-stencil-parameters-tests)
