param(
  [string]$Exe = "",
  [string]$TestsDir = "tests"
)

$ErrorActionPreference = "Stop"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$runTests = Join-Path $scriptDir "run-tests.ps1"

$params = @{}
if ($Exe) {
  $params["Exe"] = $Exe
}
if ($TestsDir) {
  $params["TestsDir"] = $TestsDir
}
$params["Update"] = $true

& $runTests @params
exit $LASTEXITCODE
