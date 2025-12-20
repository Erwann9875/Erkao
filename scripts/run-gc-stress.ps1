param(
  [string]$Exe = ".\\build\\Debug\\erkao.exe",
  [string]$Script = "tests\\stress\\gc_deep.ek"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Exe)) {
  Write-Error "Executable not found: $Exe"
  exit 1
}

if (-not (Test-Path -LiteralPath $Script)) {
  Write-Error "Stress test not found: $Script"
  exit 1
}

$env:ERKAO_GC_LOG = "1"
& $Exe run $Script
exit $LASTEXITCODE
