param(
    [Parameter(Mandatory = $true)]
    [string]$StageDir
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$AppDir = Join-Path $RepoRoot "Firmware/Application"
$CacheFile = Join-Path $RepoRoot "build/arm-debug/CMakeCache.txt"
$BuildDir = Join-Path $RepoRoot "build/arm-debug/Firmware/Application"

function Stage-CMakeArtifacts {
    New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
    Copy-Item (Join-Path $BuildDir "application.elf") -Destination $StageDir -Force
    Copy-Item (Join-Path $BuildDir "example_app.bin") -Destination $StageDir -Force
    Copy-Item (Join-Path $BuildDir "example_app.appimg") -Destination $StageDir -Force
}

function Stage-MakeArtifacts {
    New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
    $localBuild = Join-Path $AppDir "build"
    Copy-Item (Join-Path $localBuild "example_app.elf") -Destination (Join-Path $StageDir "application.elf") -Force
    Copy-Item (Join-Path $localBuild "example_app.bin") -Destination $StageDir -Force
    Copy-Item (Join-Path $localBuild "example_app.appimg") -Destination $StageDir -Force
}

Push-Location $RepoRoot
try {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmake) {
        if (-not (Test-Path $CacheFile)) {
            & cmake --preset arm-debug
            if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        }

        & cmake --build --preset build-debug --target application.elf application.bin application.appimg
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        Stage-CMakeArtifacts
        exit 0
    }

    Push-Location $AppDir
    try {
        & make DEBUG=1 stage "STAGE_DIR=$StageDir"
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    finally {
        Pop-Location
    }
}
finally {
    Pop-Location
}
