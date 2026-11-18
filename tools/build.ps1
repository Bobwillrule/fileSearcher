param(
    [ValidateSet('windows-debug', 'windows-release', 'core-debug', 'windows-clean')]
    [string]$Preset = 'windows-debug',
    [switch]$ConfigureOnly,
    [switch]$ModelTests
)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$dependencyRoot = $env:VCPKG_ROOT
if (!$dependencyRoot) { $dependencyRoot = Join-Path $repoRoot '.tools\vcpkg' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 C++ Build Tools first. See README.md.' }
$vsRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) { throw 'No complete Visual Studio C++ toolchain was found.' }
Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
$env:VCPKG_ROOT = $dependencyRoot
$env:VCPKG_DISABLE_METRICS = '1'
if (!(Test-Path -LiteralPath (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) { throw 'Bootstrap vcpkg first. See README.md.' }
if ($Preset -ne 'core-debug') {
    if (!$env:QT_ROOT_DIR) { $env:QT_ROOT_DIR = Join-Path $repoRoot '.tools\qt\6.8.3\msvc2022_64' }
    if (!(Test-Path -LiteralPath (Join-Path $env:QT_ROOT_DIR 'lib\cmake\Qt6\Qt6Config.cmake'))) { throw 'Qt 6.8.3 MSVC x64 was not found. Set QT_ROOT_DIR or follow README.md.' }
    $env:CMAKE_PREFIX_PATH = $env:QT_ROOT_DIR
    $env:PATH = (Join-Path $env:QT_ROOT_DIR 'bin') + ';' + $env:PATH
}
Push-Location $repoRoot
try {
    & cmake --preset $Preset "-DFILESEARCHER_MODEL_TESTS=$($ModelTests.IsPresent.ToString().ToUpper())"
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    if (!$ConfigureOnly) {
        & cmake --build --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
        & ctest --preset $Preset
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
    }
} finally { Pop-Location }
