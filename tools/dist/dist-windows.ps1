# Builds the plugin for distribution on Windows and packages it as dist\HS2-Port-<version>-Windows.zip
# (VST3, Standalone exe and an install note). Needs Visual Studio 2022 (or later) with the C++ workload, CMake and
# git. Run from any PowerShell prompt; the Visual Studio x64 environment is set up by the script:
#   powershell -ExecutionPolicy Bypass -File tools\dist\dist-windows.ps1
#
# Optional: $env:HS2_SIGN_CERT = path to a .pfx code signing certificate, $env:HS2_SIGN_PASSWORD = its password.
# The exe and VST3 are then signed with signtool (timestamped).
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$version = (Select-String -Path "$root\plugin\CMakeLists.txt" -Pattern '^\s*VERSION\s+([0-9.]+)' |
            Select-Object -First 1).Matches[0].Groups[1].Value
$build = "$root\build_dist_win"
$name = "HS2-Port-$version-Windows"
$stage = "$root\dist\$name"
$zip = "$root\dist\$name.zip"
$art = "$build\plugin\HS2Port_artefacts\Release"

# Visual Studio x64 build environment (cl, ninja, cmake from vcvars64.bat), unless it is set up already (a developer
# prompt). The environment comes from a small batch file, which avoids cmd's quoting rules for paths with spaces.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "Visual Studio not found (no vswhere.exe)" }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found" }
    $envBat = Join-Path ([System.IO.Path]::GetTempPath()) "hs2-vcvars-$PID.cmd"
    "@call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" >nul`r`n@set" | Set-Content -Encoding ASCII $envBat
    cmd /c $envBat | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
    Remove-Item $envBat
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "vcvars64.bat did not set up the compiler" }
}

Write-Host "== HS2 Port $version for Windows"
function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" } }
if (Get-Command ninja.exe -ErrorAction SilentlyContinue) {
    cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DCMAKE_C_COMPILER=cl -DHS2_BUILD_PLUGIN=ON -DHS2_REQUIRE_ASIO=ON
} else {  # no Ninja: CMake's default, the newest Visual Studio generator (same artefact folders, Release configuration)
    cmake -S $root -B $build -A x64 -DHS2_BUILD_PLUGIN=ON -DHS2_REQUIRE_ASIO=ON
}
Check 'CMake configure'
cmake --build $build --config Release --parallel --target HS2Port_VST3 HS2Port_Standalone
Check 'Build'

if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
if (Test-Path $zip) { Remove-Item -Force $zip }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item -Recurse "$art\VST3\HS2 Port.vst3" "$stage\HS2 Port.vst3"
Copy-Item "$art\Standalone\HS2 Port.exe" "$stage\HS2 Port.exe"

if ($env:HS2_SIGN_CERT) {
    $signtool = Get-Command signtool.exe -ErrorAction Stop
    $files = @("$stage\HS2 Port.exe") + (Get-ChildItem "$stage\HS2 Port.vst3" -Recurse -Filter *.vst3 -File).FullName
    & $signtool.Source sign /f $env:HS2_SIGN_CERT /p "$env:HS2_SIGN_PASSWORD" /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $files
    Check 'Signing'
}

@"
HS2 Port $version for Windows (64-bit)

Copy "HS2 Port.vst3" (the whole folder) to C:\Program Files\Common Files\VST3
"HS2 Port.exe" is the standalone version and can be run from anywhere.

The plugin reads your own Hypersonic 2 content. It finds the folder from the original Hypersonic 2
installation; otherwise it uses C:\Hypersonic 2 Content, or choose the folder with
"Choose content folder..." in the plugin's Settings.
"@ | Set-Content -Encoding UTF8 "$stage\Install.txt"

Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "== $zip"
