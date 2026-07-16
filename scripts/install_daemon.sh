#!/bin/bash
# Storage Tiering Daemon — Linux systemd Installation Script
# Run as root: sudo ./install_daemon.sh

set -e

SERVICE_NAME="storage-tiering"
BINARY_PATH="/usr/local/bin/storage_tiering"
CONFIG_DIR="/etc/storage_tiering"
LOG_DIR="/var/log/storage_tiering"
DATA_DIR="/var/lib/storage_tiering"

if [ "$EUID" -ne 0 ]; then
    echo "Please run as root: sudo $0"
    exit 1
fi

# Create required directories
mkdir -p "$CONFIG_DIR" "$LOG_DIR" "$DATA_DIR"

# Install binary
if [ ! -f "$BINARY_PATH" ]; then
    if [ -f "../build/storage_tiering" ]; then
        cp "../build/storage_tiering" "$BINARY_PATH"
    else
        echo "Binary not found. Build first: cd .. && cmake -B build && cmake --build build"
        exit 1
    fi
fi

chmod +x "$BINARY_PATH"

# Install config files
if [ -d "../config" ]; then
    cp -r ../config/* "$CONFIG_DIR/"
fi

# Create systemd service unit
cat > /etc/systemd/system/${SERVICE_NAME}.service << EOF
[Unit]
Description=Auto Storage Tiering Service
Documentation=https://github.com/example/storage-tiering
After=network.target local-fs.target

[Service]
Type=simple
User=root
ExecStart=${BINARY_PATH} --daemon
WorkingDirectory=${DATA_DIR}
Restart=on-failure
RestartSec=10
StandardOutput=append:${LOG_DIR}/output.log
StandardError=append:${LOG_DIR}/error.log
Environment=CONFIG_PATH=${CONFIG_DIR}/tiering_config.json
LimitNOFILE=65536

# Security hardening
NoNewPrivileges=true
ProtectSystem=full
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF

# Reload systemd and enable service
systemctl daemon-reload
systemctl enable ${SERVICE_NAME}
systemctl start ${SERVICE_NAME}

echo "Daemon installed and started."
echo "  Status: systemctl status ${SERVICE_NAME}"
echo "  Logs:   journalctl -u ${SERVICE_NAME} -f"
echo "  Config: ${CONFIG_DIR}/tiering_config.json"
