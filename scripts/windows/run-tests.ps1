$script = Join-Path $PSScriptRoot "..\\run-tests.ps1"
& $script @args
exit $LASTEXITCODE
