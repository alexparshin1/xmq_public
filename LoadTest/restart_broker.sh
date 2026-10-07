#!/usr/bin/env bash
# Restart one broker on the load-test server, all others stopped, and wait until it carries a
# message: an open TCP port is not readiness - EMQX accepts on it before its storage is up, and a
# JVM broker such as HiveMQ listens before it serves. Used as run_scenario_set's --restart-cmd.
#
#   ./restart_broker.sh --host 172.31.13.230 --unit emqx --port 1882 [-u USER -P PASSWORD] [--persistent]
#
# --persistent also waits for a persistent session (MQTT 5, clean start off) to be accepted: EMQX with
# durable sessions carries a clean-session message long before it accepts a persistent one, and
# answers "Server not available" to those in between. It is implied when \$XMQ_SCENARIO, which
# run_scenario_set.sh sets, names a persistent scenario.
set -u
host="" unit="" port="" user="" password="" persistent=0
while [ $# -gt 0 ]; do
    case "$1" in
        --host) host=$2; shift 2 ;;
        --unit) unit=$2; shift 2 ;;
        --port) port=$2; shift 2 ;;
        -u)     user=$2; shift 2 ;;
        -P)     password=$2; shift 2 ;;
        --persistent) persistent=1; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
done
[ -n "$host" ] && [ -n "$unit" ] && [ -n "$port" ] || { echo "usage: $0 --host H --unit U --port P [-u U -P P]" >&2; exit 2; }

ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" \
    "sudo -n systemctl stop xmq_server flashmq mosquitto emqx nanomq hivemq hivemq-bench 2>/dev/null; sleep 2; sudo -n systemctl start $unit" || exit 1

credentials=()
[ -n "$user" ] && credentials+=(-u "$user" -P "$password")
# A persistent session that expires a second after it disconnects, so nothing of it stays behind.
# The subscriber only: the publisher stays an ordinary client of its own.
[[ "${XMQ_SCENARIO:-}" == *persistent* ]] && persistent=1
session=""
[ "$persistent" = 1 ] && session="-V 5 -c -x 1 -i xmq-ready-persistent"
for _ in $(seq 1 90); do
    # shellcheck disable=SC2086
    if timeout 10 bash -c '
            host=$1 port=$2 session=$3; shift 3
            xmq_sub -h "$host" -p "$port" "$@" $session -t xmq/ready -C 1 -W 6 --quiet & sp=$!
            sleep 1
            xmq_pub -h "$host" -p "$port" "$@" -t xmq/ready -m 1 -q 1 --quiet >/dev/null 2>&1
            wait "$sp"' _ "$host" "$port" "$session" "${credentials[@]}" >/dev/null 2>&1; then
        exit 0
    fi
done
echo "$unit did not carry a pub/sub round-trip on $host:$port after ~180s" >&2
exit 1
