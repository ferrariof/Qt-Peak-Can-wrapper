# CanDLL - Qt CAN bus DLL (PEAK-System PCAN-Basic backend)
# Root build file: qmake + make/nmake
#
#   qmake CanDLL.pro
#   nmake        (MSVC)   |   mingw32-make   (MinGW)

TEMPLATE = subdirs
CONFIG += ordered

SUBDIRS += \
    TestDLL/TestDLL \
    src \
    example
