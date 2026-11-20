#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

curl -fsSL https://packagecloud.io/install/repositories/emqx/emqx/script.deb.sh | sudo bash
sudo apt-get install -y emqx

# Deploy our emqx.conf (moves the MQTT TCP listener to 1882 so EMQX can coexist
# with mosquitto on 1883) instead of leaving the package defaults in place.
if [ -f /etc/emqx/emqx.conf ]; then
    sudo cp /etc/emqx/emqx.conf "/etc/emqx/emqx.conf.bak.$(date +%Y%m%d%H%M%S)"
fi
sudo cp "$SCRIPT_DIR/emqx.conf" /etc/emqx/emqx.conf

sudo systemctl enable emqx
sudo systemctl restart emqx
