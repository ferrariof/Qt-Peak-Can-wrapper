# CanDLL shared library (CanDLL.dll)

# Requires Qt 5.14 or newer (5.14 .. 6.x)
lessThan(QT_MAJOR_VERSION, 5) {
    error("CanDLL requires Qt 5.14 or newer")
}
equals(QT_MAJOR_VERSION, 5) {
    lessThan(QT_MINOR_VERSION, 14) {
        error("CanDLL requires Qt 5.14 or newer")
    }
}

QT -= gui

TARGET   = CanDLL
TEMPLATE = lib
DEFINES += CANDLL_LIBRARY

# Always a shared library, even on a static Qt kit (explicit requirement).
# With static Qt the whole QtCore is linked INTO CanDLL.dll, which makes the
# DLL self-contained: the consuming application must then NOT link another Qt
# copy itself (see README, "Using a static Qt build").
CONFIG += dll
static: CONFIG += plugin

CONFIG += c++17

# Qt 5.x MSVC builds resolve zlib.lib (and friends) through Qt5Core.lib's
# default-library directive; make sure the kit's lib dir is on the link path.
LIBS += -L$$[QT_INSTALL_LIBS]

DESTDIR = $$PWD/../lib

# Deterministic import-library location: pins CanDLL.lib next to
# the DLL regardless of build dir / shadow build, so consumers linking
# -L../lib -lCanDLL always get a lib that matches the just-built DLL.
win32 {
    win32-msvc {
        QMAKE_LFLAGS += $$shell_quote(/IMPLIB:$$shell_path($$PWD/../lib/CanDLL.lib))
    }

    # Safety net: some DESTDIR configurations still place the DLL in the build
    # tree (OUT_PWD or OUT_PWD/debug|release). Collect it into lib/ anyway;
    # "if exist" keeps the copy a no-op when DESTDIR worked already, and the
    # post-link step can never fail.
    DLLDIRS = $$OUT_PWD $$OUT_PWD/debug $$OUT_PWD/release
    for(d, DLLDIRS): \
        QMAKE_POST_LINK += if exist $$shell_path($$d/CanDLL.dll) $$QMAKE_COPY /Y $$shell_path($$d/CanDLL.dll) $$shell_path($$PWD/../lib) $$escape_expand(\\n\\t)

    # MinGW: qmake forces -Wl,--out-implib,.../libCanDLL.a after QMAKE_LFLAGS.
    # Copy the generated .a import library to CanDLL.lib (identical COFF format).
    win32-g++ {
        QMAKE_POST_LINK += if exist $$shell_path($$PWD/../lib/libCanDLL.a) $$QMAKE_COPY /Y $$shell_path($$PWD/../lib/libCanDLL.a) $$shell_path($$PWD/../lib/CanDLL.lib) $$escape_expand(\\n\\t)
    }
}

HEADERS += \
    candll_global.h \
    pcanbasic_api.h \
    canbus.h

SOURCES += \
    canbus.cpp

