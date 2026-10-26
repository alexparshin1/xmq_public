#!/bin/bash
#
# A chaos run on the test cluster: nodes are killed outright - no handover, no lease released - and
# brought back while the cluster carries a steady publish/subscribe load. Every cycle takes one node
# away for a while, checks that the survivors keep serving and keep delivering, brings it back and
# checks that it serves again. Each node is taken in turn, so the coordinator and both load nodes
# are among the victims; a node that comes back is tested for delivery, not only for a listening port.
#
# The timeline is fixed, not random: the same nodes are killed at the same seconds every run, so a
# failure can be looked at again. The seed of a run is its node order and its clock.
#
# Usage:
#   chaos_run.sh [--nodes "node1@host node2@host node3@host"] [--duration 600] [--cycle 90] [--down 20]
#
# The defaults come from the test cluster's cluster.env. At least three nodes are required: with two,
# a crash leaves a cluster too small to prove that anything but the survivor still works.
#
# The generated test file is left in $CLUSTER_DIR/chaos-run.generated.json for the run's record.
# Any other argument is passed to xmq_scn_cluster, so --dry-run and --keep-cluster work as there.
#
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
CLUSTER_DIR=${CLUSTER_DIR:-$HOME/cluster.run}
DURATION=600
CYCLE=90
DOWN=20

usage() { sed -n '/^# Usage:/,/^# The defaults/p' "$0" | sed 's/^# \{0,1\}//'; }

while [ $# -gt 0 ]; do
    case $1 in
        --nodes)    NODES=$2; shift 2 ;;
        --duration) DURATION=$2; shift 2 ;;
        --cycle)    CYCLE=$2; shift 2 ;;
        --down)     DOWN=$2; shift 2 ;;
        -h|--help)  usage; exit 0 ;;
        *)          break ;;
    esac
done

# cluster.env holds `key=value` with spaces in the values, so it is read, not sourced.
if [ -f "$CLUSTER_DIR/cluster.env" ]; then
    while IFS='=' read -r key value; do
        case $key in '' | \#*) continue ;; esac
        case $key in
            NODES|REDIS_URI|REDIS_HOST|REDIS_PORT|REDIS_DB|CLIENT_USER|CLIENT_PASSWORD)
                [ -n "${!key+x}" ] || printf -v "$key" '%s' "$value"
                ;;
        esac
    done < "$CLUSTER_DIR/cluster.env"
fi

NODES=${NODES:-}
if [ -z "$NODES" ]; then
    echo "No nodes: name them with --nodes or run test_cluster.sh up first." >&2
    exit 2
fi
REDIS_URI=${REDIS_URI:-redis://${REDIS_HOST:-farm-host}:${REDIS_PORT:-6379}/${REDIS_DB:-4}}
CLIENT_USER=${CLIENT_USER:-user}
CLIENT_PASSWORD=${CLIENT_PASSWORD:-User-Secret1}

LOCAL_HOST=$(hostname)
NAMES=()
HOSTS=()
for item in $NODES; do
    name=${item%%@*}
    host=${item#*@}
    [ "$host" = "$item" ] && host=$LOCAL_HOST
    NAMES+=("$name")
    HOSTS+=("$host")
done
COUNT=${#NAMES[@]}
if [ "$COUNT" -lt 3 ]; then
    echo "A chaos run needs at least three nodes; $COUNT given: $NODES" >&2
    exit 2
fi
if [ "$CYCLE" -le $((DOWN + 50)) ]; then
    echo "The cycle ($CYCLE s) must be longer than the crash ($DOWN s) plus the recovery window." >&2
    exit 2
fi

# The node commands run where the node lives: test_cluster.sh does the same comparison.
node_command() {
    local host=$1 command=$2
    if [ "$host" = "$LOCAL_HOST" ]; then
        printf '~/cluster/test_cluster.sh %s' "$command"
    else
        printf 'ssh %s ~/cluster/test_cluster.sh %s' "$host" "$command"
    fi
}

# Place the crashes so that the last recovery window still ends before the load does.
CYCLES=0
probe=30
while [ $((probe + DOWN + 45)) -le $((DURATION - 40)) ]; do
    CYCLES=$((CYCLES + 1))
    probe=$((probe + CYCLE))
done
if [ "$CYCLES" -lt 1 ]; then
    echo "A duration of ${DURATION}s fits no crash cycle: give it at least $((30 + DOWN + 45 + 40))s." >&2
    exit 2
fi

OUT=$CLUSTER_DIR/chaos-run.generated.json
# The load lasts exactly as long as the run, so that it is the timeline, not the scenario's own
# default, that decides when a chaos run ends. The generated copy is kept with the plan: a record of
# what was actually run against the cluster.
LOAD=$CLUSTER_DIR/chaos-load.generated.json
sed "s/\"duration_sec\": [0-9]\+/\"duration_sec\": $DURATION/" "$HERE/chaos-p2p.json" > "$LOAD"
STEPS=$(mktemp)
CHECKS=$(mktemp)
NODE_LIST=$(mktemp)
trap 'rm -f "$STEPS" "$CHECKS" "$NODE_LIST"' EXIT

step_first=1
add_step() {
    [ $step_first = 1 ] || printf ',\n' >> "$STEPS"
    step_first=0
    printf '    %s' "$1" >> "$STEPS"
}

check_first=1
add_check() {
    [ $check_first = 1 ] || printf ',\n' >> "$CHECKS"
    check_first=0
    printf '    %s' "$1" >> "$CHECKS"
}

node_first=1
for index in "${!NAMES[@]}"; do
    name=${NAMES[$index]}
    host=${HOSTS[$index]}
    [ $node_first = 1 ] || printf ',\n' >> "$NODE_LIST"
    node_first=0
    stop=$(node_command "$host" 'crash {node}')
    start=$(node_command "$host" 'start {node}')
    printf '    { "name": "%s", "host": "%s", "stop": "%s", "start": "%s" }' \
        "$name" "$host" "$stop" "$start" >> "$NODE_LIST"
done

# Every node is asked to serve, and every ordered pair to carry a message: the timeline uses some
# of them, and the runner runs the rest at the end, when every node has to be back.
for name in "${NAMES[@]}"; do
    add_check "$(printf '{ "name": "%s serves", "kind": "mqtt", "action": "connect", "node": "%s" }' "$name" "$name")"
done
for from in "${NAMES[@]}"; do
    for to in "${NAMES[@]}"; do
        [ "$from" = "$to" ] && continue
        add_check "$(printf '{ "name": "a message crosses %s to %s", "kind": "mqtt", "action": "deliver", "publish-node": "%s", "subscribe-node": "%s", "topic": "cluster/chaos/%s-%s", "payload": "chaos", "timeout": 20, "settle": 3 }' \
            "$from" "$to" "$from" "$to" "$from" "$to")"
    done
done
add_check "$(printf '{ "name": "a coordinator is leading", "kind": "redis", "uri": "%s", "command": "GET cluster:coordinator", "nonempty": true }' "$REDIS_URI")"
add_check "$(printf '{ "name": "the cluster still has every member", "kind": "redis", "uri": "%s", "command": "ZCARD cluster:members", "min": %d }' "$REDIS_URI" "$COUNT")"

at=30
for ((cycle = 0; cycle < CYCLES; cycle++)); do
    victim=${NAMES[$((cycle % COUNT))]}
    survivors=()
    for name in "${NAMES[@]}"; do
        [ "$name" = "$victim" ] || survivors+=("$name")
    done

    add_step "$(printf '{ "at": %d, "action": "stop", "node": "%s" }' "$at" "$victim")"
    for name in "${survivors[@]}"; do
        add_step "$(printf '{ "at": %d, "action": "check", "check": "%s serves", "every": 5, "until": %d }' \
            $((at + 5)) "$name" $((at + DOWN - 5)))"
    done
    add_step "$(printf '{ "at": %d, "action": "check", "check": "a message crosses %s to %s", "every": 10, "until": %d }' \
        $((at + 5)) "${survivors[0]}" "${survivors[1]}" $((at + DOWN - 5)))"

    add_step "$(printf '{ "at": %d, "action": "start", "node": "%s" }' $((at + DOWN)) "$victim")"
    add_step "$(printf '{ "at": %d, "action": "check", "check": "%s serves", "every": 5, "until": %d }' \
        $((at + DOWN + 30)) "$victim" $((at + DOWN + 45)))"
    add_step "$(printf '{ "at": %d, "action": "check", "check": "a message crosses %s to %s", "every": 10, "until": %d }' \
        $((at + DOWN + 30)) "${survivors[0]}" "$victim" $((at + DOWN + 40)))"

    at=$((at + CYCLE))
done

{
    printf '{\n'
    printf '  "name": "chaos: %d crashes over %d seconds",\n' "$CYCLES" "$DURATION"
    printf '  "comments": [\n'
    printf '    "Generated by LoadTest/Cluster/chaos_run.sh: nodes killed outright, one at a time,",\n'
    printf '    "under the chaos-p2p load. Survivors must keep serving and delivering while a node",\n'
    printf '    "is gone, and every victim must carry traffic again afterwards."\n'
    printf '  ],\n'
    printf '  "cluster": "%s",\n' "$NODES"
    printf '  "server": { "username": "%s", "password": "%s" },\n' "$CLIENT_USER" "$CLIENT_PASSWORD"
    printf '  "load": {\n'
    printf '    "scenario": "%s",\n' "$LOAD"
    printf '    "groups": [ { "name": "publishers", "node": "%s" }, { "name": "subscribers", "node": "%s" } ]\n' "${NAMES[0]}" "${NAMES[1]}"
    printf '  },\n'
    printf '  "nodes": [\n'
    cat "$NODE_LIST"
    printf '\n  ],\n'
    printf '  "before": [ "~/cluster/test_cluster.sh mark" ],\n'
    printf '  "timeline": [\n'
    cat "$STEPS"
    printf '\n  ],\n'
    printf '  "after": [ "~/cluster/test_cluster.sh check-logs" ],\n'
    printf '  "checks": [\n'
    cat "$CHECKS"
    printf '\n  ]\n'
    printf '}\n'
} > "$OUT"

echo "chaos: ${CYCLES} crashes over ${DURATION}s, one node down ${DOWN}s per ${CYCLE}s cycle"
echo "nodes: $NODES"
echo "plan:  $OUT"
exec "${XMQ_SCN_CLUSTER:-xmq_scn_cluster}" --scenario "$OUT" "$@"
