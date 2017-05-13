QT += testlib concurrent

TARGET = promisetests
CONFIG -= c++11
CONFIG += c++14
CONFIG += console
CONFIG -= app_bundle

TEMPLATE = app

SOURCES += \
  promisetests.cpp

HEADERS += \
  promisetests.h \
  ../promise.h

INCLUDEPATH += \
  ..
