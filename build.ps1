# Builds build\d3d9.dll (x86, static CRT) with the Visual Studio 18 toolchain.
#   .\build.ps1             build
#   .\build.ps1 -Deploy     also copy d3d9.dll (+ .pdb) next to portal2.exe
#   .\build.ps1 -Undeploy   remove them from the game folder (the game is plain D3D9 again)
param([switch]$Deploy, [switch]$Undeploy, [string]$GameDir = "E:\Program Files (x86)\Steam\steamapps\common\Portal 2")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path

if ($Undeploy) {
    foreach ($f in "d3d9.dll", "d3d9.pdb") {
        $p = Join-Path $GameDir $f
        if (Test-Path $p) { Remove-Item $p; Write-Host "removed $p" }
    }
    return
}

# Import the x86 MSVC environment once per shell.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vcvars = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars32.bat"
    if (-not (Test-Path $vcvars)) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        $inst = & $vswhere -latest -products * -property installationPath
        $vcvars = Join-Path $inst "VC\Auxiliary\Build\vcvars32.bat"
    }
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
    }
}

$out = Join-Path $root "build"
New-Item -ItemType Directory -Force $out | Out-Null
$src = Get-ChildItem (Join-Path $root "src") -Filter *.cpp | ForEach-Object { $_.FullName }
$def = Join-Path $root "src\d3d9.def"
$common = @("/nologo", "/O2", "/MT", "/EHsc", "/W3", "/Zi", "/std:c++17", "/DWIN32_LEAN_AND_MEAN", "/D_CRT_SECURE_NO_WARNINGS")

Push-Location $out
try {
    & cl.exe @common /LD @src /Fe:d3d9.dll /Fd:d3d9_cl.pdb /link /DEBUG /OPT:REF /OPT:ICF "/DEF:$def" kernel32.lib /PDB:d3d9.pdb
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
} finally { Pop-Location }

if ($Deploy) {
    Copy-Item (Join-Path $out "d3d9.dll") (Join-Path $GameDir "d3d9.dll") -Force
    Copy-Item (Join-Path $out "d3d9.pdb") (Join-Path $GameDir "d3d9.pdb") -Force
    Write-Host "deployed to $GameDir"
}
