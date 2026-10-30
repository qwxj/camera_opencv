# ---------------------------------------------------------------
#  camera_qt.pro —— qmake 工程文件（Qt6 + OpenCV）
#
#  ⚠ 重要：本工程必须放在「不带低完整性标签」的目录里！
#
#      Windows 会让从低完整性文件启动的进程以低完整性运行，
#      而低完整性进程被系统禁止访问摄像头和麦克风
#      （表现：QMediaDevices::videoInputs() 返回 0 个，
#        或者 OpenCV 报 AccessDenied / 0x80070005，极难排查）。
#
#      随手检查：
#          icacls "目录" | Select-String "Mandatory Label"
#          出现 Mandatory Label\Low Mandatory Level 就是它。
#
# ---------------------------------------------------------------
#  构建方式（推荐直接双击同目录的 build.bat）：
#
#    MSVC 2022（默认，用 build.bat）：
#        call "D:\vs2022\VC\Auxiliary\Build\vcvars64.bat"
#        cd build-msvc
#        qmake ..\camera_qt.pro
#        jom -j8 release            （没有 jom 就用 nmake release）
#
#    MinGW（用 build-mingw.bat）：
#        cd releaseCamera
#        qmake ..\camera_qt.pro
#        mingw32-make -j8 release
#
#  也可以在 Qt Creator 里「打开文件或项目」选这个 .pro，
#  套件选 6.11.1 msvc2022_64，构建目录设成 .../CameraQt/build-msvc。
# ---------------------------------------------------------------

QT += core gui widgets
QT += multimedia multimediawidgets

CONFIG += c++17

TEMPLATE = app
TARGET   = camera_qt

SOURCES += \
    main.cpp \
    camerawindow.cpp \
    videoview.cpp \
    captureworker.cpp \
    processing.cpp \
    cli.cpp

HEADERS += \
    camerawindow.h \
    videoview.h \
    captureworker.h \
    processing.h \
    cli.h

# 界面用 Qt Designer 的 .ui 文件（qmake 会调 uic 生成 ui_camerawindow.h）
FORMS += \
    camerawindow.ui

# 源码里有中文，MSVC 下需要 /utf-8
msvc: QMAKE_CXXFLAGS += /utf-8

# 少让 windows.h 往代码里塞 min/max 宏，免得和 std::min/std::max 打架
win32: DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN

# ===============================================================
#  OpenCV
#
#  默认路径按本机装好的来，也可以用环境变量 OPENCV_DIR 覆盖：
#      set OPENCV_DIR=D:\somewhere\opencv\build
# ===============================================================

OPENCV_ROOT = $$(OPENCV_DIR)
isEmpty(OPENCV_ROOT) {
    win32-msvc*: OPENCV_ROOT = D:/opencv/build
    else:        OPENCV_ROOT = D:/opencv/install-mingw
}

win32-msvc* {
    # 官方 Windows 预编译包：整套打成一个 world 大库，Release/Debug 各一份
    OPENCV_INC    = $$OPENCV_ROOT/include
    OPENCV_LIBDIR = $$OPENCV_ROOT/x64/vc16/lib
    CONFIG(debug, debug|release): OPENCV_MODULES = opencv_world500d
    else:                         OPENCV_MODULES = opencv_world500
} else {
    # 自己用 MinGW 编的那套：模块是分开的，没有 world
    OPENCV_INC    = $$OPENCV_ROOT/include
    OPENCV_LIBDIR = $$OPENCV_ROOT/x64/mingw/lib
    OPENCV_MODULES = opencv_core500 opencv_imgproc500 opencv_imgcodecs500 \
                     opencv_highgui500 opencv_videoio500 opencv_photo500
}

!exists($$OPENCV_INC/opencv2/core.hpp) {
    error("找不到 OpenCV 头文件: $$OPENCV_INC/opencv2/core.hpp —— 请设置环境变量 OPENCV_DIR 指向 OpenCV 根目录")
}

INCLUDEPATH += $$OPENCV_INC

LIBS += -L$$OPENCV_LIBDIR
for(module, OPENCV_MODULES) {
    LIBS += -l$$module
}

message("OpenCV 头文件 : $$OPENCV_INC")
message("OpenCV 库目录 : $$OPENCV_LIBDIR   模块: $$OPENCV_MODULES")
