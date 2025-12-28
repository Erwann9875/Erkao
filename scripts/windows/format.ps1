$script = Join-Path $PSScriptRoot "..\\format.ps1"
& $script @args
exit $LASTEXITCODE
