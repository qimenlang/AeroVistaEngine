set(TARGET_NAME vsgEngineTests)

set(SOURCES
    ImageCompare.cpp
    RenderingTests.cpp
)

add_executable(${TARGET_NAME} ${SOURCES})

target_compile_definitions(${TARGET_NAME} PRIVATE "RESOURCE_DIR=\"${RESOURCE_DIR}\"")

target_link_libraries(${TARGET_NAME}
    PRIVATE
        vsgEngineLib
        Catch2::Catch2WithMain
)

install(TARGETS ${TARGET_NAME} RUNTIME DESTINATION bin)

add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
