$script = Join-Path $PSScriptRoot "..\\run-gc-stress.ps1"
& $script @args
exit $LASTEXITCODE
