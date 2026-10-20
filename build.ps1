# Builds build\d3d9.dll (x86, static CRT) and build\encoder\p2r_encoder.exe (x64) with the Visual
# Studio 18 toolchain.
# The proxy goes into <Portal 2>\bin: shaderapidx9.dll is loaded from bin\ by full path, so its
# d3d9.dll import is looked up in bin\ first; a d3d9.dll next to portal2.exe is NOT picked up.
# The encoder sits next to it (the proxy starts it from its own folder).
#   .\build.ps1             build
#   .\build.ps1 -Deploy     also copy d3d9.dll (+ .pdb) and p2r_encoder.exe into <Portal 2>\bin
#   .\build.ps1 -Undeploy   remove them from the game folder (the game is plain D3D9 again)
param([switch]$Deploy, [switch]$Undeploy, [string]$GameDir = "E:\Program Files (x86)\Steam\steamapps\common\Portal 2")
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$binDir = Join-Path $GameDir "bin"
$vsBuild = "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build"

if ($Undeploy) {
    foreach ($f in "d3d9.dll", "d3d9.pdb", "p2r_encoder.exe") {
        $p = Join-Path $binDir $f
        if (Test-Path $p) { Remove-Item $p; Write-Host "removed $p" }
    }
    return
}

# Import the x86 MSVC environment once per shell.
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vcvars = Join-Path $vsBuild "vcvars32.bat"
    if (-not (Test-Path $vcvars)) {
        $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
        $inst = & $vswhere -latest -products * -property installationPath
        $vsBuild = Join-Path $inst "VC\Auxiliary\Build"
        $vcvars = Join-Path $vsBuild "vcvars32.bat"
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
    & cl.exe @common /LD @src /Fe:d3d9.dll /Fd:d3d9_cl.pdb /link /DEBUG /OPT:REF /OPT:ICF "/DEF:$def" kernel32.lib psapi.lib /PDB:d3d9.pdb
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
} finally { Pop-Location }

# p2r_encoder.exe is x64: built in its own cmd with the x64 toolchain.
$vc64 = Join-Path $vsBuild "vcvars64.bat"
$enc = Join-Path $root "encoder\encoder.cpp"
$encOut = Join-Path $out "encoder"
New-Item -ItemType Directory -Force $encOut | Out-Null
$encCmd = "`"$vc64`" >nul 2>&1 && cd /d `"$encOut`" && cl.exe /nologo /O2 /MT /EHsc /W3 /Zi /std:c++17 /DUNICODE /D_UNICODE `"$enc`" /Fe:p2r_encoder.exe /link /DEBUG d3d11.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib"
$encLog = cmd /c $encCmd
$encOk = $LASTEXITCODE -eq 0
$encLog | Select-String -Pattern "error|warning" | ForEach-Object { Write-Host $_.Line }
if (-not $encOk) { throw "encoder build failed" }

if ($Deploy) {
    Copy-Item (Join-Path $out "d3d9.dll") (Join-Path $binDir "d3d9.dll") -Force
    Copy-Item (Join-Path $out "d3d9.pdb") (Join-Path $binDir "d3d9.pdb") -Force
    Copy-Item (Join-Path $encOut "p2r_encoder.exe") (Join-Path $binDir "p2r_encoder.exe") -Force
    Write-Host "deployed to $binDir"
}
