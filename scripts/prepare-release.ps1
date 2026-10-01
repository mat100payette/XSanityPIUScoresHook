[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('patch', 'minor', 'major')]
    [string]$Bump
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    function Invoke-Git {
        & git @args
        if ($LASTEXITCODE -ne 0) { throw "git $($args[0]) failed." }
    }

    if ($env:GITHUB_REF -ne 'refs/heads/main') {
        throw 'Run the release workflow from main.'
    }
    $commit = Invoke-Git rev-parse HEAD
    if ($commit -ne $env:GITHUB_SHA) { throw 'Checkout does not match the workflow commit.' }
    if (Invoke-Git status --porcelain --untracked-files=no) { throw 'Release source must be unchanged.' }

    $versions = @(Invoke-Git tag --list 'v*' | ForEach-Object {
        if ($_ -match '^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') {
            [version]$_.Substring(1)
        }
    })
    if ($versions.Count -eq 0) {
        $version = (Get-Content -LiteralPath VERSION -Raw).Trim()
    } else {
        $base = $versions | Sort-Object -Descending | Select-Object -First 1
        $version = $base.ToString()
        # A failed publication may already have pushed its tag. Retry that release.
        if ((Invoke-Git rev-parse "refs/tags/v$version^{commit}") -ne $commit) {
            $major, $minor, $patch = $base.Major, $base.Minor, $base.Build
            switch ($Bump) {
                'major' { $major++; $minor = 0; $patch = 0 }
                'minor' { $minor++; $patch = 0 }
                'patch' { $patch++ }
            }
            $version = "$major.$minor.$patch"
        }
    }
    if ($version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$' -or
            @($version.Split('.') | Where-Object { [long]$_ -gt 65534 }).Count) {
        throw 'Release versions must have three numeric parts, each 0-65534.'
    }

    "version=$version", "tag=v$version", "commit=$commit" |
        Out-File -FilePath $env:GITHUB_OUTPUT -Append -Encoding utf8
    Write-Host "Releasing v$version from $commit."
} finally {
    Pop-Location
}
