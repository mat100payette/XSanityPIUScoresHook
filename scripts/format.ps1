[CmdletBinding()]
param(
    [switch]$Check,
    [ValidateSet('All', 'Cpp', 'Lua')]
    [string]$Language = 'All',
    [string]$ClangFormat,
    [string]$StyLua
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$formatCpp = $Language -in @('All', 'Cpp')
$formatLua = $Language -in @('All', 'Lua')

# Resolve every required formatter before changing files.
if ($formatCpp -and -not $ClangFormat) {
    $command = Get-Command clang-format -CommandType Application -ErrorAction SilentlyContinue
    if ($command) {
        $ClangFormat = $command.Source
    } else {
        $extensions = Join-Path $env:USERPROFILE '.vscode/extensions'
        $bundled = Get-ChildItem -Path "$extensions/ms-vscode.cpptools-*/LLVM/bin/clang-format.exe" -File -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending |
            Select-Object -First 1
        if ($bundled) {
            $ClangFormat = $bundled.FullName
        }
    }
}
if ($formatCpp -and -not $ClangFormat) {
    throw 'Install clang-format 15 or newer, or the recommended VS Code C/C++ extension. You can also pass -ClangFormat with its executable path.'
}

if ($formatLua -and -not $StyLua) {
    $command = Get-Command stylua -CommandType Application -ErrorAction SilentlyContinue
    if ($command) {
        $StyLua = $command.Source
    } else {
        $candidates = @(
            (Join-Path $env:USERPROFILE '.cargo/bin/stylua.exe'),
            (Join-Path $env:APPDATA 'Code/User/globalStorage/johnnymorganz.stylua/stylua.exe')
        )
        $StyLua = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    }
}
if ($formatLua -and -not $StyLua) {
    throw 'Install the recommended StyLua VS Code extension and open a Lua file to download its formatter, or install stylua on PATH. You can also pass -StyLua with its executable path.'
}

if ($formatCpp) {
    $folders = @('src', 'installer', 'tests') | ForEach-Object {
        Join-Path $repoRoot $_
    }
    $files = @(Get-ChildItem -LiteralPath $folders -Recurse -File |
        Where-Object { $_.Extension -in @('.cpp', '.h', '.hpp') } |
        Sort-Object FullName |
        ForEach-Object { $_.FullName })

    $options = @('--style=file')
    if ($Check) {
        $options += '--dry-run', '--Werror'
    } else {
        $options += '-i'
    }

    & $ClangFormat @options @files
    if ($LASTEXITCODE -ne 0) {
        throw 'C++ formatting failed.'
    }
}

if ($formatLua) {
    $options = @('--config-path', (Join-Path $repoRoot '.stylua.toml'), '--verify')
    if ($Check) {
        $options += '--check', '--output-format=summary'
    }

    & $StyLua @options (Join-Path $repoRoot 'hook') (Join-Path $repoRoot 'tests')
    if ($LASTEXITCODE -ne 0) {
        throw 'Lua formatting failed.'
    }
}

if ($Check) {
    Write-Output 'Source formatting is clean.'
} else {
    Write-Output 'Formatted sources.'
}
