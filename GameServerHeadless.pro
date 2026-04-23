TARGET = GameServer
DEFINES += JAEGER_HEADLESS_BUILD
QT -= gui widgets
CONFIG += console
CONFIG -= app_bundle

include(GameServer.pro)
