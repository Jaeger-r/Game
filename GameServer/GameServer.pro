QT += widgets core gui
QT += network
QT += sql
QT += core

CONFIG += c++17

# You can make your code fail to compile if it uses deprecated APIs.
# In order to do so, uncomment the following line.
#DEFINES += QT_DISABLE_DEPRECATED_BEFORE=0x060000    # disables all the APIs deprecated before Qt 6.0.0

SOURCES += \
        core.cpp \
        main.cpp \
        QtKcp/src/qkcpserver.cpp \
        QtKcp/src/qkcpsocket.cpp \
        QtKcp/3rdparty/kcp/ikcp.c
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
    tou.h \
    QtKcp/src/qkcpserver.h \
    QtKcp/src/qkcpsocket.h \
    QtKcp/src/qkcpsocket_global.h \
    QtKcp/src/qkcpsocket_p.h \
    QtKcp/3rdparty/kcp/ikcp.h

INCLUDEPATH += $$PWD/QtKcp/src \
               $$PWD/QtKcp/3rdparty/kcp
