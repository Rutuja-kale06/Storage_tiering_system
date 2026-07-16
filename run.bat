@echo off
cd /d "%~dp0"
setlocal enabledelayedexpansion

chcp 65001 >nul 2>&1

set "EXE=build\Release\storage_tiering.exe"
if not exist "!EXE!" set "EXE=build\storage_tiering.exe"

if /i "%~1"=="--build"    goto build
if /i "%~1"=="--rebuild"  goto rebuild
if /i "%~1"=="--help"     goto help
if /i "%~1"=="-h"         goto help
if /i "%~1"=="--once"     goto once
if /i "%~1"=="--scan"     goto scan
if /i "%~1"=="--client"   goto client
if /i "%~1"=="--demo"     goto demo
if /i "%~1"=="--start"    goto start_all
if /i "%~1"=="--extent"   goto extent
if /i "%~1"=="--extent-scan" goto extent_scan
if /i "%~1"=="--extent-demo" goto extent_demo
if /i "%~1"=="--test"     goto test

:check_exe
if exist "%EXE%" goto kill_and_run

:build
echo ============================================
echo   Building Storage Tiering System v2.0
echo ============================================
if not exist "build" mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
if %ERRORLEVEL% neq 0 (
    cd ..
    rmdir /s /q build
    mkdir build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    if %ERRORLEVEL% neq 0 (
        echo [ERROR] CMake configuration failed.
        pause & exit /b 1
    )
)
cmake --build . --config Release
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed.
    pause & exit /b 1
)
cd ..
set "EXE=build\Release\storage_tiering.exe"
if not exist "!EXE!" set "EXE=build\storage_tiering.exe"
if /i "%~1"=="--build" goto kill_and_run
goto end

:rebuild
if exist "build" rmdir /s /q build
goto build

:kill_and_run
tasklist /FI "IMAGENAME eq storage_tiering.exe" 2>nul | find /I "storage_tiering.exe" >nul
if %ERRORLEVEL% equ 0 (
    echo [CLEANUP] Stopping previous storage_tiering.exe...
    taskkill /F /IM storage_tiering.exe >nul 2>&1
    timeout /t 2 /nobreak >nul
)
goto run

:run
echo ============================================
echo   Storage Tiering System v2.0
echo ============================================
echo   REST API:  http://localhost:3000/
echo   Login:     admin / admin123
echo ============================================
echo.
start "" "http://localhost:3000/"
"%EXE%" %*
goto end

:once
"%EXE%" --once
goto end

:scan
if "%~2"=="" (
    echo [ERROR] --scan requires a directory path
    echo   Usage: run.bat --scan "C:\path\to\dir"
    exit /b 1
)
"%EXE%" --scan "%~2"
goto end

:client
if not exist "client\node_modules" (
    echo [SETUP] Installing client dependencies...
    pushd client
    call npm install
    popd
)
start "Storage Tiering Client" cmd /c "cd /d %~dp0client && npx electron ."
goto end

:demo
echo [DEMO] Starting interactive mode with a test scan...
"%EXE%" --scan "%USERPROFILE%\Documents"
goto end

:start_all
start "" cmd /c "start.bat"
goto end

:extent
echo ============================================
echo   Extent-Based Sub-LUN Tiering Mode
echo ============================================
echo   This runs the enterprise-grade extent-based
echo   tiering engine with EMA heat maps, I/O
echo   profiling, hysteresis, wear leveling,
echo   and NVRAM write coalescing.
echo ============================================
echo.
echo   Enable in config/tiering_config.json:
echo     "extent_tiering": { "enabled": true }
echo ============================================
echo.
"%EXE%" --extent
goto end

:extent_scan
if "%~2"=="" (
    echo [ERROR] --extent-scan requires a volume path
    echo   Usage: run.bat --extent-scan "C:\path\to\volume"
    echo.
    echo   This registers the path as a volume, partitions
    echo   it into 256 MB extents, and starts monitoring.
    exit /b 1
)
echo [EXTENT] Registering volume: %~2
"%EXE%" --extent --register-volume "%~2"
goto end

:extent_demo
echo ============================================
echo   Extent Tiering Demo
echo ============================================
echo   This creates a simulated volume, generates
echo   synthetic I/O patterns, runs the heat map
echo   engine, and shows relocation decisions.
echo ============================================
echo.
"%EXE%" --extent --demo
goto end

:test
echo ============================================
echo   Running Test Suite
echo ============================================
pushd build
ctest --output-on-failure -C Release
if %ERRORLEVEL% neq 0 (
    echo.
    echo [INFO] Some tests failed — see output above.
    echo   The extent-tiering tests validate:
    echo   - EMA heat score calculation
    echo   - Hysteresis cost-benefit gate
    echo   - Sequential vs random I/O profiling
    echo   - Wear leveling balance
    echo   - NVRAM cache write coalescing
)
popd
goto end

:help
echo ============================================
echo   Storage Tiering System v2.0 — run.bat
echo ============================================
echo.
echo Usage: run.bat [OPTIONS]
echo.
echo --- General Commands ---
echo   --build           Build from source ^(if needed, start server^)
echo   --rebuild         Clean build from scratch
echo   --start           Start server + desktop client + browser ^(calls start.bat^)
echo   --once            Run a single tiering cycle and exit
echo   --scan ^<dir^>     Scan a directory and enter interactive mode
echo   --client          Launch Electron desktop client ^(no server^)
echo   --demo            Quick demo: scan Documents and start
echo   --test            Build and run unit tests
echo   -h, --help        Show this help
echo.
echo --- Extent Tiering (Sub-LUN) ---
echo   --extent          Run in extent-based tiering mode
echo   --extent-scan ^<vol^>  Register a volume and partition into extents
echo   --extent-demo     Demo with simulated volume + synthetic I/O
echo.
echo Examples:
echo   run.bat                    Build ^(if needed^) and start server
echo   run.bat --build            Force build, then start
echo   run.bat --start            Start everything ^(server + client + browser^)
echo   run.bat --once             Run one tiering cycle
echo   run.bat --scan "D:\Data"   Scan a folder
echo   run.bat --extent           Enterprise extent tiering mode
echo   run.bat --extent-scan "D:\"  Register D:\ as a volume
echo   run.bat --test             Run all unit tests
echo.
echo You can also use start.bat directly for more options:
echo   start.bat                  Server + client + browser
echo   start.bat --server-only    Server only
echo   start.bat --client-only    Client only
echo.
goto end

:end
endlocal
