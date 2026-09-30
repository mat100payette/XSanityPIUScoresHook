[CmdletBinding()]
param(
    [switch]$Check,
    [string]$ClangFormat
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not $ClangFormat) {
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
if (-not $ClangFormat) {
    throw 'Install clang-format 15 or newer, or the recommended VS Code C/C++ extension. You can also pass -ClangFormat with its executable path.'
}

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
if ($Check) {
    Write-Output 'C++ formatting is clean.'
} else {
    Write-Output 'Formatted C++ sources.'
}
