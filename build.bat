@echo off
rem Usage: build.bat [release|debug] [mod output folder]
rem   build.bat
rem   build.bat debug
rem   build.bat release "D:\MO2\mods\SlavetatsPzx"
set PRESET=%~1
if "%PRESET%"=="" set PRESET=release

if "%~2"=="" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Preset %PRESET%
) else (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" -Preset %PRESET% -OutputDir "%~2"
)
if errorlevel 1 goto :fail

echo.
echo Build succeeded: build\%PRESET%-msvc
pause
exit /b 0

:fail
echo.
echo Build FAILED
pause
exit /b 1
