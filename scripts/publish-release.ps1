[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$')]
    [string]$Tag,
    [Parameter(Mandatory)]
    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$Commit
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    function Invoke-Git {
        & git @args
        if ($LASTEXITCODE -ne 0) { throw "git $($args[0]) failed." }
    }
    function Invoke-Gh {
        & gh @args
        if ($LASTEXITCODE -ne 0) { throw "gh $($args[0]) $($args[1]) failed." }
    }

    if ($env:GITHUB_REF -ne 'refs/heads/main' -or $env:GITHUB_SHA -ne $Commit) {
        throw 'Publish only the main commit selected by this workflow.'
    }
    if ((Invoke-Git rev-parse HEAD) -ne $Commit) { throw 'Checkout differs from the tested commit.' }
    if (Invoke-Git status --porcelain --untracked-files=no) { throw 'Release source must be unchanged.' }
    Invoke-Git merge-base --is-ancestor $Commit refs/remotes/origin/main

    $installer = "artifacts/XSanityPIUScoresHook-$Tag-win-x64-setup.exe"
    $assets = @($installer, "$installer.sha256")
    foreach ($asset in $assets) {
        if (-not (Test-Path -LiteralPath $asset -PathType Leaf)) {
            throw "Release asset is missing: $asset"
        }
    }
    $hash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    $expected = "$hash  $(Split-Path -Leaf $installer)"
    if ((Get-Content -LiteralPath "$installer.sha256" -Raw).Trim() -cne $expected) {
        throw 'Release installer checksum does not match.'
    }

    $releases = Invoke-Gh release list --limit 100 --json tagName,isDraft
    $existing = $releases | ConvertFrom-Json | Where-Object { $_.tagName -eq $Tag }
    if ($existing -and -not $existing.isDraft) { throw "$Tag is already published; it will not be overwritten." }

    $localTag = Invoke-Git tag --list $Tag
    if ($localTag) {
        if ((Invoke-Git rev-parse "refs/tags/$Tag^{commit}") -ne $Commit) {
            throw "$Tag points to a different commit; it will not be moved."
        }
    } else {
        Invoke-Git -c user.name=github-actions[bot] -c user.email=41898282+github-actions[bot]@users.noreply.github.com tag -a $Tag $Commit -m "XSanity PIU Scores Hook $Tag"
    }
    Invoke-Git push origin "refs/tags/${Tag}:refs/tags/$Tag"

    $title = "XSanity PIU Scores Hook $Tag"
    if (-not $existing) {
        Invoke-Gh release create $Tag --draft --verify-tag --generate-notes --title $title
    }
    Invoke-Gh release upload $Tag @assets --clobber
    Invoke-Gh release edit $Tag --draft=false --verify-tag --latest --title $title

    $url = Invoke-Gh release view $Tag --json url --jq .url
    "Published [$Tag]($url) from commit $Commit with the installer and SHA-256 checksum." |
        Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Append
} finally {
    Pop-Location
}
