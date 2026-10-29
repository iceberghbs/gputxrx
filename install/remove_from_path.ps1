# Removes a directory from the user PATH (idempotent).
# Called by uninstall.bat; can also be run standalone:
#   powershell -NoProfile -ExecutionPolicy Bypass -File remove_from_path.ps1 <directory>
param(
    [Parameter(Mandatory = $true)]
    [string]$Directory
)

$ErrorActionPreference = 'Stop'

$dir = (Resolve-Path -LiteralPath $Directory).Path.TrimEnd('\')

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($null -eq $userPath) { $userPath = '' }

$parts = @($userPath -split ';' | Where-Object { $_ -ne '' })

$kept = @($parts | Where-Object { $_ -ne $dir })

if ($kept.Count -eq $parts.Count) {
    Write-Host "[gputxrx] '$dir' was not on the user PATH - nothing to do."
    exit 0
}

[Environment]::SetEnvironmentVariable('Path', ($kept -join ';'), 'User')
Write-Host "[gputxrx] removed '$dir' from the user PATH."
exit 0
