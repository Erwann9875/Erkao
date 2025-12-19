param(
  [string]$Action
)

$ErrorActionPreference = "Stop"

function Test-Command {
  param([string]$Name)
  return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
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

  $buildDir = "build"
  cmake -S . -B $buildDir
  cmake --build $buildDir
}

function Show-Menu {
  Write-Host ""
  Write-Host "Erkao setup"
  Write-Host "1) Install prerequisites (CMake, compiler)"
  Write-Host "2) Build"
}

switch ($Action) {
  "1" { Install-Prereqs; break }
  "install" { Install-Prereqs; break }
  "2" { Build-Project; break }
  "build" { Build-Project; break }
  default {
    Show-Menu
    $choice = Read-Host "Choose an option"
    switch ($choice) {
      "1" { Install-Prereqs }
      "2" { Build-Project }
      default { Write-Host "No action selected." }
    }
  }
}
