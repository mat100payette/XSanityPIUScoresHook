[CmdletBinding()]
param([Parameter(Mandatory)][string]$VcVars)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$toolRoot = Join-Path $repoRoot 'build/tools'
$luaRoot = Join-Path $toolRoot 'lua-5.1.5'
$lua = Join-Path $luaRoot 'lua.exe'
if (-not (Test-Path -LiteralPath $lua)) {
    New-Item -ItemType Directory -Path $toolRoot -Force | Out-Null
    $archive = Join-Path $toolRoot 'lua-5.1.5.tar.gz'
    $sha256 = '2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333'
    if (-not (Test-Path -LiteralPath $archive)) {
        Invoke-WebRequest -Uri 'https://www.lua.org/ftp/lua-5.1.5.tar.gz' -OutFile $archive
    }
    if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $sha256) {
        throw 'Lua test runtime download failed checksum verification.'
    }
    & tar.exe -xzf $archive -C $toolRoot
    if ($LASTEXITCODE -ne 0) { throw 'Could not extract the Lua test runtime.' }

    $objects = Join-Path $luaRoot 'obj'
    New-Item -ItemType Directory -Path $objects -Force | Out-Null
    $sources = (Get-ChildItem -LiteralPath (Join-Path $luaRoot 'src') -Filter '*.c' |
        Where-Object { $_.Name -notin @('luac.c', 'print.c') } |
        ForEach-Object { '"' + $_.FullName + '"' }) -join ' '
    $commands = @(
        '@echo off',
        ('call "' + $VcVars + '" >nul'),
        'if errorlevel 1 exit /b 1',
        ('cl.exe /nologo /O2 /MT /D_CRT_SECURE_NO_WARNINGS /DLUA_USE_WINDOWS ' + $sources +
            ' /Fo"' + $objects + '/" /Fe"' + $lua + '"'),
        'if errorlevel 1 exit /b 1'
    )
    $batch = Join-Path $luaRoot 'compile.cmd'
    [IO.File]::WriteAllLines($batch, $commands, [Text.UTF8Encoding]::new($false))
    & $env:ComSpec /d /c $batch
    if ($LASTEXITCODE -ne 0) { throw 'Could not build the Lua test runtime.' }
}

& $lua (Join-Path $repoRoot 'tests/hook.lua') (Join-Path $repoRoot 'hook/ScreenSystemLayer overlay.lua')
if ($LASTEXITCODE -ne 0) { throw 'Game exporter checks failed.' }
