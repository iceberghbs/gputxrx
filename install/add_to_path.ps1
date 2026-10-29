# Adds a directory to the user PATH (idempotent).
# Called by install.bat; can also be run standalone:
#   powershell -NoProfile -ExecutionPolicy Bypass -File add_to_path.ps1 <directory>
param(
    [Parameter(Mandatory = $true)]
    [string]$Directory
)

$ErrorActionPreference = 'Stop'

$dir = (Resolve-Path -LiteralPath $Directory).Path.TrimEnd('\')

$userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($null -eq $userPath) { $userPath = '' }

$parts = @($userPath -split ';' | Where-Object { $_ -ne '' })

if ($parts -contains $dir) {
    Write-Host "[gputxrx] '$dir' is already on the user PATH - nothing to do."
    exit 0
}

$parts += $dir
# .NET broadcasts WM_SETTINGCHANGE, so newly opened terminals pick this up.
[Environment]::SetEnvironmentVariable('Path', ($parts -join ';'), 'User')

Write-Host "[gputxrx] added '$dir' to the user PATH."
exit 0
