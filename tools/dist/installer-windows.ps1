# Builds the Windows installer, dist\HS2-Port-<version>-Windows-Setup.exe, for Windows 7 SP1 to 11
# (tools\dist\installer-windows.iss). It packages the stage folder of the Windows build, so run that first (or let CI
# download its zip):
#   powershell -ExecutionPolicy Bypass -File tools\dist\dist-windows.ps1
#   powershell -ExecutionPolicy Bypass -File tools\dist\installer-windows.ps1 [-Iscc C:\path\to\ISCC.exe]
#
# Needs Inno Setup 6 (6.3 or later): ISCC.exe is looked for on the PATH and in the usual install folders.
# Optional: $env:HS2_SIGN_CERT / $env:HS2_SIGN_PASSWORD sign the setup with signtool, as dist-windows.ps1 does.
param([string]$Iscc = '')
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$version = (Select-String -Path "$root\plugin\CMakeLists.txt" -Pattern '^\s*VERSION\s+([0-9.]+)' |
            Select-Object -First 1).Matches[0].Groups[1].Value
$stage = "$root\dist\HS2-Port-$version-Windows"
foreach ($item in 'HS2 Port.exe', 'HS2 Port.vst3') {
    if (-not (Test-Path "$stage\$item")) { throw "$stage\$item is missing: run tools\dist\dist-windows.ps1 first" }
}

if (-not $Iscc) {
    $candidates = @((Get-Command ISCC.exe -ErrorAction SilentlyContinue).Source,
                    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
                    "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
                    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe")
    $Iscc = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (-not $Iscc -or -not (Test-Path $Iscc)) { throw "Inno Setup 6 (ISCC.exe) not found: install it or pass -Iscc" }

Write-Host "== HS2 Port $version installer for Windows ($Iscc)"
& $Iscc /Qp "/DAppVersion=$version" "/DStageDir=$stage" "/DOutputDir=$root\dist" "$PSScriptRoot\installer-windows.iss"
if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed (exit code $LASTEXITCODE)" }
$setup = "$root\dist\HS2-Port-$version-Windows-Setup.exe"

if ($env:HS2_SIGN_CERT) {
    $signtool = Get-Command signtool.exe -ErrorAction Stop
    & $signtool.Source sign /f $env:HS2_SIGN_CERT /p "$env:HS2_SIGN_PASSWORD" /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $setup
    if ($LASTEXITCODE -ne 0) { throw "Signing failed (exit code $LASTEXITCODE)" }
}
Write-Host "== $setup"
