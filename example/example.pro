TEMPLATE = app
TARGET = promiseExample

QT += core gui network concurrent

CONFIG -= c++11
CONFIG += c++14
CONFIG += console
CONFIG -= app_bundle

SOURCES = \
  main.cpp \
  promiseexample.cpp

HEADERS = \
  ../promise.h \
  promiseexample.h

INCLUDEPATH += \
  ..
