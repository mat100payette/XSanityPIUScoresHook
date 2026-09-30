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

    if ($env:GITHUB_REF_TYPE -ne 'branch') {
        throw 'Run the release workflow from a branch.'
    }

    $versions = @(Invoke-Git tag --list 'v*' | ForEach-Object {
        if ($_ -match '^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$') {
            [version]$_.Substring(1)
        }
    })
    if ($versions.Count -eq 0) {
        $version = (Get-Content -LiteralPath VERSION -Raw).Trim()
    } else {
        $base = $versions | Sort-Object -Descending | Select-Object -First 1
        $major, $minor, $patch = $base.Major, $base.Minor, $base.Build
        switch ($Bump) {
            'major' { $major++; $minor = 0; $patch = 0 }
            'minor' { $minor++; $patch = 0 }
            'patch' { $patch++ }
        }
        $version = "$major.$minor.$patch"
    }
    if ($version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$' -or
            @($version.Split('.') | Where-Object { [long]$_ -gt 65534 }).Count) {
        throw 'Release versions must have three numeric parts, each 0-65534.'
    }
    $tag = "v$version"

    [System.IO.File]::WriteAllText((Join-Path $PWD 'VERSION'), "$version`n", [System.Text.UTF8Encoding]::new($false))
    Invoke-Git config user.name 'github-actions[bot]'
    Invoke-Git config user.email '41898282+github-actions[bot]@users.noreply.github.com'
    Invoke-Git add VERSION
    if (Invoke-Git diff --cached --name-only) {
        Invoke-Git commit -m "Release $tag"
    }
    Invoke-Git tag -a $tag -m "XSanity PIU Scores Hook $tag"
    "tag=$tag" | Out-File -FilePath $env:GITHUB_OUTPUT -Append
    Write-Host "Releasing $tag."
} finally {
    Pop-Location
}
