@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion

rem ===============================================================
rem  build.bat  --  one-click build for camera_qt  (MSVC 2022)
rem
rem  用 MSVC 2022 + Qt 6.11.1 msvc2022_64 + OpenCV 5 预编译包。
rem  做「影子构建」：qmake 生成的 Makefile / release\ 全部落在
rem  build-msvc\ 下，工程根目录只留源码。
rem
rem  Usage:  build.bat                  -> release
rem          build.bat debug            -> debug
rem          build.bat release nopause  -> 不暂停（给脚本用）
rem
rem  MinGW 版见同目录 build-mingw.bat
rem ===============================================================

rem ---- 路径配置：按本机实际情况改这几行 ----
set "QTDIR=D:\qt6\6.11.1\msvc2022_64"
set "VCVARS=D:\vs2022\VC\Auxiliary\Build\vcvars64.bat"
set "OPENCV_DIR=D:\opencv\build"
set "JOM=D:\qt6\Tools\QtCreator\bin\jom\jom.exe"

set "CONFIGNAME=release"
if /i "%~1"=="debug" set "CONFIGNAME=debug"
set "NOPAUSE="
if /i "%~2"=="nopause" set "NOPAUSE=1"
if /i "%~1"=="nopause" set "NOPAUSE=1"

set "BUILDDIR=%~dp0build-msvc"
set "OUTDIR=%BUILDDIR%\%CONFIGNAME%"

if not exist "%VCVARS%" (
    echo [ERROR] 找不到 vcvars64.bat: %VCVARS%
    echo         请把 build.bat 里的 VCVARS 改成你的 Visual Studio 路径。
    goto :fail
)
if not exist "%QTDIR%\bin\qmake.exe" (
    echo [ERROR] 找不到 qmake: %QTDIR%\bin\qmake.exe
    goto :fail
)
if not exist "%OPENCV_DIR%\include\opencv2\core.hpp" (
    echo [ERROR] 找不到 OpenCV 头文件: %OPENCV_DIR%\include\opencv2\core.hpp
    goto :fail
)

echo ===============================================================
echo   camera_qt  MSVC build   [%CONFIGNAME%]
echo   Qt     : %QTDIR%
echo   OpenCV : %OPENCV_DIR%
echo ===============================================================
echo.

echo === 准备 MSVC 环境 (vcvars64) ===
call "%VCVARS%" >nul
if errorlevel 1 goto :fail

set "PATH=%QTDIR%\bin;%PATH%"

cd /d "%~dp0"
if not exist "%BUILDDIR%" mkdir "%BUILDDIR%"
cd "%BUILDDIR%"

echo === qmake ===
if /i "%CONFIGNAME%"=="debug" (
    "%QTDIR%\bin\qmake.exe" "..\camera_qt.pro" "CONFIG+=debug" "CONFIG-=release"
) else (
    "%QTDIR%\bin\qmake.exe" "..\camera_qt.pro" "CONFIG+=release" "CONFIG-=debug"
)
if errorlevel 1 goto :fail

echo.
echo === 编译 (%CONFIGNAME%) ===
rem  jom 更快，但影子构建目录比 Qt 安装目录深时，qmake 会生成类似
rem  ..\..\..\..\..\qt6\... 这种越过盘符根目录的相对依赖，
rem  jom 解析不了会报 "dependent ... does not exist"，这时自动退回 nmake。
set "BUILD_OK="
if exist "%JOM%" (
    "%JOM%" -j8 %CONFIGNAME%
    if !errorlevel! equ 0 set "BUILD_OK=1"
)
if not defined BUILD_OK (
    echo.
    echo [INFO] jom 不可用或失败，改用 nmake 重新编译 ...
    nmake %CONFIGNAME%
    if errorlevel 1 goto :fail
)

echo.
echo === 部署 Qt 运行时 (windeployqt) ===
"%QTDIR%\bin\windeployqt.exe" --%CONFIGNAME% --no-translations --no-opengl-sw --no-system-d3d-compiler "%OUTDIR%\camera_qt.exe"
if errorlevel 1 echo   [WARN] windeployqt 失败，只要 Qt 的 bin 在 PATH 里仍然能跑。

echo.
echo === 拷贝 OpenCV 运行时 DLL ===
copy /y "%OPENCV_DIR%\x64\vc16\bin\opencv_*.dll" "%OUTDIR%\" >nul
if errorlevel 1 (
    echo   [WARN] 拷贝 OpenCV DLL 失败，请手动把
    echo          %OPENCV_DIR%\x64\vc16\bin\opencv_world*.dll
    echo          和 opencv_videoio_msmf*.dll 放到 exe 旁边。
) else (
    echo   OpenCV DLL 已拷贝到 %OUTDIR%
)

echo.
echo ===============================================================
echo   BUILD OK
echo ===============================================================
echo   exe : %OUTDIR%\camera_qt.exe
echo.
echo   run : "%OUTDIR%\camera_qt.exe"                 打开界面
echo         "%OUTDIR%\camera_qt.exe" --list          列出摄像头
echo         "%OUTDIR%\camera_qt.exe" --opencv-info   打印 OpenCV 版本
echo         "%OUTDIR%\camera_qt.exe" --selftest      不用摄像头自检处理链路
echo.
echo   提示：如果摄像头打不开，先确认这个目录没有「低完整性」标签：
echo         icacls "%OUTDIR%" ^| findstr /i "Mandatory"
echo ===============================================================
echo.

if not defined NOPAUSE pause
exit /b 0

:fail
echo.
echo ===============================================================
echo   BUILD FAILED
echo ===============================================================
echo.
if not defined NOPAUSE pause
exit /b 1
