[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [switch]$Test,
    [switch]$Package
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if ($repoRoot -match '["&|<>%^!\r\n]') { throw 'Build from a path without shell metacharacters.' }
if ($Package -and $Configuration -ne 'Release') { throw 'Packages require Release.' }
$version = (Get-Content -LiteralPath (Join-Path $repoRoot 'VERSION') -Raw).Trim()
if ($version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$' -or
    @($version.Split('.') | Where-Object { [long]$_ -gt 65534 }).Count) {
    throw 'VERSION must be a three-part numeric version (each part 0-65534).'
}
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
$toolchain = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $toolchain) { throw 'Install the MSVC x64 tools and Windows SDK.' }
$vcvars = Join-Path $toolchain 'VC\Auxiliary\Build\vcvars64.bat'
$buildRoot = Join-Path $repoRoot "build\$Configuration"
$objectRoot = Join-Path $buildRoot 'obj'
$distRoot = Join-Path $repoRoot 'dist'
if ($Configuration -eq 'Debug') { $distRoot = Join-Path $distRoot 'Debug' }
New-Item -ItemType Directory -Path $buildRoot, $objectRoot, $distRoot -Force | Out-Null
$utf8 = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText((Join-Path $buildRoot 'version.h'), "#pragma once`n#define PIU_VERSION `"$version`"`n#define PIU_VERSION_W L`"$version`"`n", $utf8)

function Quote([string]$value) { return '"' + $value + '"' }
function ResourcePath([string]$value) { return $value.Replace('\', '\\') }
function Write-Resources([string]$name, [string]$description, [bool]$embedApp) {
    $fileVersion = $version.Replace('.', ',') + ',0'
    $icons = "IDI_SETUP ICON `"$(ResourcePath (Join-Path $repoRoot 'installer\setup.ico'))`""
    if (-not $embedApp) {
        $icons = "IDI_APP ICON `"$(ResourcePath (Join-Path $repoRoot 'src\app.ico'))`"`n" + $icons
    }

    $resources = @"
#include <windows.h>
#include "$(ResourcePath (Join-Path $repoRoot 'src\dialogs.rc'))"
101 RCDATA "$(ResourcePath (Join-Path $repoRoot 'hook\ScreenSystemLayer aux.lua'))"
102 RCDATA "$(ResourcePath (Join-Path $repoRoot 'src\overlay.html'))"
104 RCDATA "$(ResourcePath (Join-Path $repoRoot 'LICENSE'))"
$icons
1 RT_MANIFEST "$(ResourcePath (Join-Path $repoRoot 'src\app.manifest'))"
1 VERSIONINFO
FILEVERSION $fileVersion
PRODUCTVERSION $fileVersion
FILEFLAGSMASK 0x3fL
FILEFLAGS 0
FILEOS VOS_NT_WINDOWS32
FILETYPE VFT_APP
BEGIN
  BLOCK "StringFileInfo"
  BEGIN
    BLOCK "040904b0"
    BEGIN
      VALUE "CompanyName", "mat100payette\0"
      VALUE "FileDescription", "$description\0"
      VALUE "FileVersion", "$version\0"
      VALUE "ProductName", "XSanityPIUScoresHook\0"
      VALUE "ProductVersion", "$version\0"
      VALUE "OriginalFilename", "$name.exe\0"
      VALUE "LegalCopyright", "Copyright (c) 2026 mat100payette\0"
    END
  END
  BLOCK "VarFileInfo"
  BEGIN
    VALUE "Translation", 0x0409, 1200
  END
END
"@
    if ($embedApp) { $resources += "`n103 RCDATA `"$(ResourcePath (Join-Path $distRoot 'PiuCompanion.exe'))`"`n" }
    [System.IO.File]::WriteAllText((Join-Path $buildRoot "$name.rc"), $resources, $utf8)
}
Write-Resources 'PiuCompanion' 'PIU Companion' $false
Write-Resources 'PiuCompanionSetup' 'PIU Companion Setup' $true
$flags = '/nologo /std:c++20 /permissive- /W4 /WX /EHsc /Zc:__cplusplus /utf-8 /external:anglebrackets /external:W0 /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00'
if ($Configuration -eq 'Release') { $flags += ' /MT /O2 /DNDEBUG /Gy /Gw' }
else { $flags += ' /MTd /Od /Zi' }
$includes = '/I' + (Quote (Join-Path $repoRoot 'src')) + ' /I' + (Quote (Join-Path $repoRoot 'installer')) + ' /I' + (Quote $buildRoot) + ' /Fd' + (Quote (Join-Path $buildRoot 'compiler.pdb'))
$libs = 'user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib comctl32.lib crypt32.lib advapi32.lib winhttp.lib ws2_32.lib normaliz.lib runtimeobject.lib windowscodecs.lib dwmapi.lib oleacc.lib'
$common = @('platform', 'model', 'api', 'engine', 'game_hook', 'overlay')
$sourceArgs = ($common | ForEach-Object { Quote (Join-Path $repoRoot "src\$_.cpp") }) -join ' '
$objArgs = ($common | ForEach-Object { Quote (Join-Path $objectRoot "$_.obj") }) -join ' '
$installObjArgs = (@('platform', 'model', 'game_hook', 'install', 'setup_view') | ForEach-Object { Quote (Join-Path $objectRoot "$_.obj") }) -join ' '
$compile = "cl.exe $flags $includes"
$link = '/link /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA /OPT:REF /OPT:ICF'
$commands = [System.Collections.Generic.List[string]]::new()
$commands.Add('@echo off')
$commands.Add('chcp 65001 >nul')
$commands.Add('call ' + (Quote $vcvars) + ' >nul')
$commands.Add('if errorlevel 1 exit /b 1')
$commands.Add("$compile /c $sourceArgs $(Quote (Join-Path $repoRoot 'installer\install.cpp')) $(Quote (Join-Path $repoRoot 'installer\setup_view.cpp')) /Fo$(Quote ($objectRoot + '/'))")
$commands.Add('if errorlevel 1 exit /b 1')
$commands.Add('rc.exe /nologo /c65001 /I' + (Quote (Join-Path $repoRoot 'src')) + ' /fo' + (Quote (Join-Path $buildRoot 'PiuCompanion.res')) + ' ' + (Quote (Join-Path $buildRoot 'PiuCompanion.rc')))
$commands.Add('if errorlevel 1 exit /b 1')
$commands.Add("$compile $(Quote (Join-Path $repoRoot 'src\app.cpp')) $objArgs $(Quote (Join-Path $buildRoot 'PiuCompanion.res')) /Fo$(Quote (Join-Path $objectRoot 'app.obj')) /Fe$(Quote (Join-Path $distRoot 'PiuCompanion.exe')) $link /SUBSYSTEM:WINDOWS $libs")
$commands.Add('if errorlevel 1 exit /b 1')
$commands.Add('rc.exe /nologo /c65001 /I' + (Quote (Join-Path $repoRoot 'src')) + ' /fo' + (Quote (Join-Path $buildRoot 'PiuCompanionSetup.res')) + ' ' + (Quote (Join-Path $buildRoot 'PiuCompanionSetup.rc')))
$commands.Add('if errorlevel 1 exit /b 1')
$commands.Add("$compile $(Quote (Join-Path $repoRoot 'installer\main.cpp')) $installObjArgs $(Quote (Join-Path $buildRoot 'PiuCompanionSetup.res')) /Fo$(Quote (Join-Path $objectRoot 'setup.obj')) /Fe$(Quote (Join-Path $distRoot 'PiuCompanionSetup.exe')) $link /SUBSYSTEM:WINDOWS $libs")
$commands.Add('if errorlevel 1 exit /b 1')
if ($Test) {
    $commands.Add("$compile $(Quote (Join-Path $repoRoot 'tests\checks.cpp')) $objArgs $(Quote (Join-Path $objectRoot 'install.obj')) $(Quote (Join-Path $objectRoot 'setup_view.obj')) $(Quote (Join-Path $buildRoot 'PiuCompanion.res')) /Fo$(Quote (Join-Path $objectRoot 'checks.obj')) /Fe$(Quote (Join-Path $buildRoot 'Checks.exe')) $link /SUBSYSTEM:CONSOLE $libs")
    $commands.Add('if errorlevel 1 exit /b 1')
}
foreach ($name in @('PiuCompanion', 'PiuCompanionSetup')) {
    $commands.Add('dumpbin.exe /nologo /dependents ' + (Quote (Join-Path $distRoot "$name.exe")) + ' >' + (Quote (Join-Path $buildRoot "$name.dependencies.txt")))
    $commands.Add('if errorlevel 1 exit /b 1')
}
$batch = Join-Path $buildRoot 'compile.cmd'
[System.IO.File]::WriteAllLines($batch, $commands, $utf8)
& $env:ComSpec /d /c $batch
if ($LASTEXITCODE -ne 0) { throw 'Native compilation failed.' }
foreach ($name in @('PiuCompanion', 'PiuCompanionSetup')) {
    $dependencies = Get-Content -LiteralPath (Join-Path $buildRoot "$name.dependencies.txt") -Raw
    if ($dependencies -match '(?i)(mscoree|vcruntime|msvcp\d|ucrtbase)\.dll') { throw "$name has an unexpected runtime dependency." }
    $bytes = [System.IO.File]::ReadAllBytes((Join-Path $distRoot "$name.exe"))
    $pe = [BitConverter]::ToInt32($bytes, 0x3c)
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664 -or
        [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20b -or
        [BitConverter]::ToUInt64($bytes, $pe + 24 + 112 + 14 * 8) -ne 0) {
        throw "$name must be native x64 with no CLR directory."
    }
}
Write-Output "Built $Configuration v$version."
if ($Test) {
    & (Join-Path $buildRoot 'Checks.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Behavioral checks failed.' }
}
if ($Package) {
    $artifacts = Join-Path $repoRoot 'artifacts'
    New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
    $name = "XSanityPIUScoresHook-v$version-win-x64-setup.exe"
    $setup = Join-Path $artifacts $name
    Copy-Item -LiteralPath (Join-Path $distRoot 'PiuCompanionSetup.exe') -Destination $setup -Force
    $hash = (Get-FileHash -LiteralPath $setup -Algorithm SHA256).Hash.ToLowerInvariant()
    [System.IO.File]::WriteAllText((Join-Path $artifacts ($name + '.sha256')), "$hash  $name`n", [System.Text.Encoding]::ASCII)
    Write-Output "Share $setup"
}
