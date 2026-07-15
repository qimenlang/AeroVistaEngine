set(TARGET_NAME vsgEngine)

set(RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

add_executable(${TARGET_NAME}
    source/main.cpp
    source/engine.cpp
)

target_include_directories(${TARGET_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/source)

target_compile_definitions(${TARGET_NAME} PRIVATE "RESOURCE_DIR=\"${RESOURCE_DIR}\"")

target_link_libraries(${TARGET_NAME}
    PRIVATE
        vsg::vsg
        vsgXchange::vsgXchange
)

add_subdirectory(Tests)