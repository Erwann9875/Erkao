param(
  [string]$Exe = "",
  [string]$TestsDir = "tests"
)

$ErrorActionPreference = "Stop"
$isWindows = $env:OS -eq "Windows_NT"
if (-not $isWindows -and $PSVersionTable.PSEdition -eq "Desktop") {
  $isWindows = $true
}
if (-not $Exe) {
  if ($isWindows) {
    $Exe = ".\\build\\Debug\\erkao.exe"
  } else {
    $Exe = "./build/erkao"
  }
}
$env:ERKAO_STACK_TRACE = "1"
$httpTestEnabled = $true
if ($env:ERKAO_HTTP_TEST) {
  $httpSetting = $env:ERKAO_HTTP_TEST.ToLowerInvariant()
  if ($httpSetting -in @("0", "false", "no", "off")) {
    $httpTestEnabled = $false
  }
}
$httpServer = $null

function Find-Python {
  param([bool]$IsWindows)
  $candidates = if ($IsWindows) { @("py", "python", "python3") } else { @("python3", "python") }
  foreach ($candidate in $candidates) {
    $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
    if ($cmd) {
      if ($cmd.Path -match "WindowsApps") {
        continue
      }
      $args = @()
      if ($candidate -eq "py") {
        $args = @("-3")
      }
      & $cmd.Path @($args + @("-V")) 2>$null | Out-String | Out-Null
      if ($LASTEXITCODE -ne 0) {
        continue
      }
      return [pscustomobject]@{
        Path = $cmd.Path
        Args = $args
      }
    }
  }
  return $null
}

function Wait-HttpServer {
  param([int]$Port)
  for ($i = 0; $i -lt 30; $i++) {
    try {
      $client = [System.Net.Sockets.TcpClient]::new()
      $async = $client.BeginConnect("127.0.0.1", $Port, $null, $null)
      $ready = $async.AsyncWaitHandle.WaitOne(200)
      if ($ready -and $client.Connected) {
        $client.Close()
        return $true
      }
      $client.Close()
    } catch {
    }
    Start-Sleep -Milliseconds 200
  }
  return $false
}

if (-not (Test-Path -LiteralPath $Exe)) {
  Write-Error "Executable not found: $Exe"
  exit 1
}

if (-not (Test-Path -LiteralPath $TestsDir)) {
  Write-Error "Tests directory not found: $TestsDir"
  exit 1
}

if ($httpTestEnabled) {
  $httpPort = $env:ERKAO_HTTP_TEST_PORT
  if (-not $httpPort) {
    $httpPort = "18421"
    $env:ERKAO_HTTP_TEST_PORT = $httpPort
  }
  $pythonInfo = Find-Python -IsWindows:$isWindows
  if (-not $pythonInfo) {
    Write-Error "Python is required for HTTP tests (ERKAO_HTTP_TEST=1)."
    exit 1
  }
  $serverPath = Join-Path $TestsDir "http_server.py"
  if (-not (Test-Path -LiteralPath $serverPath)) {
    Write-Error "HTTP test server not found: $serverPath"
    exit 1
  }
  $httpServer = Start-Process -FilePath $pythonInfo.Path -ArgumentList @($pythonInfo.Args + @($serverPath, $httpPort)) -NoNewWindow -PassThru
  if (-not (Wait-HttpServer -Port ([int]$httpPort))) {
    if ($httpServer -and -not $httpServer.HasExited) {
      Stop-Process -Id $httpServer.Id -Force
    }
    Write-Error "HTTP test server failed to start."
    exit 1
  }
}

$exitCode = 0
try {
  $tests = Get-ChildItem -LiteralPath $TestsDir -Filter "*.ek" | Sort-Object Name
  if (-not $tests -or $tests.Count -eq 0) {
    Write-Host "No tests found in $TestsDir"
  } else {
    if (-not $httpTestEnabled) {
      $tests = $tests | Where-Object { $_.Name -ne "15_http.ek" }
    }

    $failed = 0

    foreach ($test in $tests) {
      $expectedPath = [System.IO.Path]::ChangeExtension($test.FullName, ".out")
      if (-not (Test-Path -LiteralPath $expectedPath)) {
        Write-Host "Missing expected output: $expectedPath"
        $failed++
        continue
      }

      $relativeTest = Resolve-Path -LiteralPath $test.FullName -Relative
      if ($relativeTest.StartsWith('.\')) {
        $relativeTest = $relativeTest.Substring(2)
      }
      if ($relativeTest.StartsWith("./")) {
        $relativeTest = $relativeTest.Substring(2)
      }
      $relativeTest = $relativeTest -replace "\\", "/"

      $output = & {
        $ErrorActionPreference = "Continue"
        if ($null -ne $PSNativeCommandUseErrorActionPreference) {
          $PSNativeCommandUseErrorActionPreference = $false
        }
        & $Exe run $relativeTest 2>&1 |
          ForEach-Object {
            if ($_ -is [System.Management.Automation.ErrorRecord]) {
              $_.Exception.Message
            } else {
              $_
            }
          } |
          Out-String
      }
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
      $exitCode = 1
    } else {
      Write-Host ""
      Write-Host "All tests passed ($($tests.Count))."
    }
  }
} finally {
  if ($httpServer -and -not $httpServer.HasExited) {
    Stop-Process -Id $httpServer.Id -Force
  }
}

if ($exitCode -ne 0) {
  exit $exitCode
}
