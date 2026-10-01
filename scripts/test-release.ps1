[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetTempPath()) ('piu-release-' + [guid]::NewGuid().ToString('N'))
$git = (Get-Command git -CommandType Application | Select-Object -First 1).Source
$savedEnvironment = @{}
foreach ($key in @('GITHUB_REF', 'GITHUB_SHA', 'GITHUB_OUTPUT', 'GITHUB_STEP_SUMMARY',
        'GIT_CONFIG_GLOBAL', 'GIT_CONFIG_NOSYSTEM', 'GIT_DEFAULT_HASH')) {
    $savedEnvironment[$key] = [Environment]::GetEnvironmentVariable($key)
}
$savedExit = $global:LASTEXITCODE
$script:passed = 0
$ghState = [pscustomobject]@{
    Calls = [Collections.Generic.List[string]]::new()
    ReleaseState = 'absent'
    FailUpload = $false
    AssetsReady = $false
}

function Check([bool]$condition, [string]$label) {
    if (-not $condition) { throw "FAILED: $label" }
    $script:passed++
}
function Rejects([scriptblock]$action, [string]$message) {
    try { & $action } catch {
        Check ($_.Exception.Message -like "*$message*") "Expected failure: $message; received: $($_.Exception.Message)"
        return
    }
    throw "FAILED: expected $message"
}
function FixtureGit {
    & $git @args
    if ($LASTEXITCODE -ne 0) { throw "Fixture git $($args[0]) failed." }
}
function Write-Text([string]$path, [string]$text) {
    [IO.File]::WriteAllText($path, $text, [Text.UTF8Encoding]::new($false))
}
# Never contacts GitHub. Git itself uses only the temporary local bare remote.
Set-Item Function:gh -Value {
    $ghState.Calls.Add(($args -join ' '))
    $global:LASTEXITCODE = 0
    switch ($args[1]) {
        'list' {
            # A PowerShell function mock preserves nested arrays; an executable expands them.
            # Git's argument echo exercises that native boundary without contacting GitHub.
            $nativeArgs = & $git rev-parse --sq-quote @args
            if ($LASTEXITCODE -ne 0 -or $nativeArgs.Trim() -cne
                    "'release' 'list' '--limit' '100' '--json' 'tagName,isDraft'") {
                throw 'Release list must pass --json fields as one native argument.'
            }
            if ($ghState.ReleaseState -eq 'absent') { return '[]' }
            return (@{ tagName = 'v0.5.5'; isDraft = ($ghState.ReleaseState -eq 'draft') } | ConvertTo-Json -Compress)
        }
        'create' {
            if ($args -notcontains '--draft' -or $args -notcontains '--verify-tag') {
                throw 'Release must start as a draft against a verified tag.'
            }
            $ghState.ReleaseState = 'draft'
        }
        'upload' {
            if ($ghState.ReleaseState -ne 'draft') { throw 'Assets must upload to a draft.' }
            $assets = @($args | Where-Object { $_ -like 'artifacts/*' })
            if ($assets.Count -ne 2 -or $assets[1] -ne ($assets[0] + '.sha256')) {
                throw 'Release must upload the installer and its checksum together.'
            }
            if ($ghState.FailUpload) { $global:LASTEXITCODE = 1; return }
            $ghState.AssetsReady = $true
        }
        'edit' {
            if (-not $ghState.AssetsReady -or $args -notcontains '--draft=false' -or
                    $args -notcontains '--verify-tag') {
                throw 'Publish only after both assets are uploaded and the tag is verified.'
            }
            $ghState.ReleaseState = 'published'
        }
        'view' { return 'https://example.invalid/releases/v0.5.5' }
        default { throw 'Unexpected fake GitHub command.' }
    }
}.GetNewClosure()
function Resolve-Version([string]$bump) {
    Write-Text $env:GITHUB_OUTPUT ''
    & .\scripts\prepare-release.ps1 -Bump $bump
    return (Get-Content -LiteralPath $env:GITHUB_OUTPUT | ConvertFrom-StringData)
}
function Write-Assets([string]$tag) {
    $asset = Join-Path $PWD "artifacts/XSanityPIUScoresHook-$tag-win-x64-setup.exe"
    Write-Text $asset 'isolated installer fixture'
    $hash = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()
    Write-Text "$asset.sha256" "$hash  $(Split-Path -Leaf $asset)`n"
}

New-Item -ItemType Directory -Path "$fixture/work/scripts", "$fixture/work/artifacts" -Force | Out-Null
Push-Location "$fixture/work"
try {
    # Personal signing settings, hooks and Git aliases must not influence fixtures.
    $env:GIT_CONFIG_GLOBAL = Join-Path $fixture 'empty.gitconfig'
    Write-Text $env:GIT_CONFIG_GLOBAL ''
    $env:GIT_CONFIG_NOSYSTEM = '1'
    $env:GIT_DEFAULT_HASH = 'sha1'
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'prepare-release.ps1'), (Join-Path $PSScriptRoot 'publish-release.ps1') -Destination scripts
    Write-Text (Join-Path $PWD 'VERSION') "0.5.4`n"
    Write-Text (Join-Path $PWD '.gitignore') "artifacts/`n"
    FixtureGit init --quiet --initial-branch=main
    FixtureGit config user.name 'Release test'
    FixtureGit config user.email 'test@example.invalid'
    FixtureGit config commit.gpgsign false
    FixtureGit config tag.gpgsign false
    FixtureGit config core.autocrlf false
    FixtureGit add .
    FixtureGit commit --quiet -m Initial
    $initial = FixtureGit rev-parse HEAD
    $env:GITHUB_REF = 'refs/heads/main'
    $env:GITHUB_SHA = $initial
    $env:GITHUB_OUTPUT = Join-Path $fixture 'output.txt'
    $env:GITHUB_STEP_SUMMARY = Join-Path $fixture 'summary.txt'
    $first = Resolve-Version patch
    Check ($first.version -eq '0.5.4' -and $first.tag -eq 'v0.5.4' -and $first.commit -eq $initial) 'first release uses fallback and exact commit'
    Check (-not (FixtureGit tag --list)) 'preparation creates no tags'
    FixtureGit tag v0.5.4
    FixtureGit commit --quiet --allow-empty -m 'Reviewed changes'
    $commit = FixtureGit rev-parse HEAD
    $env:GITHUB_SHA = $commit
    Check ((Resolve-Version patch).version -eq '0.5.5') 'patch increment'
    Check ((Resolve-Version minor).version -eq '0.6.0') 'minor increment'
    Check ((Resolve-Version major).version -eq '1.0.0') 'major increment'
    Check ((FixtureGit rev-parse HEAD) -eq $commit -and -not (FixtureGit status --porcelain)) 'preparation leaves commit and source untouched'
    $env:GITHUB_REF = 'refs/heads/feature'
    Rejects { Resolve-Version patch } 'from main'
    $env:GITHUB_REF = 'refs/heads/main'
    $env:GITHUB_SHA = $initial
    Rejects { Resolve-Version patch } 'workflow commit'
    $env:GITHUB_SHA = $commit
    Write-Text (Join-Path $PWD 'VERSION') "0.5.3`n"
    Rejects { Resolve-Version patch } 'unchanged'
    Write-Text (Join-Path $PWD 'VERSION') "0.5.4`n"
    FixtureGit tag v65534.65534.65534 $initial
    Rejects { Resolve-Version patch } '0-65534'
    FixtureGit tag --delete v65534.65534.65534 | Out-Null

    FixtureGit init --quiet --bare "$fixture/remote.git"
    FixtureGit remote add origin "$fixture/remote.git"
    FixtureGit push --quiet origin main
    # Emulate main's PR-only rule: tags may be created, branches may not change.
    $hook = "#!/bin/sh`ncase `"`$1`" in refs/heads/*) echo 'branch updates require a PR' >&2; exit 1;; esac`nexit 0`n"
    Write-Text "$fixture/remote.git/hooks/update" $hook
    $publish = '.\scripts\publish-release.ps1'
    Rejects { & $publish -Tag v0.5.5 -Commit $commit } 'asset is missing'
    Check (-not (FixtureGit tag --list v0.5.5) -and $ghState.Calls.Count -eq 0) 'missing assets cause no remote changes'
    Write-Assets v0.5.5
    Write-Text (Join-Path $PWD 'artifacts/XSanityPIUScoresHook-v0.5.5-win-x64-setup.exe.sha256') 'wrong checksum'
    Rejects { & $publish -Tag v0.5.5 -Commit $commit } 'checksum'
    Write-Assets v0.5.5
    Rejects { & $publish -Tag v0.5.5 -Commit $initial } 'selected by this workflow'
    FixtureGit tag v0.5.5 $initial
    Rejects { & $publish -Tag v0.5.5 -Commit $commit } 'different commit'
    FixtureGit tag --delete v0.5.5 | Out-Null

    $ghState.FailUpload = $true
    Rejects { & $publish -Tag v0.5.5 -Commit $commit } 'release upload failed'
    Check ($ghState.ReleaseState -eq 'draft') 'failed asset upload leaves a draft'
    Check ((FixtureGit --git-dir="$fixture/remote.git" rev-parse 'refs/tags/v0.5.5^{commit}') -eq $commit) 'tag points to tested commit'
    Check ((FixtureGit --git-dir="$fixture/remote.git" rev-parse refs/heads/main) -eq $commit) 'release never changes main'
    Check ((Resolve-Version patch).version -eq '0.5.5') 'full rerun retains failed release version'
    $ghState.FailUpload = $false
    & $publish -Tag v0.5.5 -Commit $commit
    Check ($ghState.ReleaseState -eq 'published') 'retry publishes existing draft'
    Check (@($ghState.Calls | Where-Object { $_ -like 'release create*' }).Count -eq 1) 'retry does not create a second release'
    $callCount = $ghState.Calls.Count
    Rejects { & $publish -Tag v0.5.5 -Commit $commit } 'already published'
    Check ($ghState.Calls.Count -eq $callCount + 1) 'published assets cannot be overwritten'
    Check ((Get-Content VERSION -Raw).Trim() -eq '0.5.4' -and -not (FixtureGit status --porcelain)) 'publication leaves VERSION and working tree unchanged'
    FixtureGit commit --quiet --allow-empty -m 'Unreviewed fixture change'
    & $git push --quiet origin main 2>&1 | Out-Null
    Check ($LASTEXITCODE -ne 0) 'test remote actually rejects branch updates'
    Check ((FixtureGit --git-dir="$fixture/remote.git" rev-parse refs/heads/main) -eq $commit) 'protected main remains unchanged'
    Write-Host "$script:passed release checks passed. Local Git remote and fake GitHub only."
} finally {
    Pop-Location
    foreach ($key in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($key, $savedEnvironment[$key])
    }
    $global:LASTEXITCODE = $savedExit
    # Delete only this run's verified temporary fixture directory.
    $resolved = [IO.Path]::GetFullPath($fixture)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            (Split-Path -Leaf $resolved) -notmatch '^piu-release-[0-9a-f]{32}$') {
        throw 'Unexpected release fixture cleanup path.'
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
