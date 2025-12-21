param(
  [string]$Exe = "",
  [string[]]$Dirs = @("tests", "examples")
)

$ErrorActionPreference = "Stop"
if (-not $Exe) {
  $isWindows = $env:OS -eq "Windows_NT"
  if (-not $isWindows -and $PSVersionTable.PSEdition -eq "Desktop") {
    $isWindows = $true
  }
  if ($isWindows) {
    $Exe = ".\\build\\Debug\\erkao.exe"
  } else {
    $Exe = "./build/erkao"
  }
}

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

$argsList = @("lint")
foreach ($file in $files) {
  $argsList += $file.FullName
}

& $Exe @argsList
exit $LASTEXITCODE
