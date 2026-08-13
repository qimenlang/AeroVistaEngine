# sync 多通道同步模块 — 独立库。
# 依赖方向单向：vsgEngineLib → aerovistaSync；库不得反向依赖引擎。
# 分层：传输层零 vsg；门面层（SynchronSystem）依赖 vsg，不依赖 Engine。
# 见 doc/design/sync模块化设计.md。

add_library(aerovistaSync STATIC
    SyncJson.h
    SyncConfig.cpp
    UdpSocket.cpp
    CigiWire.cpp
    EventProcess.cpp
    HostSync.cpp
    IgSync.cpp
    SynchronSystem.cpp
)

target_include_directories(aerovistaSync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(aerovistaSync
    PUBLIC
        vsg::vsg
        cigicl-static
        ws2_32
)

# 可选：最小接入示例（默认关闭）。供其他 vsg 项目参考接入契约。
option(AEROVISTA_BUILD_SYNC_EXAMPLE "Build aerovistaSync minimal integration example" OFF)
if(AEROVISTA_BUILD_SYNC_EXAMPLE)
    add_executable(aerovistaSyncExample
        ${CMAKE_SOURCE_DIR}/engine/examples/minimal_sync/main.cpp
    )
    target_link_libraries(aerovistaSyncExample PRIVATE aerovistaSync)
endif()
