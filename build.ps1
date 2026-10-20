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

# Icon and version information of the application (visible in the taskbar and in the file properties).
function RcPath([string]$p) { return $p -replace '\\', '\\' }
$appRc = @(
    "#include <windows.h>",
    "1 ICON `"$(RcPath (Join-Path $root 'app\ui\icon.ico'))`"",
    "1 VERSIONINFO",
    "FILEVERSION 1,0,0,0",
    "PRODUCTVERSION 1,0,0,0",
    "FILEFLAGSMASK 0x3fL",
    "FILEFLAGS 0x0L",
    "FILEOS 0x40004L",
    "FILETYPE 0x1L",
    "FILESUBTYPE 0x0L",
    "BEGIN",
    "    BLOCK `"StringFileInfo`"",
    "    BEGIN",
    "        BLOCK `"040904b0`"",
    "        BEGIN",
    "            VALUE `"FileDescription`", `"P2Render - rendu rapide de demos Portal 2`"",
    "            VALUE `"FileVersion`", `"1.0`"",
    "            VALUE `"InternalName`", `"P2Render`"",
    "            VALUE `"OriginalFilename`", `"P2Render.exe`"",
    "            VALUE `"ProductName`", `"P2Render`"",
    "            VALUE `"ProductVersion`", `"1.0`"",
    "        END",
    "    END",
    "    BLOCK `"VarFileInfo`"",
    "    BEGIN",
    "        VALUE `"Translation`", 0x409, 1200",
    "    END",
    "END"
)
$appRcFile = Join-Path $build "app.rc"
Set-Content -LiteralPath $appRcFile -Value $appRc -Encoding UTF8
$appRes = Join-Path $build "app.res"
$vcvars64 = Join-Path $vsBuild "vcvars64.bat"
$log = cmd /c "`"$vcvars64`" >nul 2>&1 && rc.exe /nologo /fo `"$appRes`" `"$appRcFile`" 2>&1"
if ($LASTEXITCODE -ne 0) { $log | ForEach-Object { Write-Host $_ }; throw "resource build failed" }

Compile "x86" (Join-Path $build "core") "$common /LD $core /Fe:p2r_core.dll /link /DEBUG /OPT:REF /OPT:ICF kernel32.lib user32.lib psapi.lib"
Compile "x86" (Join-Path $build "launch") "$common /DUNICODE /D_UNICODE `"$root\launcher\launch.cpp`" /Fe:p2r_launch.exe /link /DEBUG"
Compile "x64" (Join-Path $build "encoder") "$common /DUNICODE /D_UNICODE `"$root\encoder\encoder.cpp`" /Fe:p2r_encoder.exe /link /DEBUG d3d11.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib"
Compile "x64" (Join-Path $build "app") "$common /DUNICODE /D_UNICODE `"$root\app\main.cpp`" `"$appRes`" /Fe:P2Render.exe /link /DEBUG /SUBSYSTEM:WINDOWS ws2_32.lib shell32.lib ole32.lib bcrypt.lib advapi32.lib user32.lib urlmon.lib"

Copy-Item (Join-Path $build "core\p2r_core.dll"), (Join-Path $build "core\p2r_core.pdb") $dist -Force
Copy-Item (Join-Path $build "launch\p2r_launch.exe") $dist -Force
Copy-Item (Join-Path $build "encoder\p2r_encoder.exe") $dist -Force
Copy-Item (Join-Path $build "app\P2Render.exe") $dist -Force
New-Item -ItemType Directory -Force (Join-Path $dist "ui") | Out-Null
Copy-Item (Join-Path $root "app\ui\*") (Join-Path $dist "ui") -Recurse -Force

# --- portable folder -----------------------------------------------------------------------
# dist\portable: everything needed, data included (settings, imported demos, downloaded ffmpeg and
# the Edge profile all live in portable\data), so the folder can be copied or zipped as is. No
# installer, no registry, nothing in %LOCALAPPDATA%, nothing in the game folder.
$portable = Join-Path $dist "portable"
New-Item -ItemType Directory -Force (Join-Path $portable "ui"), (Join-Path $portable "data") | Out-Null
Copy-Item (Join-Path $dist "P2Render.exe"), (Join-Path $dist "p2r_launch.exe"), (Join-Path $dist "p2r_core.dll"),
          (Join-Path $dist "p2r_encoder.exe") $portable -Force
Copy-Item (Join-Path $dist "ui\*") (Join-Path $portable "ui") -Force
$readme = @(
    "P2Render - rendu rapide des demos de Portal 2",
    "",
    "1. Double-clique sur P2Render.exe",
    "2. Glisse une demo (.dem) dans la fenetre",
    "3. Clique sur Lancer le rendu (les videos arrivent dans le dossier videos_out, juste a cote)",
    "",
    "Version portable : rien n'est installe. Tout ce que l'application ecrit reste dans ce",
    "dossier, sous-dossier data (reglages, demos importees, profil du navigateur, ffmpeg",
    "telecharge au besoin). Deplace ou copie le dossier entier, il fonctionne tel quel.",
    "",
    "Rien n'est installe dans Portal 2 : l'application lance le jeu et injecte son module",
    "uniquement le temps du rendu.",
    "",
    "Si Portal 2 n'est pas trouve (installation Steam ailleurs), indique son dossier dans",
    "les reglages de l'interface."
)
Set-Content -LiteralPath (Join-Path $portable "LISEZ-MOI.txt") -Value $readme -Encoding UTF8

Write-Host "built $dist"
Write-Host "portable: dist\portable (copy or zip the whole folder)"

if ($Run) { Start-Process (Join-Path $portable "P2Render.exe") }
