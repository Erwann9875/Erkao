$script = Join-Path $PSScriptRoot "..\\update-tests.ps1"
& $script @args
exit $LASTEXITCODE
