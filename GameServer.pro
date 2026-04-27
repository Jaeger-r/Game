QT += core network sql

equals($$clean_path($$PWD), $$clean_path($$OUT_PWD)) {
    error(GameServer does not support in-source builds. Remove generated files from $$PWD and use a shadow build directory such as $$PWD/build/Qt_6_9_1_for_macOS-Debug.)
}

contains(CONFIG, JAEGER_HEADLESS_BUILD) {
    DEFINES += JAEGER_HEADLESS_BUILD
    QT -= gui widgets
    CONFIG += console
    CONFIG -= app_bundle
}

!contains(DEFINES, JAEGER_HEADLESS_BUILD) {
    QT += widgets
}

CONFIG += c++17
CONFIG += jaeger_force_shared_sources qtkcp_force_shared_sources

OBJECTS_DIR = $$OUT_PWD/obj
MOC_DIR = $$OUT_PWD/moc
UI_DIR = $$OUT_PWD/ui
RCC_DIR = $$OUT_PWD/rcc

QMAKE_CLEAN += \
    $$PWD/Makefile \
    $$PWD/moc_predefs.h \
    $$files($$PWD/*.o) \
    $$files($$PWD/moc_*.cpp) \
    $$files($$PWD/moc_*.o) \
    $$files($$PWD/ui_*.h) \
    $$files($$PWD/qrc_*.cpp)

include(../Shared/JaegerShared.pri)
include(../ThirdParty/QtKcp/QtKcp.pri)

INCLUDEPATH += $$PWD/include

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
        src/core.cpp \
        src/main.cpp \
        src/serverconfig.cpp \
        src/serverupdatemanager.cpp \
        src/servermonitorbridge.cpp

!contains(DEFINES, JAEGER_HEADLESS_BUILD) {
    SOURCES += \
        src/servermonitorwindow.cpp
}
# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

include(./server2/server2.pri)
include(./mysql/mysql.pri)

HEADERS += \
    include/Icore.h \
    include/combatbalance.h \
    include/core.h \
    include/packdef.h \
    include/packetbuilder.h \
    include/serverconfig.h \
    include/serverupdatemanager.h \
    include/servermonitorbridge.h \
    include/servermonitortypes.h \
    include/tou.h

!contains(DEFINES, JAEGER_HEADLESS_BUILD) {
    HEADERS += \
        include/servermonitorwindow.h
}

TINYXML2_ROOT = $$(TINYXML2_ROOT)
TINYXML2_INCLUDE_DIR = $$(TINYXML2_INCLUDE_DIR)
TINYXML2_LIB_DIR = $$(TINYXML2_LIB_DIR)

isEmpty(TINYXML2_ROOT) {
    macx:exists(/opt/homebrew/opt/tinyxml2): TINYXML2_ROOT = /opt/homebrew/opt/tinyxml2
}

isEmpty(TINYXML2_INCLUDE_DIR):!isEmpty(TINYXML2_ROOT) {
    TINYXML2_INCLUDE_DIR = $$TINYXML2_ROOT/include
}

isEmpty(TINYXML2_LIB_DIR):!isEmpty(TINYXML2_ROOT) {
    TINYXML2_LIB_DIR = $$TINYXML2_ROOT/lib
}

exists($$TINYXML2_INCLUDE_DIR/tinyxml2.h) {
    INCLUDEPATH += $$TINYXML2_INCLUDE_DIR
}

!isEmpty(TINYXML2_LIB_DIR) {
    LIBS += -L$$TINYXML2_LIB_DIR -ltinyxml2
    macx: QMAKE_RPATHDIR += $$TINYXML2_LIB_DIR
} else {
    LIBS += -ltinyxml2
}

macx {
    AGL_COMPAT_ROOT_ABS = $$clean_path($$PWD/../macos_frameworks)
    AGL_COMPAT_SCRIPT_ABS = $$clean_path($$PWD/../scripts/build_agl_compat.sh)
    AGL_COMPAT_ALIAS_ROOT = /tmp/codex_agl_frameworks
    exists($$AGL_COMPAT_SCRIPT_ABS):exists($$AGL_COMPAT_ROOT_ABS/empty_agl.c) {
        QMAKE_FRAMEWORKPATH += $$AGL_COMPAT_ALIAS_ROOT
        QMAKE_PRE_LINK += $$shell_quote($$AGL_COMPAT_SCRIPT_ABS) $$shell_quote($$AGL_COMPAT_ROOT_ABS) $$shell_quote($$AGL_COMPAT_ALIAS_ROOT) $$escape_expand(\\n\\t)
    }

    CLT_STDCPP_PATH = /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1
    exists($$CLT_STDCPP_PATH/type_traits) {
        INCLUDEPATH += $$CLT_STDCPP_PATH
    }

    contains(QMAKE_HOST.arch, arm64)|contains(QT_ARCH, arm64) {
        QMAKE_CXXFLAGS += -include arm_acle.h
    }

    LIBS -= -framework AGL
    LIBS -= -framework OpenGL
}

DISTFILES += \
    deploy/jaeger-gameserver.service \
    data/update_config.json \
    data/version.json \
    server.env.example \
    server.json.example
