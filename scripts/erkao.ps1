param(
  [string]$Action,
  [string]$Generator
)

$ErrorActionPreference = "Stop"

function Test-Command {
  param([string]$Name)
  return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Test-CompilerInPath {
  return (Test-Command "cl") -or (Test-Command "gcc") -or (Test-Command "clang")
}

function Is-VisualStudioGenerator {
  param([string]$Name)
  return $Name -match "^Visual Studio [0-9]+"
}

function Get-VSWherePath {
  $paths = @()
  if (${env:ProgramFiles(x86)}) {
    $paths += (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\\Installer\\vswhere.exe")
  }
  if ($env:ProgramFiles) {
    $paths += (Join-Path $env:ProgramFiles "Microsoft Visual Studio\\Installer\\vswhere.exe")
  }

  foreach ($path in $paths) {
    if (Test-Path -LiteralPath $path) {
      return $path
    }
  }
  return $null
}

function Get-VisualStudioGenerator {
  $vswhere = Get-VSWherePath
  if (-not $vswhere) {
    return $null
  }

  $version = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationVersion 2>$null
  if (-not $version) {
    return $null
  }

  $major = $version.Trim().Split(".")[0]
  switch ($major) {
    "17" { return "Visual Studio 17 2022" }
    "16" { return "Visual Studio 16 2019" }
    "15" { return "Visual Studio 15 2017" }
    default { return $null }
  }
}

function Test-VisualStudioInstalled {
  return $null -ne (Get-VisualStudioGenerator)
}

function Resolve-Generator {
  param([string]$Requested)

  if ($Requested) {
    return $Requested
  }

  if ($env:CMAKE_GENERATOR) {
    if ($env:CMAKE_GENERATOR -eq "NMake Makefiles" -and -not (Test-Command "nmake")) {
      Write-Host "CMAKE_GENERATOR is set to NMake Makefiles but nmake is not available. Ignoring it."
    } elseif ((Is-VisualStudioGenerator $env:CMAKE_GENERATOR) -and -not (Test-VisualStudioInstalled)) {
      Write-Host "CMAKE_GENERATOR is set to $env:CMAKE_GENERATOR but no Visual Studio installation was detected. Ignoring it."
    } else {
      return $env:CMAKE_GENERATOR
    }
  }

  if (Test-Command "ninja") {
    return "Ninja"
  }

  $vsGen = Get-VisualStudioGenerator
  if ($vsGen) {
    return $vsGen
  }

  if (Test-Command "nmake") {
    return "NMake Makefiles"
  }

  if (Test-Command "mingw32-make") {
    return "MinGW Makefiles"
  }

  return $null
}

function Require-RepoRoot {
  if (-not (Test-Path -Path "CMakeLists.txt")) {
    throw "Run this script from the repo root (CMakeLists.txt not found)."
  }
}

function Install-WithWinget {
  param([string]$Id)
  Write-Host "winget install --id $Id -e --source winget"
  winget install --id $Id -e --source winget
}

function Install-WithChoco {
  param([string]$Package)
  Write-Host "choco install $Package -y"
  choco install $Package -y
}

function Install-Prereqs {
  if (-not (Test-Command "cmake")) {
    if (Test-Command "winget") {
      Install-WithWinget "Kitware.CMake"
    } elseif (Test-Command "choco") {
      Install-WithChoco "cmake"
    } else {
      throw "Neither winget nor choco found. Install CMake manually."
    }
  } else {
    Write-Host "CMake already installed."
  }

  if (-not (Test-Command "cl") -and -not (Test-Command "gcc") -and -not (Test-Command "clang")) {
    Write-Host "No C compiler detected."
    if (Test-Command "winget") {
      $answer = Read-Host "Install Visual Studio Build Tools? (y/n)"
      if ($answer -match "^(y|yes)$") {
        Install-WithWinget "Microsoft.VisualStudio.2022.BuildTools"
      } else {
        Write-Host "Skipping Build Tools install."
      }
    } else {
      Write-Host "Install a C compiler (Visual Studio Build Tools, MinGW, or LLVM) manually."
    }
  } else {
    Write-Host "C compiler detected."
  }
}

function Build-Project {
  Require-RepoRoot
  if (-not (Test-Command "cmake")) {
    throw "CMake not found. Run option 1 first."
  }

  $resolvedGenerator = Resolve-Generator -Requested $Generator
  if (-not $resolvedGenerator) {
    throw "No CMake generator found. Install Visual Studio Build Tools or Ninja, or pass -Generator."
  }

  if ((Is-VisualStudioGenerator $resolvedGenerator) -and -not (Test-VisualStudioInstalled)) {
    if ($Generator) {
      throw "CMake generator '$resolvedGenerator' requested but no Visual Studio installation was detected. Install Build Tools or choose a different generator (e.g., Ninja)."
    }
    throw "CMake generator '$resolvedGenerator' selected but no Visual Studio installation was detected. Install Build Tools or choose a different generator (e.g., Ninja)."
  }

  if ($resolvedGenerator -eq "NMake Makefiles" -and -not (Test-Command "nmake")) {
    throw "CMake generator 'NMake Makefiles' requested but nmake is not available. Install Visual Studio Build Tools or choose a different generator (e.g., Ninja)."
  }

  if (-not (Test-CompilerInPath) -and -not (Test-VisualStudioInstalled)) {
    throw "No C compiler detected. Run option 1 or install a compiler (Visual Studio Build Tools, MinGW, or LLVM)."
  }

  $buildDir = "build"
  $cmakeArgs = @("-S", ".", "-B", $buildDir, "-G", $resolvedGenerator)
  cmake @cmakeArgs
  cmake --build $buildDir
}

function Format-Check {
  Require-RepoRoot
  $formatScript = Join-Path $PSScriptRoot "format.ps1"
  if (-not (Test-Path -LiteralPath $formatScript)) {
    throw "format.ps1 not found at $formatScript"
  }
  & $formatScript -Check
}

function Show-Menu {
  Write-Host ""
  Write-Host "Erkao setup"
  Write-Host "1) Install prerequisites (CMake, compiler)"
  Write-Host "2) Build"
  Write-Host "3) Format check (tests/examples)"
}

switch ($Action) {
  "1" { Install-Prereqs; break }
  "install" { Install-Prereqs; break }
  "2" { Build-Project; break }
  "build" { Build-Project; break }
  "3" { Format-Check; break }
  "format-check" { Format-Check; break }
  "formatcheck" { Format-Check; break }
  default {
    Show-Menu
    $choice = Read-Host "Choose an option"
    switch ($choice) {
      "1" { Install-Prereqs }
      "2" { Build-Project }
      "3" { Format-Check }
      default { Write-Host "No action selected." }
    }
  }
}
