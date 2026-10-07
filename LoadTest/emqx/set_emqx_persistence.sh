#!/usr/bin/env bash
# Before each scenario, on the client: EMQX durable sessions only for the persistent scenario,
# starting from empty durable storage; in memory otherwise (6.3's default). Run as the set's
# --setup-cmd; restart_broker.sh restarts EMQX after it and waits until it carries a message.
#
#   ./emqx/set_emqx_persistence.sh --host 172.31.13.230 --for "$XMQ_SCENARIO"
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

enable=false
case "$scenario" in *persistent*) enable=true ;; esac

ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "set -e
    sudo -n systemctl stop emqx 2>/dev/null || true
    sudo -n sed -i '/^## Load-test durable sessions/,/^## end durable sessions/d' /etc/emqx/emqx.conf
    printf '## Load-test durable sessions\ndurable_sessions { enable = $enable }\n## end durable sessions\n' | sudo -n tee -a /etc/emqx/emqx.conf >/dev/null
    data=\$(sed -nE 's/^[[:space:]]*data_dir[[:space:]]*=[[:space:]]*\"([^\"]+)\".*/\\1/p' /etc/emqx/emqx.conf | head -1)
    data=\${data:-/var/lib/emqx}
    sudo -n rm -rf \"\$data/ds\" \"\$data/durable_storage\" \"\$data/data/ds\" 2>/dev/null || true"
echo "EMQX: durable sessions $enable, empty durable storage"
