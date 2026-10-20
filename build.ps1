# Builds P2Render into dist\ with the Visual Studio 18 toolchain:
#   P2Render.exe     (x64)  the application (local web interface in an Edge app window)
#   p2r_launch.exe   (x86)  starts portal2.exe with p2r_core.dll loaded before main()
#   p2r_core.dll     (x86)  injected into the game: D3D9Ex + GPU capture + job control
#   p2r_encoder.exe  (x64)  D3D11 + Media Foundation hardware encoder, WAV mux
#   ui\                     the interface (index.html, icon.svg)
# Nothing is installed in the game folder.
#   .\build.ps1             build dist\
#   .\build.ps1 -Run        build and start dist\P2Render.exe
#   .\build.ps1 -CleanGame  remove files older versions deployed into <Portal 2>\bin (d3d9.dll proxy)
param([switch]$Run, [switch]$CleanGame, [string]$GameDir = "E:\Program Files (x86)\Steam\steamapps\common\Portal 2")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$vsBuild = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build"
if (-not (Test-Path $vsBuild)) {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $vsBuild = Join-Path (& $vswhere -latest -products * -property installationPath) "VC\Auxiliary\Build"
}

if ($CleanGame) {
    foreach ($f in "d3d9.dll", "d3d9.pdb", "p2r_encoder.exe", "p2r_encoder.log") {
        $p = Join-Path $GameDir "bin\$f"
        if (Test-Path $p) { Remove-Item $p; Write-Host "removed $p" }
    }
    return
}

$build = Join-Path $root "build"
$dist = Join-Path $root "dist"
New-Item -ItemType Directory -Force $build, $dist | Out-Null

# Runs cl.exe in a fresh cmd with the given toolchain (x86 / x64); fails on errors, shows warnings.
function Compile([string]$arch, [string]$outDir, [string]$args_) {
    $vcvars = Join-Path $vsBuild ($(if ($arch -eq "x86") { "vcvars32.bat" } else { "vcvars64.bat" }))
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $log = cmd /c "`"$vcvars`" >nul 2>&1 && cd /d `"$outDir`" && cl.exe $args_ 2>&1"
    $ok = $LASTEXITCODE -eq 0
    $log | Select-String -Pattern "error|warning" | ForEach-Object { Write-Host $_.Line }
    if (-not $ok) { throw "build failed: $args_" }
}

$common = "/nologo /O2 /MT /EHsc /W3 /Zi /std:c++17 /utf-8 /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS"
$core = (Get-ChildItem (Join-Path $root "src") -Filter *.cpp | ForEach-Object { "`"$($_.FullName)`"" }) -join " "

Compile "x86" (Join-Path $build "core") "$common /LD $core /Fe:p2r_core.dll /link /DEBUG /OPT:REF /OPT:ICF kernel32.lib user32.lib psapi.lib"
Compile "x86" (Join-Path $build "launch") "$common /DUNICODE /D_UNICODE `"$root\launcher\launch.cpp`" /Fe:p2r_launch.exe /link /DEBUG"
Compile "x64" (Join-Path $build "encoder") "$common /DUNICODE /D_UNICODE `"$root\encoder\encoder.cpp`" /Fe:p2r_encoder.exe /link /DEBUG d3d11.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib"
Compile "x64" (Join-Path $build "app") "$common /DUNICODE /D_UNICODE `"$root\app\main.cpp`" /Fe:P2Render.exe /link /DEBUG /SUBSYSTEM:WINDOWS ws2_32.lib shell32.lib ole32.lib bcrypt.lib advapi32.lib user32.lib urlmon.lib"

Copy-Item (Join-Path $build "core\p2r_core.dll"), (Join-Path $build "core\p2r_core.pdb") $dist -Force
Copy-Item (Join-Path $build "launch\p2r_launch.exe") $dist -Force
Copy-Item (Join-Path $build "encoder\p2r_encoder.exe") $dist -Force
Copy-Item (Join-Path $build "app\P2Render.exe") $dist -Force
New-Item -ItemType Directory -Force (Join-Path $dist "ui") | Out-Null
Copy-Item (Join-Path $root "app\ui\*") (Join-Path $dist "ui") -Recurse -Force
Write-Host "built $dist"

if ($Run) { Start-Process (Join-Path $dist "P2Render.exe") }
