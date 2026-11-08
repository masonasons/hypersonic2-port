# Render the patches listed in tools\reaper\job.tsv (see make_job.py) with Hypersonic 2 in REAPER.
# Uses ONLY the portable render copy in C:\programs\reaper-render. It was extracted from the installer with
# 7-Zip and never installed, so nothing is registered. Its own reaper.ini sets projrenderlimit=1 (1x render
# speed, because Hypersonic streams samples in real time). The user's own REAPER (C:\Program Files\REAPER,
# %APPDATA%\REAPER) must never be launched or configured by this script.
# Never pass -cfgfile: REAPER then treats that folder as its whole resource path.
param([int]$TimeoutMinutes = 600)
$ErrorActionPreference = 'Stop'
$reaper = 'C:\programs\reaper-render\reaper.exe'
if (-not (Test-Path 'C:\programs\reaper-render\reaper.ini')) { throw "render copy missing its portable reaper.ini" }
if ($reaper -like '*Program Files*') { throw "refusing to launch the user's REAPER" }

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out = ((Get-Content "$here\job.tsv" -TotalCount 1) -split "`t")[1]
New-Item -ItemType Directory -Force $out | Out-Null
Remove-Item "$out\done.txt" -ErrorAction SilentlyContinue

$p = Start-Process $reaper -PassThru -ArgumentList @('-newinst', '-nosplash', '-new', "`"$here\hs2_render.lua`"")
$deadline = (Get-Date).AddMinutes($TimeoutMinutes)
while (-not (Test-Path "$out\done.txt") -and -not $p.HasExited -and (Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 2
}
if (-not $p.WaitForExit(15000)) { Stop-Process -Id $p.Id -Force }
if (Test-Path "$out\done.txt") { Get-Content "$out\done.txt" } else { Write-Error "render did not finish (no done.txt)" }
