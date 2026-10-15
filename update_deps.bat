@echo off
rem Downloads/updates the vcpkg dependencies into ..\REF\vcpkg_installed (needs git + internet), then builds.
rem Run it after changing vcpkg.json or vcpkg-configuration.json. Normal builds (build.bat) stay offline.
rem Usage: update_deps.bat [release|debug]
set PRESET=%~1
if "%PRESET%"=="" set PRESET=release

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Preset %PRESET% -Online
if errorlevel 1 goto :fail

echo.
echo Dependencies updated and build succeeded: build\%PRESET%-msvc
pause
exit /b 0

:fail
echo.
echo FAILED
pause
exit /b 1
