HEADERS += \
    $$PWD/CMySql.h \
    $$PWD/mysqlconnpool.h

SOURCES += \
    $$PWD/CMySql.cpp \
    $$PWD/mysqlconnpool.cpp

POSTGRES_ROOT = $$(POSTGRES_ROOT)
POSTGRES_INCLUDE_DIR = $$(POSTGRES_INCLUDE_DIR)
POSTGRES_LIB_DIR = $$(POSTGRES_LIB_DIR)
POSTGRES_PG_CONFIG = $$(POSTGRES_PG_CONFIG)

isEmpty(POSTGRES_PG_CONFIG) {
    macx:exists(/opt/homebrew/opt/libpq/bin/pg_config): POSTGRES_PG_CONFIG = /opt/homebrew/opt/libpq/bin/pg_config
}

isEmpty(POSTGRES_PG_CONFIG) {
    macx:exists(/opt/homebrew/opt/postgresql@17/bin/pg_config): POSTGRES_PG_CONFIG = /opt/homebrew/opt/postgresql@17/bin/pg_config
}

isEmpty(POSTGRES_ROOT) {
    macx:exists(/opt/homebrew/opt/libpq): POSTGRES_ROOT = /opt/homebrew/opt/libpq
}

isEmpty(POSTGRES_ROOT) {
    macx:exists(/opt/homebrew/opt/postgresql@17): POSTGRES_ROOT = /opt/homebrew/opt/postgresql@17
}

isEmpty(POSTGRES_INCLUDE_DIR):!isEmpty(POSTGRES_PG_CONFIG) {
    POSTGRES_INCLUDE_DIR = $$system($$shell_quote($$POSTGRES_PG_CONFIG) --includedir)
    POSTGRES_INCLUDE_DIR = $$replace(POSTGRES_INCLUDE_DIR, [\\r\\n]+, )
}

isEmpty(POSTGRES_LIB_DIR):!isEmpty(POSTGRES_PG_CONFIG) {
    POSTGRES_LIB_DIR = $$system($$shell_quote($$POSTGRES_PG_CONFIG) --libdir)
    POSTGRES_LIB_DIR = $$replace(POSTGRES_LIB_DIR, [\\r\\n]+, )
}

isEmpty(POSTGRES_INCLUDE_DIR):!isEmpty(POSTGRES_ROOT) {
    POSTGRES_INCLUDE_DIR = $$POSTGRES_ROOT/include
}

POSTGRES_INCLUDE_ALT_DIR =
!isEmpty(POSTGRES_ROOT) {
    POSTGRES_INCLUDE_ALT_DIR = $$POSTGRES_ROOT/include/postgresql
}

isEmpty(POSTGRES_LIB_DIR):!isEmpty(POSTGRES_ROOT) {
    POSTGRES_LIB_DIR = $$POSTGRES_ROOT/lib
}

exists($$POSTGRES_INCLUDE_DIR/libpq-fe.h) {
    INCLUDEPATH += $$POSTGRES_INCLUDE_DIR
}
exists($$POSTGRES_INCLUDE_ALT_DIR/libpq-fe.h) {
    INCLUDEPATH += $$POSTGRES_INCLUDE_ALT_DIR
}

!isEmpty(POSTGRES_LIB_DIR) {
    LIBS += -L$$POSTGRES_LIB_DIR -lpq

    macx {
        QMAKE_RPATHDIR += $$POSTGRES_LIB_DIR
    }
} else:unix {
    CONFIG += link_pkgconfig
    PKGCONFIG += libpq
}
