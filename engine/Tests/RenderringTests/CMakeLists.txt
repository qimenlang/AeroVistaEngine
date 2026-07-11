set(SOURCES
    RenderingTests.cpp
)

add_executable(RenderingTests ${SOURCES})

target_link_libraries(RenderingTests vsg::vsg)

if (vsgXchange_FOUND)
    target_compile_definitions(RenderingTests PRIVATE vsgXchange_FOUND)
    target_link_libraries(RenderingTests vsgXchange::vsgXchange)
endif()

install(TARGETS RenderingTests RUNTIME DESTINATION bin)
