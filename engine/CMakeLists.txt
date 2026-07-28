set(TARGET_NAME vsgEngine)

set(RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

# Run clang-tidy on engine targets during build (uses repo-root .clang-tidy).
# Does not apply to thirdparty/ (configured before this directory).
find_program(AEROVISTA_CLANG_TIDY NAMES clang-tidy clang-tidy.exe)
if(AEROVISTA_CLANG_TIDY)
    set(CMAKE_CXX_CLANG_TIDY "${AEROVISTA_CLANG_TIDY}")
    message(STATUS "Engine clang-tidy: ${AEROVISTA_CLANG_TIDY}")
else()
    message(STATUS "Engine clang-tidy: not found (build continues without tidy)")
endif()

add_library(vsgEngineLib STATIC
    source/engine.cpp
    source/function/config/EngineConfig.cpp
    source/function/sync/Network.cpp
    source/function/sync/SynchronSystem.cpp
    source/function/sync/HostSync.cpp
    source/function/sync/IgSync.cpp
    source/function/sync/EventProcess.cpp
)

target_include_directories(vsgEngineLib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source)

target_compile_definitions(vsgEngineLib PUBLIC "RESOURCE_DIR=\"${RESOURCE_DIR}\"")

target_link_libraries(vsgEngineLib
    PUBLIC
        vsg::vsg
        vsgXchange::vsgXchange
        ws2_32
        cigicl-static
)

add_executable(${TARGET_NAME}
    source/main.cpp
)

target_link_libraries(${TARGET_NAME}
    PRIVATE
        vsgEngineLib
)

if(vsgXchange_FOUND)
    add_subdirectory(Tests)
else()
    message(STATUS "Skipping Tests: vsgXchange not found (PNG I/O required)")
endif()
