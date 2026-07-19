set(TARGET_NAME vsgEngine)

set(RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

add_library(vsgEngineLib STATIC
    source/engine.cpp
)

target_include_directories(vsgEngineLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source)

target_compile_definitions(vsgEngineLib PRIVATE
    "VSG_EXAMPLES_DATA_DIR=\"${CMAKE_SOURCE_DIR}/thirdparty/vsgExamples/data\""
)

target_link_libraries(vsgEngineLib
    PUBLIC
        vsg::vsg
        vsgXchange::vsgXchange
)

add_executable(${TARGET_NAME}
    source/main.cpp
)

target_compile_definitions(${TARGET_NAME} PRIVATE "RESOURCE_DIR=\"${RESOURCE_DIR}\"")

target_link_libraries(${TARGET_NAME}
    PRIVATE
        vsgEngineLib
)

add_subdirectory(Tests)
