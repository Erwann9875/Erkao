param(
  [string]$Exe = ".\\build\\Debug\\erkao.exe",
  [string]$TestsDir = "tests"
)

$ErrorActionPreference = "Stop"
$env:ERKAO_STACK_TRACE = "1"

if (-not (Test-Path -LiteralPath $Exe)) {
  Write-Error "Executable not found: $Exe"
  exit 1
}

if (-not (Test-Path -LiteralPath $TestsDir)) {
  Write-Error "Tests directory not found: $TestsDir"
  exit 1
}

$tests = Get-ChildItem -LiteralPath $TestsDir -Filter "*.ek" | Sort-Object Name
if (-not $tests -or $tests.Count -eq 0) {
  Write-Host "No tests found in $TestsDir"
  exit 0
}

$failed = 0

foreach ($test in $tests) {
  $expectedPath = [System.IO.Path]::ChangeExtension($test.FullName, ".out")
  if (-not (Test-Path -LiteralPath $expectedPath)) {
    Write-Host "Missing expected output: $expectedPath"
    $failed++
    continue
  }

  $escapedExe = $Exe.Replace('"', '""')
  $relativeTest = Resolve-Path -LiteralPath $test.FullName -Relative
  if ($relativeTest.StartsWith('.\')) {
    $relativeTest = $relativeTest.Substring(2)
  }
  if ($relativeTest.StartsWith("./")) {
    $relativeTest = $relativeTest.Substring(2)
  }
  $relativeTest = $relativeTest -replace "\\", "/"

  $escapedTest = $relativeTest.Replace('"', '""')
  $cmd = "`"$escapedExe`" run `"$escapedTest`""
  $output = cmd /c "$cmd 2>&1" | Out-String
  $output = $output -replace "`r`n", "`n"
  $output = $output.TrimEnd()

  $expected = Get-Content -LiteralPath $expectedPath -Raw
  $expected = $expected -replace "`r`n", "`n"
  $expected = $expected.TrimEnd()

  if ($output -ne $expected) {
    Write-Host ""
    Write-Host "FAIL $($test.Name)"
    Write-Host "Expected:"
    Write-Host $expected
    Write-Host "Actual:"
    Write-Host $output
    $failed++
  } else {
    Write-Host "PASS $($test.Name)"
  }
}

if ($failed -gt 0) {
  Write-Host ""
  Write-Host "Failed: $failed / $($tests.Count)"
  exit 1
}

Write-Host ""
Write-Host "All tests passed ($($tests.Count))."
