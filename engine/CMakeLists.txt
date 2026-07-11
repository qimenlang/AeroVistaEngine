set(TARGET_NAME vsgEngine)

set(RESOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources")

add_executable(${TARGET_NAME} source/main.cpp)

add_subdirectory(Tests)