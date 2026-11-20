#!/usr/bin/env bash
# Before each scenario, on the client: Mosquitto persistence only for the persistent scenario,
# starting from an empty database. Mosquitto saves its whole in-memory database to disk on an
# interval (autosave_interval, default 1800 s) and at shutdown - a snapshot, not a write per
# message; that is what it offers. Run as the set's --setup-cmd.
#
#   ./mosquitto/set_mosquitto_persistence.sh --host 172.31.13.230 --for "$XMQ_SCENARIO"
set -euo pipefail
host="" scenario=""
while [ $# -gt 0 ]; do
    case "$1" in
        --host) host=$2; shift 2 ;;
        --for)  scenario=$2; shift 2 ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -n "$host" ] && [ -n "$scenario" ] || { echo "usage: $0 --host HOST --for SCENARIO" >&2; exit 2; }

case "$scenario" in
    *persistent*)
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "set -e
            sudo -n systemctl stop mosquitto 2>/dev/null || true
            printf 'persistence true\npersistence_location /var/lib/mosquitto/\n' | sudo -n tee /etc/mosquitto/conf.d/zz-loadtest-persistence.conf >/dev/null
            sudo -n rm -f /var/lib/mosquitto/mosquitto.db"
        echo "Mosquitto: persistence true (interval snapshot), empty database" ;;
    *)
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "sudo -n rm -f /etc/mosquitto/conf.d/zz-loadtest-persistence.conf"
        echo "Mosquitto: in memory" ;;
esac
