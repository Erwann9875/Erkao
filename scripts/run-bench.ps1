param(
  [string]$Exe = "",
  [string]$BenchDir = "bench",
  [int]$Repeat = 1
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptRoot "resolve-erkao.ps1")
$Exe = Resolve-ErkaoExe -Exe $Exe

if (-not (Test-Path -LiteralPath $Exe)) {
  Write-Error "Executable not found: $Exe"
  exit 1
}

if (-not (Test-Path -LiteralPath $BenchDir)) {
  Write-Error "Bench directory not found: $BenchDir"
  exit 1
}

$files = Get-ChildItem -LiteralPath $BenchDir -Filter "*.ek" -File | Sort-Object Name
if (-not $files -or $files.Count -eq 0) {
  Write-Host "No benchmark files found in $BenchDir"
  exit 0
}

if ($Repeat -lt 1) {
  Write-Error "Repeat must be >= 1"
  exit 1
}

foreach ($file in $files) {
  for ($i = 1; $i -le $Repeat; $i++) {
    if ($Repeat -gt 1) {
      Write-Host "== $($file.Name) (run $i/$Repeat) =="
    } else {
      Write-Host "== $($file.Name) =="
    }
    & $Exe run $file.FullName
    if ($LASTEXITCODE -ne 0) {
      exit $LASTEXITCODE
    }
  }
}
