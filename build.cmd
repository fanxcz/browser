@echo off
setlocal
echo BROWSER 4.0 / CONSOLE ONLY
echo ---------------------------
cmake -S . -B build
if errorlevel 1 exit /b 1
cmake --build build --config Release --target Browser --parallel
if errorlevel 1 exit /b 1
if not exist build\Release\Browser.exe (
    echo Browser.exe was not found after build.
    exit /b 1
)
build\Release\Browser.exe
