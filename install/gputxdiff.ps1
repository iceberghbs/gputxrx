# ============================================================================
#  gputxdiff.ps1 - incremental git-bundle transfer via gputxrx (transmit side)
#
#  Usage: gputxdiff <repo-dir> <branch> [<branch>...] <base-commit> [-- tx flags]
#
#  1. Validates the repo, every branch, and the base commit.
#  2. Creates %TEMP%\gputxrx-diff\<repo>_<b1-b2>_<shorthash>.bundle via
#     `git bundle create <file> <branches> --not <base>`: an incremental
#     bundle containing only what is needed to advance from the base commit.
#     A leading '^' on the base is accepted and stripped; the script always
#     uses --not internally, so cmd.exe's caret-escaping hazard never applies.
#  3. Runs `gputxrx tx` on the bundle, streaming its output live.
#  4. Deletes the local bundle as soon as the identifier banner appears (the
#     banner is printed only after the GPU upload is fence-confirmed). The
#     bundle is kept on any failure or early Ctrl+C.
#
#  Extra gputxrx tx flags (e.g. --max-payload-mb N) may be given after the
#  base commit, optionally after a '--' separator.
# ============================================================================

$ErrorActionPreference = 'Stop'

function Info([string]$msg) { [Console]::Out.WriteLine($msg) }
function Fail([string]$msg, [int]$code) {
    [Console]::Error.WriteLine("gputxdiff: error: $msg")
    exit $code
}
function PrintUsage {
    [Console]::Error.WriteLine(
        'gputxdiff: error: expected <repo-dir>, at least one branch, and a base commit')
    [Console]::Error.WriteLine(
"usage:
  gputxdiff <repo-dir> <branch> [<branch>...] <base-commit> [-- tx flags]
  base-commit : the commit the receiving repo sits on; a leading '^' is ok
  tx flags    : passed to 'gputxrx tx' verbatim (e.g. --max-payload-mb N)")
}

# ---- arguments -------------------------------------------------------------

if ($args.Count -lt 1) { PrintUsage; exit 2 }
$RepoDir = [string]$args[0]

$positionals = @()   # branches..., base commit (last)
$txFlags     = @()   # passed to gputxrx tx verbatim
$flagZone    = $false
for ($i = 1; $i -lt $args.Count; $i++) {
    $a = [string]$args[$i]
    if (-not $flagZone -and $a.Length -gt 0 -and $a.StartsWith('-')) {
        $flagZone = $true
        if ($a -eq '--') { continue }   # explicit separator: not forwarded
    }
    if ($flagZone) { $txFlags += $a } else { $positionals += $a }
}
if ($positionals.Count -lt 2) { PrintUsage; exit 2 }

$branches = @($positionals[0..($positionals.Count - 2)])
$base = ([string]$positionals[$positionals.Count - 1]).TrimStart('^')

# ---- validation ------------------------------------------------------------

if (-not (Test-Path -LiteralPath $RepoDir -PathType Container)) {
    Fail "repo directory not found: $RepoDir" 2
}

$null = & git -C $RepoDir rev-parse --git-dir
if ($LASTEXITCODE -ne 0) { Fail "not a git repository: $RepoDir" 2 }

foreach ($b in $branches) {
    $null = & git -C $RepoDir rev-parse --verify --quiet "$b^{commit}"
    if ($LASTEXITCODE -ne 0) { Fail "unknown branch or revision: $b" 2 }
}
$null = & git -C $RepoDir rev-parse --verify --quiet "$base^{commit}"
if ($LASTEXITCODE -ne 0) { Fail "unknown base commit: $base" 2 }

$short = ([string](& git -C $RepoDir rev-parse --short $base)).Trim()
if ($LASTEXITCODE -ne 0) { Fail "cannot shorten base hash: $base" 2 }

# ---- bundle ----------------------------------------------------------------

$repoResolved = (Resolve-Path -LiteralPath $RepoDir).Path
$repoName = Split-Path -Leaf $repoResolved
if ([string]::IsNullOrWhiteSpace($repoName)) { $repoName = 'repo' }

$nameBranches = (@($branches | ForEach-Object { ([string]$_) -replace '/', '_' }) -join '-')
$bundleDir = Join-Path $env:TEMP 'gputxrx-diff'
$bundleName = "${repoName}_${nameBranches}_${short}.bundle"
$bundlePath = Join-Path $bundleDir $bundleName

$null = New-Item -ItemType Directory -Path $bundleDir -Force
if (Test-Path -LiteralPath $bundlePath) { Remove-Item -LiteralPath $bundlePath -Force }

Info ("gputxdiff: repo '$repoName'  base $short  branches: " + ($branches -join ' '))
Info "gputxdiff: creating $bundlePath"
& git -C $RepoDir bundle create $bundlePath @branches --not $base
if ($LASTEXITCODE -ne 0) { Fail "git bundle create failed" 1 }
Info "gputxdiff: bundle heads:"
& git -C $RepoDir bundle list-heads $bundlePath
if ($LASTEXITCODE -ne 0) { Fail "git bundle list-heads failed" 1 }

# ---- locate gputxrx.exe ----------------------------------------------------

$txCmd = $null
$c = Get-Command gputxrx -ErrorAction SilentlyContinue
if ($c) { $txCmd = $c.Source }
if (-not $txCmd) {
    foreach ($p in @((Join-Path $PSScriptRoot 'gputxrx.exe'),
                     (Join-Path $PSScriptRoot '..\build\Release\gputxrx.exe'))) {
        if ($p -and (Test-Path -LiteralPath $p)) { $txCmd = (Resolve-Path -LiteralPath $p).Path; break }
    }
}
if (-not $txCmd) {
    Fail ('gputxrx.exe not found on PATH, beside this script, or in build\Release. ' +
          'Build the project or run install\install.bat.') 1
}

# ---- transmit --------------------------------------------------------------

Info 'gputxdiff: starting gputxrx tx ...'

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $txCmd
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$argItems = @('tx', $bundlePath) + @($txFlags)
$psi.Arguments = ($argItems | ForEach-Object { '"' + (([string]$_) -replace '"', '\"') + '"' }) -join ' '

$p = New-Object System.Diagnostics.Process
$p.StartInfo = $psi
$null = $p.Start()

$bannerSeen = $false
$notePending = $false   # deletion done; note waiting for the banner block to finish
$note = [string]$null
$newlines = 0
$window = New-Object System.Text.StringBuilder
try {
    $reader = $p.StandardOutput
    while ($true) {
        $i = $reader.Read()
        if ($i -lt 0) { break }
        $ch = [char]$i
        [Console]::Out.Write($ch)
        if ($notePending) {
            # banner line's own \n (#1), the identifier line (#2), the closing
            # '====' line (#3): print our note only after the whole block.
            if ($ch -eq "`n") {
                $newlines++
                if ($newlines -ge 3) {
                    $notePending = $false
                    [Console]::Out.WriteLine()
                    [Console]::Out.WriteLine($note)
                }
            }
        } elseif (-not $bannerSeen) {
            $null = $window.Append($ch)
            if ($window.Length -gt 64) { $null = $window.Remove(0, $window.Length - 64) }
            if ($window.ToString().IndexOf('COPY THIS IDENTIFIER') -ge 0) {
                $bannerSeen = $true
                try {
                    Remove-Item -LiteralPath $bundlePath -Force -ErrorAction Stop
                    $note = "gputxdiff: upload confirmed; local bundle deleted ($bundleName)."
                    $notePending = $true
                    $newlines = 0
                } catch {
                    [Console]::Error.WriteLine("gputxdiff: could not delete $bundlePath : $($_.Exception.Message)")
                }
            }
        }
    }
    if ($notePending) {   # stream ended before the banner block completed
        [Console]::Out.WriteLine()
        [Console]::Out.WriteLine($note)
    }
    # Drain stderr after stdout EOF (tx writes little to stderr; its progress
    # bar self-suppresses when stderr is piped).
    $errTail = $p.StandardError.ReadToEnd()
    if ($errTail) { [Console]::Error.Write($errTail) }
    $p.WaitForExit()
} finally {
    if (-not $p.HasExited) { try { $p.Kill() } catch { } }
}

if ($bannerSeen) { exit 0 }   # transfer confirmed; tx was stopped afterwards
$code = $p.ExitCode
if ($code -ne 0) { Fail ("gputxrx tx failed (exit $code); bundle kept at: $bundlePath") 1 }
Fail ('gputxrx tx ended before confirming the upload; bundle kept at: ' + $bundlePath) 1
