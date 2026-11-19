# CanDLL example / smoke test (console)

QT -= gui

TARGET   = CanTest
TEMPLATE = app
CONFIG  += c++17 console
CONFIG  -= app_bundle

# Requires Qt 5.14 or newer (5.14 .. 6.x)
lessThan(QT_MAJOR_VERSION, 5) {
    error("CanDLL requires Qt 5.14 or newer")
}
equals(QT_MAJOR_VERSION, 5) {
    lessThan(QT_MINOR_VERSION, 14) {
        error("CanDLL requires Qt 5.14 or newer")
    }
}

INCLUDEPATH += $$PWD/../src
DEPENDPATH  += $$PWD/../src

DESTDIR = $$PWD/../bin

LIBS += -L$$PWD/../lib -lCanDLL
LIBS += -L$$[QT_INSTALL_LIBS]

win32 {
    # Link only after src has produced the library (root .pro uses
    # CONFIG += ordered; this also catches stale/missing lib states).
    PRE_TARGETDEPS += $$PWD/../lib/CanDLL.lib

    # Make CanDLL.dll findable next to the executable after a build
    # (guarded: skipped silently when not present where expected).
    QMAKE_POST_LINK += if exist $$shell_path($$PWD/../lib/CanDLL.dll) $$QMAKE_COPY /Y $$shell_path($$PWD/../lib/CanDLL.dll) $$shell_path($$PWD/../bin) $$escape_expand(\\n\\t)
    QMAKE_POST_LINK += if exist $$shell_path($$PWD/../lib/CanDLL.dll) $$QMAKE_COPY /Y $$shell_path($$PWD/../lib/CanDLL.dll) $$shell_path($$OUT_PWD)
}

SOURCES += main.cpp \
    console.cpp

HEADERS += console.h
