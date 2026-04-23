HEADERS += \
    $$PWD/CMySql.h \
    $$PWD/mysqlconnpool.h

SOURCES += \
    $$PWD/CMySql.cpp \
    $$PWD/mysqlconnpool.cpp

MYSQL_ROOT = $$(MYSQL_ROOT)

isEmpty(MYSQL_ROOT) {
    macx: MYSQL_ROOT = /usr/local/mysql-8.0.35-macos13-arm64
}

!isEmpty(MYSQL_ROOT) {
    MYSQL_LIB_DIR = $$MYSQL_ROOT/lib
    MYSQL_INCLUDE_DIR = $$MYSQL_ROOT/include
    INCLUDEPATH += $$MYSQL_INCLUDE_DIR
    LIBS += -L$$MYSQL_LIB_DIR -lmysqlclient

    macx {
        QMAKE_RPATHDIR += $$MYSQL_LIB_DIR
    }
}

unix:!macx: isEmpty(MYSQL_ROOT) {
    exists(/usr/include/mysql/mysql.h) {
        INCLUDEPATH += /usr/include/mysql
    }
    LIBS += -lmysqlclient
}
