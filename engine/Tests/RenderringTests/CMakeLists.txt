set(SOURCES
    ImageCompare.cpp
    RenderingTests.cpp
)

add_executable(RenderingTests ${SOURCES})

target_compile_definitions(RenderingTests PRIVATE "RESOURCE_DIR=\"${RESOURCE_DIR}\"")

target_link_libraries(RenderingTests
    PRIVATE
        vsgEngineLib
        Catch2::Catch2WithMain
)

install(TARGETS RenderingTests RUNTIME DESTINATION bin)

# add_test(NAME RenderingTests COMMAND RenderingTests)
