#!/usr/bin/env bash
# Before each scenario, on the client: HiveMQ keeps sessions in memory, except for the persistent
# scenario, which gets file persistence starting from empty storage. Run as the set's --setup-cmd;
# the broker is (re)started by --restart-cmd after it.
#
#   ./hivemq/set_hivemq_persistence.sh --host 172.31.13.230 --for "$XMQ_SCENARIO"
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

mode=in-memory
case "$scenario" in *persistent*) mode=file ;; esac

ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" "set -e
    sudo -n systemctl stop hivemq hivemq-bench 2>/dev/null || true
    d=\$(ls -d /opt/hivemq-ce-* | tail -1)
    sudo -n cp \$d/conf/config.$mode.xml \$d/conf/config.xml
    sudo -n rm -rf \$d/data/*
    sudo -n chown hivemq:hivemq \$d/conf/config.xml"
echo "HiveMQ: $mode persistence, empty storage"
