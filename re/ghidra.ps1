# Helper around Ghidra headless for the RE projects of this repo.
#   .\re\ghidra.ps1 import <proj> <binary path>          # import + full auto-analysis (slow, once)
#   .\re\ghidra.ps1 query  <proj> <cmdfile> [outfile]    # run P2Query.java commands (see script header)
# Ghidra projects are big and not versioned: they live in D:\OpenCode\P2D3D9Ex_ghidra\<proj>.
param(
    [Parameter(Mandatory=$true)][ValidateSet("import","query")][string]$Mode,
    [Parameter(Mandatory=$true)][string]$Proj,
    [Parameter(Mandatory=$true)][string]$Arg1,
    [string]$Arg2
)
$ErrorActionPreference = "Stop"
$root    = Split-Path -Parent $MyInvocation.MyCommand.Path
$ghidra  = "D:\ghidra\ghidra_12.1.4_PUBLIC\support\analyzeHeadless.bat"
$projDir = "D:\OpenCode\P2D3D9Ex_ghidra\$Proj"
$scripts = Join-Path $root "ghidra_scripts"
$outDir  = Join-Path $root "out"
New-Item -ItemType Directory -Force $projDir, $outDir | Out-Null
$nameFile = Join-Path $root "projects\$Proj.progname"

if ($Mode -eq "import") {
    $env:GHIDRA_HEADLESS_MAXMEM = "6G"
    $bin = (Resolve-Path $Arg1).Path
    New-Item -ItemType Directory -Force (Split-Path $nameFile) | Out-Null
    Set-Content -Path $nameFile -Value (Split-Path $bin -Leaf) -Encoding ascii
    $log = Join-Path $outDir "import_$Proj.log"
    $ErrorActionPreference = "Continue"   # Ghidra writes JVM warnings to stderr
    & $ghidra $projDir $Proj -import $bin -overwrite -max-cpu 4 *> $log
    "import finished (exit $LASTEXITCODE), log: $log"
} else {
    $env:GHIDRA_HEADLESS_MAXMEM = "4G"
    $cmd = (Resolve-Path $Arg1).Path
    if (-not $Arg2) { $Arg2 = Join-Path $outDir "q_$Proj.txt" }
    if (Test-Path $Arg2) { Remove-Item $Arg2 }
    $name = (Get-Content $nameFile -ErrorAction SilentlyContinue)
    if (-not $name) { throw "missing $nameFile (file name of the imported binary)" }
    $log = Join-Path $outDir "query_$Proj.log"
    $ErrorActionPreference = "Continue"   # Ghidra writes JVM warnings to stderr
    & $ghidra $projDir $Proj -process $name -noanalysis -readOnly -scriptPath $scripts -postScript P2Query.java $cmd $Arg2 *> $log
    if (Test-Path $Arg2) { Get-Content $Arg2 -Raw } else { "no output; see $log"; Get-Content $log -Tail 30 }
}
