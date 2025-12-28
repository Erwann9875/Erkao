$script = Join-Path $PSScriptRoot "..\\lint.ps1"
& $script @args
exit $LASTEXITCODE
