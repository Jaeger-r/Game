QT += core network sql

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

include(../Shared/JaegerShared.pri)
include(../ThirdParty/QtKcp/QtKcp.pri)

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
        core.cpp \
        main.cpp \
        serverconfig.cpp \
        serverupdatemanager.cpp \
        servermonitorbridge.cpp

!contains(DEFINES, JAEGER_HEADLESS_BUILD) {
    SOURCES += \
        servermonitorwindow.cpp
}
# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target

include(./server2/server2.pri)
include(./mysql/mysql.pri)

HEADERS += \
    Icore.h \
    core.h \
    packdef.h \
    packetbuilder.h \
    serverconfig.h \
    serverupdatemanager.h \
    servermonitorbridge.h \
    servermonitortypes.h \
    tou.h

!contains(DEFINES, JAEGER_HEADLESS_BUILD) {
    HEADERS += \
        servermonitorwindow.h
}

TINYXML2_ROOT = $$(TINYXML2_ROOT)

isEmpty(TINYXML2_ROOT) {
    macx: TINYXML2_ROOT = /opt/homebrew/Cellar/tinyxml2/11.0.0
}

!isEmpty(TINYXML2_ROOT) {
    INCLUDEPATH += $$TINYXML2_ROOT/include
    LIBS += -L$$TINYXML2_ROOT/lib -ltinyxml2
    macx: QMAKE_RPATHDIR += $$TINYXML2_ROOT/lib
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
