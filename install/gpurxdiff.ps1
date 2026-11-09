# ============================================================================
#  gpurxdiff.ps1 - receive side of gputxdiff (git bundle via gputxrx rx)
#
#  Usage: gpurxdiff <repo-dir> <identifier>
#
#  1. Runs `gputxrx rx <identifier>` into a fresh staging directory
#     %TEMP%\gputxrx-diff\rx-<timestamp>\ (the transmitter must still be
#     alive; the identifier is the one gputxdiff printed).
#  2. `git bundle verify` - fails loudly if this repo does not contain the
#     base commit the bundle was cut against.
#  3. `git fetch <bundle> +<ref>:refs/remotes/origin/<branch>` for every
#     branch in the bundle, then `git merge --ff-only` of the current branch
#     if it is one of them. Remote-tracking refs stay truthful, so
#     'git status' does not report a bogus "ahead of origin".
#  4. Removes the staging directory on success; keeps it on failure.
# ============================================================================

$ErrorActionPreference = 'Stop'

function Info([string]$msg) { [Console]::Out.WriteLine($msg) }
function Fail([string]$msg, [int]$code) {
    [Console]::Error.WriteLine("gpurxdiff: error: $msg")
    exit $code
}

if ($args.Count -ne 2) {
    [Console]::Error.WriteLine('gpurxdiff: error: expected <repo-dir> and <identifier>')
    [Console]::Error.WriteLine('usage: gpurxdiff <repo-dir> <identifier>')
    exit 2
}
$RepoDir    = [string]$args[0]
$Identifier = [string]$args[1]

if (-not (Test-Path -LiteralPath $RepoDir -PathType Container)) {
    Fail "repo directory not found: $RepoDir" 2
}
$null = & git -C $RepoDir rev-parse --git-dir
if ($LASTEXITCODE -ne 0) { Fail "not a git repository: $RepoDir" 2 }

# ---- locate gputxrx.exe (same lookup order as gputxdiff) -------------------

$rxCmd = $null
$c = Get-Command gputxrx -ErrorAction SilentlyContinue
if ($c) { $rxCmd = $c.Source }
if (-not $rxCmd) {
    foreach ($p in @((Join-Path $PSScriptRoot 'gputxrx.exe'),
                     (Join-Path $PSScriptRoot '..\build\Release\gputxrx.exe'))) {
        if ($p -and (Test-Path -LiteralPath $p)) { $rxCmd = (Resolve-Path -LiteralPath $p).Path; break }
    }
}
if (-not $rxCmd) {
    Fail ('gputxrx.exe not found on PATH, beside this script, or in build\Release. ' +
          'Build the project or run install\install.bat.') 1
}

# ---- receive the bundle ----------------------------------------------------

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$rxDir = Join-Path $env:TEMP "gputxrx-diff\rx-$stamp"

Info "gpurxdiff: receiving into $rxDir"
& $rxCmd rx $Identifier $rxDir
if ($LASTEXITCODE -ne 0) {
    Fail ("gputxrx rx failed (is the transmitter still alive?); staging kept at: $rxDir") $LASTEXITCODE
}

$bundles = @(Get-ChildItem -LiteralPath $rxDir -Filter '*.bundle' -File -ErrorAction SilentlyContinue)
if ($bundles.Count -ne 1) {
    Fail ("expected exactly one .bundle file in $rxDir, found $($bundles.Count); staging kept") 1
}
$bundlePath = $bundles[0].FullName
Info "gpurxdiff: received $($bundles[0].Name)"

# ---- integrate into the repo ----------------------------------------------

& git -C $RepoDir bundle verify $bundlePath
if ($LASTEXITCODE -ne 0) {
    Fail ('bundle verify failed: this repo does not contain the base commit the ' +
          "bundle was cut against; staging kept at: $rxDir") 1
}

$heads = @(& git -C $RepoDir bundle list-heads $bundlePath)
if ($LASTEXITCODE -ne 0) { Fail 'git bundle list-heads failed' 1 }

$refMap = @{}   # advertised ref -> branch name
foreach ($line in $heads) {
    if ($line -match '^([0-9a-fA-F]{40,64})\s+(\S+)$') {
        $ref = $Matches[2]
        $branch = if ($ref.StartsWith('refs/heads/')) { $ref.Substring('refs/heads/'.Length) } else { $ref }
        $refMap[$ref] = $branch
    }
}
if ($refMap.Count -lt 1) {
    Fail "bundle carries no branch refs; staging kept at: $rxDir" 1
}

$refspecs = @()
foreach ($ref in $refMap.Keys) {
    $refspecs += "+$($ref):refs/remotes/origin/$($refMap[$ref])"
}
& git -C $RepoDir fetch $bundlePath @refspecs
if ($LASTEXITCODE -ne 0) {
    Fail ("git fetch from bundle failed; staging kept at: $rxDir") $LASTEXITCODE
}

$current = ([string](& git -C $RepoDir rev-parse --abbrev-ref HEAD)).Trim()
$updated = @($refMap.Values | Sort-Object -Unique)

if ($current -eq 'HEAD') {
    Info ('gpurxdiff: detached HEAD; tracking refs updated only: ' +
          (($updated | ForEach-Object { "origin/$_" }) -join ', '))
} elseif ($updated -contains $current) {
    & git -C $RepoDir merge --ff-only "refs/remotes/origin/$current"
    if ($LASTEXITCODE -ne 0) {
        Fail ("fast-forward of '$current' refused (local history diverged?); " +
              "tracking refs were updated; staging kept at: $rxDir") 1
    }
    Info "gpurxdiff: branch '$current' fast-forwarded to origin/$current"
} else {
    Info "gpurxdiff: current branch '$current' is not in the bundle; tracking refs updated only."
}

$others = @($updated | Where-Object { $_ -ne $current })
if ($others.Count -gt 0) {
    Info ('gpurxdiff: also updated: ' + (($others | ForEach-Object { "origin/$_" }) -join ', ') +
          "  ('git checkout <branch>' to pick them up)")
}

# ---- clean up --------------------------------------------------------------

try {
    Remove-Item -LiteralPath $rxDir -Recurse -Force -ErrorAction Stop
    Info 'gpurxdiff: done; staging removed.'
} catch {
    Info "gpurxdiff: done; could not fully remove $rxDir ($($_.Exception.Message))"
}
exit 0
