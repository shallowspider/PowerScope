$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $root

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw "CMake was not found. Install Visual Studio 2022 Desktop development with C++ and CMake tools for Windows."
}

$buildDir = Join-Path $root "build"
$generator = "Visual Studio 17 2022"

Write-Host "Configuring PowerScope..."
& cmake -S $root -B $buildDir -G $generator -A x64
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}

Write-Host "Building PowerScope (Release)..."
& cmake --build $buildDir --config Release --parallel
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed with exit code $LASTEXITCODE."
}

$sourceExe = Join-Path $buildDir "Release\PowerScope.exe"
$targetExe = Join-Path $root "PowerScope.exe"

if (-not (Test-Path -LiteralPath $sourceExe)) {
    throw "Build finished, but PowerScope.exe was not found at: $sourceExe"
}

Copy-Item -LiteralPath $sourceExe -Destination $targetExe -Force

Write-Host ""
Write-Host "Build succeeded: $targetExe" -ForegroundColor Green
Write-Host "Run it with: .\PowerScope.exe"
