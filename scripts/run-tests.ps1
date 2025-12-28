param(
  [string]$Exe = "",
  [string]$TestsDir = "tests",
  [switch]$Update,
  [switch]$WriteActual
)

$ErrorActionPreference = "Stop"
$scriptRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $scriptRoot "resolve-erkao.ps1")
$isWin = $env:OS -eq "Windows_NT"
if (-not $isWin -and $PSVersionTable.PSEdition -eq "Desktop") {
  $isWin = $true
}
$Exe = Resolve-ErkaoExe -Exe $Exe
$env:ERKAO_STACK_TRACE = "1"
$httpTestEnabled = $true
if ($env:ERKAO_HTTP_TEST) {
  $httpSetting = $env:ERKAO_HTTP_TEST.ToLowerInvariant()
  if ($httpSetting -in @("0", "false", "no", "off")) {
    $httpTestEnabled = $false
  }
}
$httpServer = $null
$httpServerStdout = $null
$httpServerStderr = $null

function Wait-HttpServer {
  param([int]$Port, [System.Diagnostics.Process]$Process)
  for ($i = 0; $i -lt 10; $i++) {
    if ($Process -and $Process.HasExited) {
      return $false
    }
    try {
      $client = [System.Net.Sockets.TcpClient]::new()
      $async = $client.BeginConnect("127.0.0.1", $Port, $null, $null)
      $ready = $async.AsyncWaitHandle.WaitOne(1000)
      if ($ready -and $client.Connected) {
        $client.Close()
        return $true
      }
      $client.Close()
    } catch {
       Write-Host "DEBUG: Connection attempt $i to $Port failed: $_"
    }
    Start-Sleep -Milliseconds 500
  }
  return $false
}

function Wait-HttpServerPort {
  param([System.Diagnostics.Process]$Process, [string]$StdoutPath)
  $pattern = [regex]"http\.serve listening on\s+http://127\.0\.0\.1:(\d+)"
  for ($i = 0; $i -lt 60; $i++) {
    if ($Process -and $Process.HasExited) {
      return $null
    }
    if ($StdoutPath -and (Test-Path -LiteralPath $StdoutPath)) {
      try {
        $text = Get-Content -LiteralPath $StdoutPath -Raw
        if ($text) {
          $match = $pattern.Match($text)
          if ($match.Success) {
            return [int]$match.Groups[1].Value
          }
        }
      } catch {
      }
    }

    if ($Process -and $Process.HasExited) {
       if ($StdoutPath -and (Test-Path -LiteralPath $StdoutPath)) {
         try {
           $text = Get-Content -LiteralPath $StdoutPath -Raw
           if ($text) {
             $match = $pattern.Match($text)
             if ($match.Success) {
                return [int]$match.Groups[1].Value
             }
           }
         } catch {}
       }
       return $null
    }
    Start-Sleep -Milliseconds 800
  }
  return $null
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
  $serverPath = Join-Path $TestsDir "http_server.ek"
  if (-not (Test-Path -LiteralPath $serverPath)) {
    Write-Error "HTTP test server not found: $serverPath"
    exit 1
  }
  $httpServerStdout = New-TemporaryFile
  $httpServerStderr = New-TemporaryFile
  $startProcessArgs = @{
    FilePath = $Exe
    ArgumentList = @("run", $serverPath)
    PassThru = $true
    RedirectStandardOutput = $httpServerStdout
    RedirectStandardError = $httpServerStderr
  }
  if ($isWin) {
    $startProcessArgs.NoNewWindow = $true
  }
  $httpServer = Start-Process @startProcessArgs
  $httpPort = Wait-HttpServerPort -Process $httpServer -StdoutPath $httpServerStdout
  if (-not $httpPort) {
    if ($httpServer -and -not $httpServer.HasExited) {
      Stop-Process -Id $httpServer.Id -Force
    }
    $details = @()
    if ($httpServer -and $httpServer.HasExited) {
      $details += ("exit code {0}" -f $httpServer.ExitCode)
    }
    if ($httpServerStdout -and (Test-Path -LiteralPath $httpServerStdout)) {
      try {
        $stdout = Get-Content -LiteralPath $httpServerStdout -Raw
        if ($stdout) {
          $stdout = $stdout.TrimEnd()
        }
        if ($stdout) {
          $details += ("stdout:`n{0}" -f $stdout)
        }
      } catch {
      }
    }
    if ($httpServerStderr -and (Test-Path -LiteralPath $httpServerStderr)) {
      try {
        $stderr = Get-Content -LiteralPath $httpServerStderr -Raw
        if ($stderr) {
          $stderr = $stderr.TrimEnd()
        }
        if ($stderr) {
          $details += ("stderr:`n{0}" -f $stderr)
        }
      } catch {
      }
    }
    $suffix = ""
    if ($details.Count -gt 0) {
      $suffix = " `n" + ($details -join "`n")
    }
    Write-Error ("HTTP test server failed to start.{0}" -f $suffix)
    exit 1
  }

  $env:ERKAO_HTTP_TEST_PORT = "$httpPort"

  if (-not (Wait-HttpServer -Port ([int]$httpPort) -Process $httpServer)) {
    Write-Error "HTTP test server started on port $httpPort but refused connection."
    Stop-Process -Id $httpServer.Id -Force
    exit 1
  }
}

$exitCode = 0
try {
  $tests = Get-ChildItem -LiteralPath $TestsDir -Filter "*.ek" |
    Where-Object { $_.Name -ne "http_server.ek" } |
    Sort-Object Name
  if (-not $tests -or $tests.Count -eq 0) {
    Write-Host "No tests found in $TestsDir"
  } else {
    if (-not $httpTestEnabled) {
      $tests = $tests | Where-Object { $_.Name -ne "15_http.ek" }
    }

    $failed = 0
    $updated = 0

    foreach ($test in $tests) {
      $expectedPath = [System.IO.Path]::ChangeExtension($test.FullName, ".out")
      if (-not (Test-Path -LiteralPath $expectedPath)) {
        if (-not $Update) {
          Write-Host "Missing expected output: $expectedPath"
          $failed++
          continue
        }
      }

      $relativeTest = Resolve-Path -LiteralPath $test.FullName -Relative
      if ($relativeTest.StartsWith('.\')) {
        $relativeTest = $relativeTest.Substring(2)
      }
      if ($relativeTest.StartsWith("./")) {
        $relativeTest = $relativeTest.Substring(2)
      }
      $relativeTest = $relativeTest -replace "\\", "/"
      $command = "run"
      if ($test.BaseName -like "*_typecheck*") {
        $command = "typecheck"
      }

      $output = & {
        $ErrorActionPreference = "Continue"
        if ($null -ne $PSNativeCommandUseErrorActionPreference) {
          $PSNativeCommandUseErrorActionPreference = $false
        }
        & $Exe $command $relativeTest 2>&1 |
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

      if ($Update) {
        $expected = ""
        if (Test-Path -LiteralPath $expectedPath) {
          $expected = Get-Content -LiteralPath $expectedPath -Raw
          $expected = $expected -replace "`r`n", "`n"
          $expected = $expected.TrimEnd()
        }
        if ($output -ne $expected) {
          [System.IO.File]::WriteAllText($expectedPath, $output)
          Write-Host "UPDATED $($test.Name)"
          $updated++
        } else {
          Write-Host "UNCHANGED $($test.Name)"
        }
        continue
      }

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
        if ($WriteActual) {
          [System.IO.File]::WriteAllText("$expectedPath.actual", $output)
        }
        $failed++
      } else {
        Write-Host "PASS $($test.Name)"
      }
    }

    if ($Update) {
      Write-Host ""
      Write-Host "Updated: $updated / $($tests.Count)"
    } elseif ($failed -gt 0) {
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
  if ($httpServer) {
    try {
      Wait-Process -Id $httpServer.Id -Timeout 5 -ErrorAction SilentlyContinue
    } catch {
    }
  }
  if ($httpServerStdout -and (Test-Path -LiteralPath $httpServerStdout)) {
    try {
      Remove-Item -LiteralPath $httpServerStdout -Force -ErrorAction Stop
    } catch {
    }
  }
  if ($httpServerStderr -and (Test-Path -LiteralPath $httpServerStderr)) {
    try {
      Remove-Item -LiteralPath $httpServerStderr -Force -ErrorAction Stop
    } catch {
    }
  }
}

exit $exitCode
