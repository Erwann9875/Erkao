function Resolve-ErkaoExe {
  param([string]$Exe = "")

  if ($Exe) {
    return $Exe
  }

  $isWin = $env:OS -eq "Windows_NT"
  if (-not $isWin -and $PSVersionTable.PSEdition -eq "Desktop") {
    $isWin = $true
  }

  $repoRoot = Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")
  $buildRoot = Join-Path $repoRoot "build"
  if ($isWin) {
    $default = Join-Path $buildRoot "Debug\\erkao.exe"
    $pattern = "erkao.exe"
  } else {
    $default = Join-Path $buildRoot "erkao"
    $pattern = "erkao"
  }

  if (Test-Path -LiteralPath $default) {
    return $default
  }

  if (Test-Path -LiteralPath $buildRoot) {
    $found = Get-ChildItem -LiteralPath $buildRoot -Recurse -File -Filter $pattern |
      Sort-Object LastWriteTime -Descending |
      Select-Object -First 1
    if ($found) {
      return $found.FullName
    }
  }

  return $default
}
