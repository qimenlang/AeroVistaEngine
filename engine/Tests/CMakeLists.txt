set(TARGET_NAME vsgEngineTests)

set(SOURCES
    ImageCompare.cpp
    RenderingTests.cpp
    HostIGTests.cpp
    SyncClockTests.cpp
    SyncCommandTests.cpp
    EngineConfigTests.cpp
    ModelConfigTests.cpp
    ViewHostMathTests.cpp
    # viewhost 纯 C++ 数值逻辑（测试与示例共用同一份源码，见 viewhost设计.md §6）
    ${CMAKE_SOURCE_DIR}/thirdparty/sync/examples/viewhost/src/ViewHostMath.cpp
)

add_executable(${TARGET_NAME} ${SOURCES})

target_compile_definitions(${TARGET_NAME} PRIVATE "RESOURCE_DIR=\"${RESOURCE_DIR}\"")

target_include_directories(${TARGET_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/thirdparty/sync/examples/viewhost/src
)

target_link_libraries(${TARGET_NAME}
    PRIVATE
        vsgEngineLib
        Catch2::Catch2WithMain
)

install(TARGETS ${TARGET_NAME} RUNTIME DESTINATION bin)

add_test(NAME ${TARGET_NAME} COMMAND ${TARGET_NAME})
