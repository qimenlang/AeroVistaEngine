cmake_minimum_required(VERSION 4.2)
project(AeroVistaEngine)

# Clang + Ninja on Windows does not predefine WIN32 (MSVC does).
if(WIN32)
    add_compile_definitions(WIN32)
endif()

add_subdirectory(engine)
add_subdirectory(thirdparty)