# Builds the plugin for distribution on Windows and packages it as dist\HS2-Port-<version>-Windows.zip
# (VST3, Standalone exe and an install note). Needs Visual Studio 2022 (or later) with the C++ workload, CMake and
# git. Run from any PowerShell prompt; the Visual Studio x64 environment is set up by the script:
#   powershell -ExecutionPolicy Bypass -File tools\dist\dist-windows.ps1
#
# -Windows7: the Windows 7 build instead, dist\HS2-Port-<version>-Windows7.zip: the plugin against JUCE 7.0.12 with the
# C++ runtime linked in (CMake HS2_JUCE7), checked to import nothing newer than Windows 7
# (tools\dist\check_win7_imports.py). The normal build needs Windows 10 (JUCE 9). It needs the Visual Studio 2022 C++
# toolset (MSVC 14.4x, "v143"): the C++ library of Visual Studio 2026 (MSVC 14.5x) calls Windows 8 functions
# (CreateFile2). Any Visual Studio install that has the 14.4x toolset will do; the script finds it.
#
# Optional: $env:HS2_SIGN_CERT = path to a .pfx code signing certificate, $env:HS2_SIGN_PASSWORD = its password.
# The exe and VST3 are then signed with signtool (timestamped).
param([switch]$Windows7)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$version = (Select-String -Path "$root\plugin\CMakeLists.txt" -Pattern '^\s*VERSION\s+([0-9.]+)' |
            Select-Object -First 1).Matches[0].Groups[1].Value
$build = if ($Windows7) { "$root\build_dist_win7" } else { "$root\build_dist_win" }
$name = if ($Windows7) { "HS2-Port-$version-Windows7" } else { "HS2-Port-$version-Windows" }
$juce7 = if ($Windows7) { 'ON' } else { 'OFF' }
$stage = "$root\dist\$name"
$zip = "$root\dist\$name.zip"
$art = "$build\plugin\HS2Port_artefacts\Release"

# Visual Studio x64 build environment (cl, ninja, cmake from vcvars64.bat), unless it is set up already (a developer
# prompt). The environment comes from a small batch file, which avoids cmd's quoting rules for paths with spaces.
# The Windows 7 build always sets up a 14.4x toolset (a compiler already on the PATH may be a newer one).
$toolset = ''
if ($Windows7 -or -not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { throw "Visual Studio not found (no vswhere.exe)" }
    if ($Windows7) {
        $vs = $null
        foreach ($inst in & $vswhere -all -products * -property installationPath) {
            $t = Get-ChildItem "$inst\VC\Tools\MSVC" -Directory -ErrorAction SilentlyContinue |
                 Where-Object { $_.Name -like '14.4*' } | Sort-Object Name -Descending | Select-Object -First 1
            if ($t) { $vs = $inst; $toolset = $t.Name; break }
        }
        if (-not $vs) {
            throw ("The Windows 7 build needs the Visual Studio 2022 C++ toolset (MSVC 14.4x): install " +
                   "'MSVC v143 - VS 2022 C++ x64/x86 build tools' in the Visual Studio Installer")
        }
        Write-Host "== C++ toolset $toolset ($vs)"
        $vcvarsArgs = "-vcvars_ver=$($toolset.Split('.')[0..1] -join '.')"
    } else {
        $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if (-not $vs) { throw "Visual Studio with the C++ x64 tools not found" }
        $vcvarsArgs = ''
    }
    $envBat = Join-Path ([System.IO.Path]::GetTempPath()) "hs2-vcvars-$PID.cmd"
    "@call `"$vs\VC\Auxiliary\Build\vcvars64.bat`" $vcvarsArgs >nul`r`n@set" | Set-Content -Encoding ASCII $envBat
    cmd /c $envBat | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
    Remove-Item $envBat
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "vcvars64.bat did not set up the compiler" }
}

Write-Host "== HS2 Port $version for Windows$(if ($Windows7) { ' 7' })"
function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" } }
# CMake keeps the compiler of the first configure: a build folder made with another toolset starts over.
if ($toolset -and (Test-Path "$build\CMakeCache.txt") -and
    -not (Select-String -Path "$build\CMakeCache.txt" -SimpleMatch "MSVC/$toolset/" -Quiet)) {
    Write-Host "== $build was configured with another compiler: starting it over"
    Remove-Item -Recurse -Force $build
}
if (Get-Command ninja.exe -ErrorAction SilentlyContinue) {
    cmake -S $root -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -DCMAKE_C_COMPILER=cl -DHS2_BUILD_PLUGIN=ON -DHS2_REQUIRE_ASIO=ON "-DHS2_JUCE7=$juce7"
} else {  # no Ninja: CMake's default, the newest Visual Studio generator (same artefact folders, Release configuration)
    cmake -S $root -B $build -A x64 -DHS2_BUILD_PLUGIN=ON -DHS2_REQUIRE_ASIO=ON "-DHS2_JUCE7=$juce7"
}
Check 'CMake configure'
cmake --build $build --config Release --parallel --target HS2Port_VST3 HS2Port_Standalone
Check 'Build'
if ($Windows7) {  # nothing newer than Windows 7 may be imported
    $python = Get-Command python.exe, py.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $python) { throw "Python is needed for the Windows 7 import check" }
    & $python.Source -I "$root\tools\dist\check_win7_imports.py" "$art\Standalone\HS2 Port.exe" `
        "$art\VST3\HS2 Port.vst3\Contents\x86_64-win\HS2 Port.vst3"
    Check 'Windows 7 import check'
}

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

$systems = if ($Windows7) {
    "This is the build for Windows 7, 8 and 8.1 (64-bit). On Windows 10 and 11 use the normal Windows download."
} else {
    "Needs Windows 10 or 11 (64-bit). For Windows 7, 8 and 8.1 use the Windows7 download."
}
@"
HS2 Port $version for Windows (64-bit)
$systems

Copy "HS2 Port.vst3" (the whole folder) to C:\Program Files\Common Files\VST3
"HS2 Port.exe" is the standalone version and can be run from anywhere.

The plugin reads your own Hypersonic 2 content. It finds the folder from the original Hypersonic 2
installation; otherwise it uses C:\Hypersonic 2 Content, or choose the folder with
"Choose content folder..." in the plugin's Settings.
"@ | Set-Content -Encoding UTF8 "$stage\Install.txt"

Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "== $zip"
