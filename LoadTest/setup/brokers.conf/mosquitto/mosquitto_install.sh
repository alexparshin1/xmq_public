#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

sudo apt-get update
sudo apt-get install -y mosquitto mosquitto-clients

# Deploy our config: mosquitto keeps the default 1883 (NanoMQ moved to 1881, EMQX to
# 1882, to coexist on the same host — see ../nanomq/nanomq_install.sh).
if [ -f /etc/mosquitto/mosquitto.conf ]; then
    sudo cp /etc/mosquitto/mosquitto.conf "/etc/mosquitto/mosquitto.conf.bak.$(date +%Y%m%d%H%M%S)"
fi
sudo cp "$SCRIPT_DIR/mosquitto.conf" /etc/mosquitto/mosquitto.conf

# Drop-in for the FD-limit fix — see mosquitto-override.conf for why this can't just
# be left to systemd/OS defaults.
sudo mkdir -p /etc/systemd/system/mosquitto.service.d
sudo cp "$SCRIPT_DIR/mosquitto-override.conf" /etc/systemd/system/mosquitto.service.d/override.conf
sudo systemctl daemon-reload

sudo systemctl enable mosquitto
sudo systemctl restart mosquitto
