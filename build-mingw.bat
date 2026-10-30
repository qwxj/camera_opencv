@echo off
chcp 65001 >nul
setlocal

rem ===============================================================
rem  build-mingw.bat  --  build camera_qt with Qt MinGW kit
rem
rem  用 Qt 6.11.1 mingw_64 + mingw1310 + 自编的 MinGW 版 OpenCV。
rem  同样做影子构建，产物落在 releaseCamera\ 下。
rem
rem  Usage:  build-mingw.bat            -> release
rem          build-mingw.bat debug      -> debug
rem
rem  平时请用 build.bat（MSVC 2022 版），这个脚本只是保留 MinGW 这条路。
rem ===============================================================

rem ---- 路径配置（可用同名环境变量覆盖）----
if not defined QTDIR      set "QTDIR=D:\qt6\6.11.1\mingw_64"
if not defined MINGWDIR   set "MINGWDIR=D:\qt6\Tools\mingw1310_64"
if not defined OPENCV_DIR set "OPENCV_DIR=D:\opencv\install-mingw"

set "PATH=%QTDIR%\bin;%MINGWDIR%\bin;%PATH%"
set "OPENCV_DIR=%OPENCV_DIR%"

set "BUILDDIR=%~dp0releaseCamera"
set "CONFIGNAME=release"
if /i "%~1"=="debug" set "CONFIGNAME=debug"

if not exist "%QTDIR%\bin\qmake.exe" (
    echo [ERROR] qmake not found: %QTDIR%\bin\qmake.exe
    goto :fail
)
if not exist "%OPENCV_DIR%\include\opencv2\core.hpp" (
    echo [ERROR] OpenCV headers not found: %OPENCV_DIR%\include\opencv2\core.hpp
    goto :fail
)

cd /d "%~dp0"
if not exist "%BUILDDIR%" mkdir "%BUILDDIR%"
cd "%BUILDDIR%"

echo === qmake (MinGW) ===
"%QTDIR%\bin\qmake.exe" "..\camera_qt.pro"
if errorlevel 1 goto :fail

echo.
echo === mingw32-make %CONFIGNAME% ===
"%MINGWDIR%\bin\mingw32-make.exe" -j8 %CONFIGNAME%
if errorlevel 1 goto :fail

echo.
echo === copy OpenCV DLLs ===
copy /y "%OPENCV_DIR%\x64\mingw\bin\libopencv_*.dll" "%BUILDDIR%\%CONFIGNAME%\" >nul

echo.
echo ===============================================================
echo   BUILD OK
echo ===============================================================
if exist "%BUILDDIR%\release\camera_qt.exe" echo   release exe : %BUILDDIR%\release\camera_qt.exe
if exist "%BUILDDIR%\debug\camera_qt.exe"   echo   debug   exe : %BUILDDIR%\debug\camera_qt.exe
echo ===============================================================
echo.
pause
exit /b 0

:fail
echo.
echo ===============================================================
echo   BUILD FAILED
echo ===============================================================
echo.
pause
exit /b 1
