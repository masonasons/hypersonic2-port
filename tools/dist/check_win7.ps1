# Checks that an HS2 Port binary asks Windows only for what Windows 7 has: every DLL and function it imports, against
# Windows 7's own export lists, which ship with YY-Thunks in the build folder's _deps (HS2_WIN7, plugin/CMakeLists.txt).
# As in the user's TS Reborn (tools\check_win7.ps1) and FastChat. Delay-loaded imports, loaded only where present, are
# not checked.
#
#   powershell -ExecutionPolicy Bypass -File tools\dist\check_win7.ps1 <exe or .vst3 module> [-BuildDir build_dist_win]
param([Parameter(Mandatory = $true)][string]$Binary, [string]$BuildDir = 'build_dist_win')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path

$build = if ([IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$cfg = Join-Path $build "_deps\yythunks-src\Config\x64\6.1.7600.txt"
if (-not (Test-Path $cfg)) { throw "Windows 7 export lists not found ($cfg); configure $BuildDir with HS2_WIN7 on first." }
$win7 = @{}
$dll = $null
foreach ($line in [IO.File]::ReadLines((Resolve-Path $cfg))) {
    if ($line -match '^\[(.+)\]$') {
        $dll = $Matches[1].ToLower()
        $win7[$dll] = New-Object 'System.Collections.Generic.HashSet[string]'
    } elseif ($dll -and $line -match '^\d+=(.+)$') {
        [void]$win7[$dll].Add($Matches[1])
    }
}

$dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
if (-not $dumpbin) {
    $dumpbin = Get-ChildItem "${env:ProgramFiles}\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $dumpbin) { throw 'dumpbin.exe not found (Visual Studio C++ build tools)' }
$imports = & $dumpbin /nologo /imports (Resolve-Path $Binary)
$problems = @()
$functions = 0
$dll = $null
foreach ($line in $imports) {
    if ($line -match 'delay load imports') { break }  # the rest are delay-loaded: optional by design
    if ($line -match '^\s+Summary\s*$') { break }      # section sizes, not imports
    if ($line -match '^\s{4}(\S+\.(dll|drv))$') {
        $dll = $Matches[1].ToLower()
        if (-not $win7.ContainsKey($dll)) { $problems += "$dll (no such DLL on Windows 7)" }
    } elseif ($dll -and $line -match '^\s+[0-9A-F]+\s+([A-Za-z_]\w*)$') {
        $functions++
        if ($win7.ContainsKey($dll) -and -not $win7[$dll].Contains($Matches[1])) { $problems += "$dll!$($Matches[1])" }
    }
}
if ($problems) {
    "$(Split-Path $Binary -Leaf): not on Windows 7:"
    $problems | Sort-Object -Unique | ForEach-Object { "  $_" }
    exit 1
}
"$(Split-Path $Binary -Leaf): all $functions imported functions exist on Windows 7."
