param(
  [string]$Exe = "",
  [string]$Script = "tests\\stress\\gc_deep.ek"
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptRoot "resolve-erkao.ps1")
$Exe = Resolve-ErkaoExe -Exe $Exe

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
