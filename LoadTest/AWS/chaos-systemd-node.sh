#!/bin/bash
#
# The node side of a chaos run on the AWS stand, where every broker is a systemd service on its
# own machine and all four listen on the same MQTT port.
#
# chaos_run.sh asks the machine that starts the run to `mark` and `check-logs`, and each node's
# host to `crash <node>` and `start <node>` over ssh. On the test cluster that is one script,
# test_cluster.sh, which owns processes in $CLUSTER_DIR. Here the processes are systemd units, so
# this file is installed as ~/cluster/test_cluster.sh on every node and on the load client and
# answers the same four commands:
#
#   crash <node>   SIGKILL the broker - no SIGTERM, so no handover and no lease released - and
#                  stop the unit so Restart=always does not bring it back inside the down window
#   start <node>   start the unit and wait for the MQTT port
#   mark           remember where the journal ends, so check-logs sees only this run
#   check-logs     what the nodes logged since the mark: a crash, or a node not running, fails it
#
# mark and check-logs act locally unless ~/chaos-aws/nodes.list exists, which it does on the load
# client only: there they fan out over ssh and aggregate the answers.
#
# Install: scp this file to ~/cluster/test_cluster.sh on each node and on the client, then put the
# four private addresses in ~/chaos-aws/nodes.list on the client.
#
set -u

SERVICE=${CHAOS_SERVICE:-xmq_server}
PORT=${CHAOS_PORT:-1884}
MARK=${CHAOS_MARK:-/var/tmp/xmq-chaos.mark}
NODES_LIST=$HOME/chaos-aws/nodes.list
CRASH_PATTERN='terminate called|Segmentation fault|Aborted|core dumped|AddressSanitizer|ThreadSanitizer|std::terminate'

usage()
{
    sed -n '/^#   crash/,/^# mark and check-logs/p' "$0" | sed 's/^# \{0,1\}//'
}

local_mark()
{
    sudo -n journalctl -u "$SERVICE" -n 0 --show-cursor -o cat 2>/dev/null |
        sed -n 's/^-- cursor: //p' > "$MARK"
    [ -s "$MARK" ]
}

local_check_logs()
{
    local cursor found failed=0 offline online
    cursor=$(cat "$MARK" 2>/dev/null)
    if [ -n "$cursor" ]; then
        found=$(sudo -n journalctl -u "$SERVICE" --after-cursor="$cursor" -o cat --no-pager 2>/dev/null)
    else
        found=$(sudo -n journalctl -u "$SERVICE" --since "-1 hour" -o cat --no-pager 2>/dev/null)
    fi

    if printf '%s\n' "$found" | grep -qE "$CRASH_PATTERN"; then
        echo "$(hostname): CRASHED"
        printf '%s\n' "$found" | grep -E "$CRASH_PATTERN" | head -5 | sed 's/^/    /'
        failed=1
    fi

    if [ "$(systemctl is-active "$SERVICE" 2>/dev/null)" != active ]; then
        echo "$(hostname): not running"
        failed=1
    fi

    offline=$(printf '%s\n' "$found" | grep -c 'Cluster-offline')
    online=$(printf '%s\n' "$found" | grep -c 'Cluster-online')
    if [ "$offline" -gt 0 ]; then
        echo "$(hostname): went cluster-offline $offline time(s), came back $online"
    fi
    return $failed
}

crash_node()
{
    if [ "$(systemctl is-active "$SERVICE" 2>/dev/null)" != active ]; then
        echo "$(hostname): $SERVICE is not running" >&2
        return 3
    fi
    # SIGKILL first: the cluster must find out through its own liveness, not through a goodbye.
    sudo -n systemctl kill -s SIGKILL --kill-who=all "$SERVICE"
    # Then stop the unit, so Restart=always does not defeat the down window.
    sudo -n systemctl stop "$SERVICE"
}

start_node()
{
    sudo -n systemctl start "$SERVICE" || return 1
    for _ in $(seq 80); do
        timeout 1 bash -c "</dev/tcp/127.0.0.1/$PORT" 2>/dev/null && return 0
        sleep 0.25
    done
    echo "$(hostname): $SERVICE did not open $PORT" >&2
    return 1
}

fan_out()
{
    local command=$1 ip result rc=0
    for ip in $(cat "$NODES_LIST"); do
        if result=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o ConnectTimeout=10 \
                        alexeyp@"$ip" "~/cluster/test_cluster.sh $command" 2>&1); then
            :
        else
            rc=1
        fi
        # Each node answers "no crash, every node running" for itself; the client says it once.
        printf '%s\n' "$result" | grep -v '^node logs: no crash, every node running' || true
    done
    if [ "$command" = "check-logs" ] && [ $rc -eq 0 ]; then
        echo "node logs: no crash, every node running"
    fi
    return $rc
}

case "${1:-}" in
    crash)      shift; crash_node "${1:-}" ;;
    start)      shift; start_node "${1:-}" ;;
    stop)       shift; crash_node "${1:-}" ;;
    mark)
        if [ -f "$NODES_LIST" ]; then fan_out mark; else local_mark; fi ;;
    check-logs)
        if [ -f "$NODES_LIST" ]; then fan_out check-logs; else local_check_logs; fi ;;
    -h|--help)  usage ;;
    *)          usage; exit 2 ;;
esac
