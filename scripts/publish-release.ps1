[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidatePattern('^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$')]
    [string]$Tag,
    [Parameter(Mandatory)]
    [string]$Branch
)

$ErrorActionPreference = 'Stop'
Push-Location (Split-Path -Parent $PSScriptRoot)
try {
    git push --atomic origin "HEAD:refs/heads/$Branch" "refs/tags/$Tag"
    if ($LASTEXITCODE -ne 0) { throw 'Could not push the release commit and tag.' }

    $installer = "artifacts/XSanityPIUScoresHook-$Tag-win-x64-setup.exe"
    $assets = @($installer, "$installer.sha256")
    foreach ($asset in $assets) {
        if (-not (Test-Path -LiteralPath $asset -PathType Leaf)) {
            throw "Release asset is missing: $asset"
        }
    }

    $releases = gh release list --limit 100 --json tagName,isDraft
    if ($LASTEXITCODE -ne 0) { throw 'Could not check existing releases.' }
    $existing = $releases | ConvertFrom-Json | Where-Object { $_.tagName -eq $Tag }
    $title = "XSanity PIU Scores Hook $Tag"
    if ($existing) {
        if (-not $existing.isDraft) { throw "$Tag is already published." }
        gh release upload $Tag @assets --clobber
        if ($LASTEXITCODE -ne 0) { throw 'Could not upload the release assets.' }
        gh release edit $Tag --draft=false --verify-tag --latest --title $title
    } else {
        gh release create $Tag @assets --verify-tag --latest --generate-notes --title $title
    }
    if ($LASTEXITCODE -ne 0) { throw 'Release publication failed.' }

    $url = gh release view $Tag --json url --jq .url
    if ($LASTEXITCODE -ne 0) { throw 'Could not get the published release URL.' }
    "Published [$Tag]($url) with the installer and SHA-256 checksum." |
        Out-File -FilePath $env:GITHUB_STEP_SUMMARY -Append
} finally {
    Pop-Location
}
