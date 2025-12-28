$script = Join-Path $PSScriptRoot "..\\run-bench.ps1"
& $script @args
exit $LASTEXITCODE
