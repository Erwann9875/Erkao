$script = Join-Path $PSScriptRoot "..\\erkao.ps1"
& $script @args
exit $LASTEXITCODE
