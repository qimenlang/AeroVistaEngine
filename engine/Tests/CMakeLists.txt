add_subdirectory(UnitTests)

if(vsgXchange_FOUND)
    add_subdirectory(RenderringTests)
else()
    message(STATUS "Skipping RenderringTests: vsgXchange not found (PNG I/O required)")
endif()
