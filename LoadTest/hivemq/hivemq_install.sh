#!/usr/bin/env bash
# Installs HiveMQ CE on the load-test server as it runs on the home bench (thinker11): the same
# distribution, JVM options, connection limit and port (1886), as a systemd service that is not
# started at boot - the scenario set starts and stops it like every other broker.
#
#   sudo ./hivemq_install.sh hivemq-ce-2026.5.zip
#
# The scenario set switches between config.in-memory.xml and config.file.xml before each scenario
# (set_hivemq_persistence.sh); file persistence only for the persistent scenario.
set -euo pipefail

zip=${1:?usage: $0 hivemq-ce-<version>.zip}
here=$(cd "$(dirname "$0")" && pwd)
name=$(basename "$zip" .zip)
target=/opt/$name

command -v java >/dev/null || apt-get install -y openjdk-21-jre-headless
command -v unzip >/dev/null || apt-get install -y unzip

id hivemq >/dev/null 2>&1 || useradd --system --home-dir "$target" --shell /usr/sbin/nologin hivemq
if [ ! -d "$target" ]; then
    unzip -q "$zip" -d /opt
fi
install -m 644 "$here/config.in-memory.xml" "$target/conf/config.in-memory.xml"
install -m 644 "$here/config.file.xml" "$target/conf/config.file.xml"
install -m 644 "$here/config.in-memory.xml" "$target/conf/config.xml"
chown -R hivemq:hivemq "$target"
chmod +x "$target/bin/run.sh"

sed "s|/opt/hivemq-ce-2026.5|$target|g" "$here/hivemq.service" > /etc/systemd/system/hivemq.service
systemctl daemon-reload
systemctl disable hivemq >/dev/null 2>&1 || true
echo "HiveMQ installed in $target; java: $(java -version 2>&1 | head -1)"
