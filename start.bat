@echo off
cd /d "%~dp0"
setlocal enabledelayedexpansion

echo ============================================
echo   Auto Storage Tiering System v2.0
echo ============================================
echo.

if /i "%~1"=="--help" goto help
if /i "%~1"=="-h" goto help

:: ── Parse flags ──────────────────────────────
set "SERVER_ONLY="
set "CLIENT_ONLY="
set "ONCE="
set "SCAN_DIR="
set "NO_BROWSER="
set "WEB_ONLY="
set "NO_BUILD="
set "ARGS="

:parse
if "%~1"=="" goto kill_old
if /i "%~1"=="--server-only"  set "SERVER_ONLY=1"& shift & goto parse
if /i "%~1"=="--client-only"  set "CLIENT_ONLY=1"& shift & goto parse
if /i "%~1"=="--once"         set "ONCE=--once"& shift & goto parse
if /i "%~1"=="--scan"         set "SCAN_DIR=%~2"& shift & shift & goto parse
if /i "%~1"=="--no-browser"   set "NO_BROWSER=1"& shift & goto parse
if /i "%~1"=="--no-build"     set "NO_BUILD=1"& shift & goto parse
if /i "%~1"=="--web"          set "WEB_ONLY=1"& shift & goto parse
set "ARGS=!ARGS! %~1"& shift & goto parse

:: ── Step 1: Kill any previous server process ─
:kill_old
echo [STEP 1] Checking for previous processes...
tasklist /FI "IMAGENAME eq storage_tiering.exe" 2>nul | find /I "storage_tiering.exe" >nul
if %ERRORLEVEL% equ 0 (
    echo [CLEANUP] Stopping previous storage_tiering.exe...
    taskkill /F /IM storage_tiering.exe >nul 2>&1
    timeout /t 2 /nobreak >nul
    echo [CLEANUP] Previous process stopped.
) else (
    echo [CLEANUP] No previous process found.
)
echo.

:: ── Step 2: Build the C/C++ project ─────────
:build_project
if defined NO_BUILD goto check_bin
echo [STEP 2] Building C/C++ project...
echo.

if not exist "build" mkdir build
cd build

:: Run CMake configure
echo   [CMake] Configuring...
cmake .. -DCMAKE_BUILD_TYPE=Release >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo   [CMake] Re-configuring from scratch...
    cd ..
    rmdir /s /q build 2>nul
    mkdir build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    if %ERRORLEVEL% neq 0 (
        echo [ERROR] CMake configuration failed.
        cd ..
        pause & exit /b 1
    )
)

:: Build
echo   [Build] Compiling...
cmake --build . --config Release -j %NUMBER_OF_PROCESSORS%
if %ERRORLEVEL% neq 0 (
    echo [ERROR] Build failed.
    cd ..
    pause & exit /b 1
)

cd ..
echo [BUILD] Build successful!
echo.

:: ── Step 3: Start the system ────────────────
:check_bin
echo [STEP 3] Starting Storage Tiering System...
echo.

:: Locate binary
set "EXE="
if exist "build\Release\storage_tiering.exe" set "EXE=build\Release\storage_tiering.exe"
if exist "build\storage_tiering.exe"         set "EXE=build\storage_tiering.exe"

if not defined EXE (
    echo [ERROR] Binary not found after build.
    echo.
    echo   Expected one of:
    echo     build\Release\storage_tiering.exe
    echo     build\storage_tiering.exe
    echo.
    pause
    exit /b 1
)

:: Build server args
set "SERVER_ARGS="
if defined ONCE     set "SERVER_ARGS=!SERVER_ARGS! --once"
if defined SCAN_DIR set "SERVER_ARGS=!SERVER_ARGS! --scan !SCAN_DIR!"
if defined ARGS     set "SERVER_ARGS=!SERVER_ARGS!!ARGS!"

:: ── Web-only mode ─────────────────────────────
if defined WEB_ONLY (
    echo [WEB] Opening web dashboard in browser...
    start "" "http://127.0.0.1:3000/"
    goto end
)

:: ── Launch server ────────────────────────────
if not defined CLIENT_ONLY (
    echo [SERVER] Starting backend on port 3000...
    start "Storage Tiering Server" "%EXE%" %SERVER_ARGS%

    :: Wait for server to be ready
    echo [SERVER] Waiting for server to start...
    set "READY=0"
    for /L %%i in (1,1,20) do (
        if !READY! equ 0 (
            timeout /t 1 /nobreak >nul
            curl -s http://127.0.0.1:3000/health >nul 2>&1
            if !errorlevel! equ 0 set "READY=1"
        )
    )
    if !READY! equ 1 (
        echo [SERVER] Server is ready!
    ) else (
        echo [SERVER] Server may still be starting...
    )
)

:: ── Launch client ────────────────────────────
if not defined SERVER_ONLY (
    echo [CLIENT] Launching Desktop Client...
    if not exist "client\node_modules" (
        echo [SETUP] Installing client dependencies...
        pushd client
        call npm install
        popd
    )
    start "Storage Tiering Client" cmd /c "cd /d %~dp0client && npx electron ."
)

:: ── Open web dashboard ───────────────────────
if not defined NO_BROWSER (
    echo [WEB] Opening web dashboard in browser...
    start "" "http://127.0.0.1:3000/"
)

echo.
echo ============================================
echo   System is running!
echo ============================================
echo.
echo   Dashboard:  http://127.0.0.1:3000/
echo   File Server: http://127.0.0.1:3000/fileserver
echo   Health:     http://127.0.0.1:3000/health
echo.
echo   Login: admin / admin123
echo.
echo   Close the server window to stop.
echo ============================================
goto end

:help
echo Usage: start.bat [OPTIONS]
echo.
echo Launches the backend server and/or desktop client.
echo
echo Steps performed:
echo   1. Kill any previous storage_tiering.exe process
echo   2. Build the C/C++ project (unless --no-build)
echo   3. Start server + client
echo.
echo Options:
echo   --server-only    Launch only the backend server
echo   --client-only    Launch only the desktop client
echo   --once           Run a single tiering cycle, then exit
echo   --scan ^<dir^>     Scan a directory on startup
echo   --no-build       Skip the build step
echo   --web            Open web dashboard only (no server start)
echo   --no-browser     Don't auto-open browser
echo   -h, --help       Show this help
echo.
echo Examples:
echo   start.bat                    Full rebuild + start server + client
echo   start.bat --no-build         Start without rebuilding
echo   start.bat --server-only      Rebuild and start server only
echo   start.bat --client-only      Start client only
echo   start.bat --once             Run one cycle + launch client
echo   start.bat --web              Just open the web dashboard
echo   start.bat --no-browser       Start without opening browser
echo.

:end
endlocal
