$script = Join-Path $PSScriptRoot "..\\install-sdl2.ps1"
& $script @args
exit $LASTEXITCODE
