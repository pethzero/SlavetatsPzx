<#
.SYNOPSIS
    Configure + build SlavetatsUIPzx using CMakePresets.json (Ninja + MSVC + vcpkg manifest).

.EXAMPLE
    .\build.ps1                                  # release build (build\release-msvc)
    .\build.ps1 -Preset debug                    # debug build   (build\debug-msvc)
    .\build.ps1 -Clean                           # delete the build folder, then rebuild
    .\build.ps1 -OutputDir "D:\MO2\mods\SlavetatsUIPzx"
                                                 # also copy the DLL/PDB + config into a mod folder
    .\build.ps1 -Online                          # download/update the vcpkg dependencies (git + internet)

    Dependencies live in ..\REF\vcpkg_installed (next to this folder). Once they are there, builds use them as
    they are and never call git or the network; -Online (or update_deps.bat) installs or updates them. Without
    that folder the first build is online on its own.
#>
param(
    [ValidateSet("release", "debug")]
    [string]$Preset = "release",
    [switch]$Clean,
    [switch]$Online,
    [string]$OutputDir
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$configurePreset = "build-$Preset-msvc"
$buildDir = Join-Path $PSScriptRoot "build\$Preset-msvc"

# ---------------------------------------------------- #
#                 Required envs check                  #
# ---------------------------------------------------- #

if (-not $env:VCPKG_ROOT -and (Test-Path "C:\vcpkg")) { $env:VCPKG_ROOT = "C:\vcpkg" }
if (-not $env:VCPKG_ROOT -or -not (Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake")) {
    throw "Environment variable VCPKG_ROOT is missing or does not point to a vcpkg checkout."
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

# cmake/ninja: Visual Studio's copies are on PATH after vcvars; otherwise fall back to xmake's packages.
foreach ($tool in "cmake", "ninja") {
    if (-not (Get-Command "$tool.exe" -ErrorAction SilentlyContinue)) {
        $found = Get-ChildItem "$env:LOCALAPPDATA\.xmake\packages\*\$tool\*\*\bin\$tool.exe" -ErrorAction SilentlyContinue |
                 Sort-Object FullName -Descending | Select-Object -First 1
        if (-not $found) { throw "$tool.exe not found on PATH." }
        $env:PATH = "$($found.DirectoryName);$env:PATH"
    }
}
$ninja = Get-Command ninja.exe

# ---------------------------------------------------- #
#                   Configure + Build                  #
# ---------------------------------------------------- #

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

# ---------------------------------------------------- #
#        Dependencies: local copy in ..\REF            #
# ---------------------------------------------------- #

$depsDir = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\REF\vcpkg_installed"))
$depsCMake = $depsDir -replace '\\', '/'
$haveDeps = Test-Path (Join-Path $depsDir "x64-windows-skse\share\commonlibsse-ng")
$install = $Online -or -not $haveDeps
if ($install) {
    Write-Host "Installing/updating dependencies into $depsDir (git + internet)..." -ForegroundColor Cyan
} else {
    Write-Host "Using local dependencies from $depsDir (offline; build.ps1 -Online to update)" -ForegroundColor Cyan
}

# A cache that found its packages in another folder (the old per-build vcpkg_installed) keeps those paths
if (-not $Clean -and (Test-Path $cache)) {
    $cachedDeps = Select-String -Path $cache -Pattern '^VCPKG_INSTALLED_DIR:[^=]*=(.*)$' | Select-Object -First 1
    if (-not $cachedDeps -or $cachedDeps.Matches[0].Groups[1].Value.TrimEnd('/') -ne $depsCMake) {
        # Only the cache goes (the packages are looked up again); the rest of the build folder stays
        Write-Host "Existing build cache uses other dependencies; reconfiguring." -ForegroundColor Yellow
        Remove-Item $cache -Force
    }
}

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Removing $buildDir" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildDir
}

Write-Host "Configuring ($configurePreset)..." -ForegroundColor Cyan
$manifestInstall = if ($install) { "ON" } else { "OFF" }
cmake --preset $configurePreset "-DCMAKE_MAKE_PROGRAM=$($ninja.Source)" "-DVCPKG_INSTALLED_DIR=$depsCMake" "-DVCPKG_MANIFEST_INSTALL=$manifestInstall"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed." }

Write-Host "Building ($configurePreset)..." -ForegroundColor Cyan
cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

Write-Host "Build succeeded: $buildDir" -ForegroundColor Green

# ---------------------------------------------------- #
#                  Optional mod output                 #
# ---------------------------------------------------- #

if ($OutputDir) {
    $plugins = Join-Path $OutputDir "SKSE\Plugins"
    $config = Join-Path $plugins "SlaveTatsNG"
    New-Item -ItemType Directory -Force $config | Out-Null

    Copy-Item (Join-Path $buildDir "SlavetatsUIPzx.dll") $plugins -Force
    $pdb = Join-Path $buildDir "SlavetatsUIPzx.pdb"
    if (Test-Path $pdb) { Copy-Item $pdb $plugins -Force }
    Copy-Item (Join-Path $PSScriptRoot "config\*") $config -Force

    # SlavetatsUIPzx.dll contains the whole SlaveTatsNG plugin; an old SlaveTatsNG.dll next to it would load twice
    $old = Join-Path $plugins "SlaveTatsNG.dll"
    if (Test-Path $old) { Write-Host "WARNING: remove $old (and any other SlaveTatsNG.dll in your load order)" -ForegroundColor Yellow }
    # The plugin was called SlavetatsPzx before ui-v3-1; both names would load the same plugin twice
    foreach ($stale in "SlavetatsPzx.dll", "SlavetatsPzx.pdb") {
        $path = Join-Path $plugins $stale
        if (Test-Path $path) { Remove-Item $path -Force; Write-Host "Removed the old $stale" -ForegroundColor Yellow }
    }

    Write-Host "Copied plugin + config to $OutputDir" -ForegroundColor Green
}
