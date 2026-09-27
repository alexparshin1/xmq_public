#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# NanoMQ has no packagecloud/apt repo (unlike EMQX) — it's only published as
# GitHub release .deb assets, so we pin an exact version + checksum here
# rather than trusting whatever "latest" resolves to at install time.
NANOMQ_VERSION="0.25.3"
NANOMQ_DEB="nanomq-${NANOMQ_VERSION}-linux-amd64-full.deb"
NANOMQ_URL="https://github.com/nanomq/nanomq/releases/download/${NANOMQ_VERSION}/${NANOMQ_DEB}"
NANOMQ_SHA256="dcca5108cd26783d725db6de3bf94b21134d763570e3fde60fd7d45a452bff96"

TMP_DEB="$(mktemp --suffix=.deb)"
trap 'rm -f "$TMP_DEB"' EXIT

curl -fsSL -o "$TMP_DEB" "$NANOMQ_URL"
echo "${NANOMQ_SHA256}  ${TMP_DEB}" | sha256sum -c -

sudo dpkg -i "$TMP_DEB"
sudo apt-get install -f -y

# Deploy our config: MQTT TCP listener on 1881 (mosquitto has 1883, EMQX has
# 1882 on this host), and the ws listener moved off its 8083 default, which
# collides with EMQX's ws listener, to 18081.
if [ -f /etc/nanomq.conf ]; then
    sudo cp /etc/nanomq.conf "/etc/nanomq.conf.bak.$(date +%Y%m%d%H%M%S)"
fi
sudo cp "$SCRIPT_DIR/nanomq.conf" /etc/nanomq.conf

# NanoMQ ships no systemd unit, and its own -d/--daemon flag does not reliably
# detach in this build, so run it under systemd in foreground (Type=simple).
sudo cp "$SCRIPT_DIR/nanomq.service" /etc/systemd/system/nanomq.service
sudo systemctl daemon-reload

sudo systemctl enable nanomq
sudo systemctl restart nanomq
