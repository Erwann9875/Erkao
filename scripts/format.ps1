param(
  [string]$Exe = ".\\build\\Debug\\erkao.exe",
  [string[]]$Dirs = @("tests", "examples"),
  [switch]$Check
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Exe)) {
  Write-Error "Executable not found: $Exe"
  exit 1
}

$files = @()
foreach ($dir in $Dirs) {
  if (Test-Path -LiteralPath $dir) {
    $files += Get-ChildItem -LiteralPath $dir -Recurse -Filter "*.ek" -File
  }
}

if (-not $files -or $files.Count -eq 0) {
  Write-Host "No .ek files found."
  exit 0
}

$argsList = @("fmt")
if ($Check) {
  $argsList += "--check"
}

foreach ($file in $files) {
  $argsList += $file.FullName
}

& $Exe @argsList
exit $LASTEXITCODE
