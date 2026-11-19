# candll.pri - include this file in your own qmake project to use CanDLL:
#
#   include($$PWD/../CanDLL/candll.pri)
#
# Build the library first (qmake + make/nmake in the CanDLL directory).
# CanDLL is always a shared library (CanDLL.dll + import library CanDLL.lib).
#
# NOTE for static Qt kits: CanDLL.dll embeds its Qt copy. Do NOT also link
# Qt into YOUR application (that would create two Qt runtimes in one
# process). Build your app non-Qt, or have it consume Qt only through the
# CanDLL interface - see README, "Using a static Qt build".

# Requires Qt 5.14 or newer (5.14 .. 6.x)
lessThan(QT_MAJOR_VERSION, 5) {
    error("CanDLL requires Qt 5.14 or newer")
}
equals(QT_MAJOR_VERSION, 5) {
    lessThan(QT_MINOR_VERSION, 14) {
        error("CanDLL requires Qt 5.14 or newer")
    }
}

CANDLL_ROOT = $$PWD

INCLUDEPATH += $$CANDLL_ROOT/src
DEPENDPATH  += $$CANDLL_ROOT/src

LIBS += -L$$CANDLL_ROOT/lib -lCanDLL
LIBS += -L$$[QT_INSTALL_LIBS]

win32:contains(QT_CONFIG, static) {
    warning("Static Qt kit detected: CanDLL.dll embeds Qt, so your app must not link Qt itself.")
}
