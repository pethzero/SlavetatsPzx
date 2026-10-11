@echo off
rem Usage: build.bat [release|debug]
set PRESET=%1
if "%PRESET%"=="" set PRESET=release

call "C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Users\user\AppData\Local\.xmake\packages\c\cmake\4.2.3\674adf9609f646bc9f678b5e946b080a\bin;C:\Users\user\AppData\Local\.xmake\packages\n\ninja\v1.13.2\388143d312d14320a9c5240d43405107\bin;%PATH%"
set VCPKG_ROOT=C:\vcpkg
cd /d "%~dp0"

rem SKSE_SUPPORT_PATCH_SAFETY=OFF: skip CommonLibSSE-NG's MinHook (hde64) download from GitHub.
cmake --preset %PRESET% -DSKSE_SUPPORT_PATCH_SAFETY=OFF || goto :fail
cmake --build build/%PRESET% || goto :fail

echo.
echo Build succeeded: build\%PRESET%
pause
exit /b 0

:fail
echo.
echo Build FAILED
pause
exit /b 1
