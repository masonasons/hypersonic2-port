# Capture all patches of a list with hs2capture, restarting the tool (a fresh plugin process) whenever the
# original's loader stops loading copy-protected patches (it does after a while - a dozen or so). hs2capture
# skips patches already captured and stops after --max-fails consecutive failed loads, so each pass makes
# progress and the next pass continues fresh.
#
#   .\capture_all.ps1 -List protected_patches.tsv -Out C:\hs2capture [-Plugin "C:\...\Hypersonic.dll"] [-Mode patch]
param(
    [Parameter(Mandatory = $true)] [string] $List,
    [Parameter(Mandatory = $true)] [string] $Out,
    [string] $Plugin = "",
    [string] $Mode = "patch"
)

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$exe = Join-Path $here "hs2capture.exe"
if ($Plugin -ne "") { $exeArgs = @("--plugin", $Plugin) } else { $exeArgs = @() }

$total = (Get-Content $List | Where-Object { $_ -match '^\d' }).Count
New-Item -ItemType Directory -Force -Path (Join-Path $Out "patches") | Out-Null

for ($pass = 1; $pass -le 60; ++$pass) {
    $have = (Get-ChildItem (Join-Path $Out "patches") -Filter *.stss -ErrorAction SilentlyContinue).Count
    if ($have -ge $total) { Write-Host "all $total patches captured."; break }
    Write-Host ("pass {0}: {1} of {2} patches there, running hs2capture..." -f $pass, $have, $total)
    # Short settle and an early stop after two failed loads: the original stops loading copy-protected
    # patches after a while, and a fresh process continues; no point waiting the full settle on failures.
    & $exe @exeArgs --list $List --out $Out --mode $Mode --settle 10 --max-fails 2 | Out-Host
    $exit = $LASTEXITCODE
    $now = (Get-ChildItem (Join-Path $Out "patches") -Filter *.stss -ErrorAction SilentlyContinue).Count
    Write-Host ("pass {0}: hs2capture exit {1}, {2} of {3} patches now there" -f $pass, $exit, $now, $total)
    if ($now -eq $have) {
        Write-Host "no progress this pass; stopping (check results.tsv)."
        break
    }
}
