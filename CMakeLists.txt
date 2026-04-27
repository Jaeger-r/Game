set(JAEGER_GAMESERVER_INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

add_library(JaegerServerNet STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/server2/INet.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/server2/kcpnet.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/server2/kcpnet.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/server2/tcpnet.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/server2/tcpnet.h"
)
target_include_directories(JaegerServerNet
    PUBLIC
        ${JAEGER_GAMESERVER_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/server2"
)
target_link_libraries(JaegerServerNet
    PUBLIC
        Qt6::Core
        Qt6::Network
        JaegerShared
        JaegerQtKcp
        JaegerPostgreSQL
)
jaeger_apply_target_defaults(JaegerServerNet)

add_library(JaegerServerStorage STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/mysql/CMySql.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/mysql/CMySql.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/mysql/mysqlconnpool.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/mysql/mysqlconnpool.h"
)
target_include_directories(JaegerServerStorage
    PUBLIC
        ${JAEGER_GAMESERVER_INCLUDE_DIRS}
        "${CMAKE_CURRENT_SOURCE_DIR}/mysql"
)
target_link_libraries(JaegerServerStorage
    PUBLIC
        Qt6::Core
        Qt6::Sql
        JaegerPostgreSQL
)
jaeger_apply_target_defaults(JaegerServerStorage)

add_library(JaegerServerCore STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/serverconfig.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/serverupdatemanager.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/servermonitorbridge.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/Icore.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/combatbalance.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/core.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/packdef.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/packetbuilder.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/serverconfig.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/serverupdatemanager.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/servermonitorbridge.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/servermonitortypes.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/tou.h"
)
target_include_directories(JaegerServerCore
    PUBLIC
        ${JAEGER_GAMESERVER_INCLUDE_DIRS}
)
target_link_libraries(JaegerServerCore
    PUBLIC
        Qt6::Core
        Qt6::Network
        Qt6::Sql
        JaegerShared
        JaegerServerNet
        JaegerServerStorage
        JaegerTinyXML2
)
jaeger_apply_target_defaults(JaegerServerCore)

add_executable(GameServer MACOSX_BUNDLE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/servermonitorwindow.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/servermonitorwindow.h"
)
target_include_directories(GameServer PRIVATE ${JAEGER_GAMESERVER_INCLUDE_DIRS})
target_link_libraries(GameServer PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    JaegerServerCore
)
jaeger_apply_target_defaults(GameServer)
jaeger_enable_agl_compat(GameServer)

add_executable(GameServerHeadless
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)
target_compile_definitions(GameServerHeadless PRIVATE JAEGER_HEADLESS_BUILD)
target_include_directories(GameServerHeadless PRIVATE ${JAEGER_GAMESERVER_INCLUDE_DIRS})
target_link_libraries(GameServerHeadless PRIVATE
    Qt6::Core
    JaegerServerCore
)
jaeger_apply_target_defaults(GameServerHeadless)
