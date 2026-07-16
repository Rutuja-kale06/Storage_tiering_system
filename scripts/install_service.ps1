# Storage Tiering Service — Windows Installation Script
# Run as Administrator: powershell -ExecutionPolicy Bypass .\install_service.ps1

$ServiceName = "StorageTieringService"
$ServiceDisplayName = "Auto Storage Tiering Service"
$BinaryPath = "$PSScriptRoot\..\build\Release\storage_tiering.exe"

if (-NOT ([Security.Principal.WindowsPrincipal] [Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole] "Administrator")) {
    Write-Host "This script must be run as Administrator!" -ForegroundColor Red
    exit 1
}

if (-NOT (Test-Path $BinaryPath)) {
    Write-Host "Binary not found at $BinaryPath" -ForegroundColor Red
    Write-Host "Build the project first in Release mode"
    exit 1
}

# Check if service already exists
$existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($existing) {
    Write-Host "Service '$ServiceName' already exists. Stopping and removing..."
    Stop-Service -Name $ServiceName -Force -ErrorAction SilentlyContinue
    sc.exe delete $ServiceName
    Start-Sleep -Seconds 2
}

# Create the service
Write-Host "Creating service '$ServiceDisplayName'..." -ForegroundColor Green
New-Service -Name $ServiceName `
            -DisplayName $ServiceDisplayName `
            -Description "Automatically tiers files across HOT/WARM/COLD/ARCHIVE storage tiers based on access patterns, file type, and size." `
            -BinaryPathName "`"$BinaryPath`" --daemon" `
            -StartupType Automatic

# Configure recovery options
sc.exe failure $ServiceName reset= 86400 actions= restart/5000/restart/10000/restart/30000

Start-Service -Name $ServiceName
Write-Host "Service started successfully!" -ForegroundColor Green
Write-Host "Monitor logs at: config/logs/storage_tiering.log" -ForegroundColor Yellow
