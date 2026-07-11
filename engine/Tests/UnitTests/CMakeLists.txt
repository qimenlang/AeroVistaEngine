set(TARGET_NAME vsgEngineTests)

add_executable(${TARGET_NAME} testExample.cpp)

target_link_libraries(${TARGET_NAME} PRIVATE Catch2::Catch2WithMain)