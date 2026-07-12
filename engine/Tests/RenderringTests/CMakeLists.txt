set(SOURCES
    ImageCompare.cpp
    RenderingEngine.cpp
    RenderingTests.cpp
)

add_executable(RenderingTests ${SOURCES})

target_compile_definitions(RenderingTests PRIVATE "RESOURCE_DIR=\"${RESOURCE_DIR}\"")

target_link_libraries(RenderingTests vsg::vsg Catch2::Catch2WithMain)

if (vsgXchange_FOUND)
    target_compile_definitions(RenderingTests PRIVATE vsgXchange_FOUND)
    target_link_libraries(RenderingTests vsgXchange::vsgXchange)
endif()

install(TARGETS RenderingTests RUNTIME DESTINATION bin)

#add_test(NAME RenderingTests COMMAND RenderingTests)