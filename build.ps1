<#
.SYNOPSIS
    Configure + build the SKSE plugin using CMakePresets.json (Ninja + MSVC + vcpkg).

.EXAMPLE
    .\build.ps1                 # release build
    .\build.ps1 -Preset debug   # debug build
    .\build.ps1 -Clean          # delete build\<preset> then rebuild
#>
param(
    [ValidateSet("release", "debug")]
    [string]$Preset = "release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

# ---------------------------------------------------- #
#                 Required envs check                  #
# ---------------------------------------------------- #

foreach ($name in "VCPKG_ROOT", "COMMONLIB_SSE_FOLDER") {
    $value = [Environment]::GetEnvironmentVariable($name)
    if (-not $value -or -not (Test-Path $value)) {
        throw "Environment variable $name is missing or points to a non-existent folder."
    }
}

# ---------------------------------------------------- #
#            Load MSVC x64 developer environment       #
# ---------------------------------------------------- #

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vcvars = $null

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsPath) { $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat" }
    }

    # Fallback: vswhere doesn't always list Insiders/Preview installs, so search the usual folders.
    if (-not $vcvars -or -not (Test-Path $vcvars)) {
        $vcvars = Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat",
                                "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue |
                  Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $vcvars) { throw "No Visual Studio installation with C++ tools found." }

    Write-Host "Loading MSVC environment from $vcvars" -ForegroundColor Cyan
    # vcvars overwrites VCPKG_ROOT with the copy bundled in Visual Studio; keep ours.
    $userVcpkgRoot = $env:VCPKG_ROOT
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
    $env:VCPKG_ROOT = $userVcpkgRoot
}
Write-Host "Using vcpkg at $env:VCPKG_ROOT" -ForegroundColor Cyan

$ninja = Get-Command ninja.exe -ErrorAction SilentlyContinue
if (-not $ninja) { throw "ninja.exe not found on PATH." }

# ---------------------------------------------------- #
#                   Configure + Build                  #
# ---------------------------------------------------- #

$buildDir = Join-Path $PSScriptRoot "build\$Preset"

# A cache configured against a different vcpkg can't be reused; start fresh.
$cache = Join-Path $buildDir "CMakeCache.txt"
if (-not $Clean -and (Test-Path $cache)) {
    $expected = ($env:VCPKG_ROOT -replace '\\', '/').TrimEnd('/')
    $cached = Select-String -Path $cache -Pattern '^CMAKE_TOOLCHAIN_FILE:[^=]*=(.*)$' | Select-Object -First 1
    if ($cached -and -not $cached.Matches[0].Groups[1].Value.StartsWith($expected, [StringComparison]::OrdinalIgnoreCase)) {
        Write-Host "Existing build cache uses a different vcpkg; cleaning." -ForegroundColor Yellow
        $Clean = $true
    }
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "Configuring ($Preset)..." -ForegroundColor Cyan
$configureArgs = @("--preset", $Preset, "-DCMAKE_MAKE_PROGRAM=$($ninja.Source)")

# CommonLibSSE-NG clones MinHook (hde64) from GitHub; use a local copy if one exists.
$minhook = Join-Path $PSScriptRoot "deps\minhook"
if (Test-Path (Join-Path $minhook "src\hde")) {
    Write-Host "Using local MinHook at $minhook" -ForegroundColor Cyan
    $configureArgs += "-DFETCHCONTENT_SOURCE_DIR_HDE64=$($minhook -replace '\\', '/')"
}

cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

Write-Host "Building ($Preset)..." -ForegroundColor Cyan
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host "Build succeeded: $buildDir" -ForegroundColor Green
