$ErrorActionPreference = "Stop"

Write-Host ""
Write-Host "BROWSER 4.0 / CONSOLE ONLY" -ForegroundColor Cyan
Write-Host "---------------------------" -ForegroundColor DarkGray

Write-Host "[1/2] Configure CMake and fetch Lexbor..." -ForegroundColor Yellow
& cmake -S . -B build
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed. Check internet access and dependency download."
}

Write-Host "[2/2] Build Browser.exe..." -ForegroundColor Yellow
& cmake --build build --config Release --target Browser --parallel
if ($LASTEXITCODE -ne 0) {
    throw "Compilation failed. Scroll up to the first C++ error."
}

$exe = Join-Path (Get-Location) "build\Release\Browser.exe"
if (-not (Test-Path $exe)) {
    throw "Browser.exe was not created at $exe"
}

Write-Host ""
Write-Host "Created: $exe" -ForegroundColor Green
Write-Host "Starting console Browser..." -ForegroundColor Green
& $exe
