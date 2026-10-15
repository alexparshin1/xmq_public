#!/usr/bin/env bash
#
# TLS Point-To-Point for the brokers in turn: 50K messages/s for all of them, then 75K and 100K for
# those whose median latency was under the limit at the rate before. Ten minutes a scenario, the
# connect rate that of the scenario files, the broker restarted before each, and its RSS recorded.
#
#   ./tls_p2p_campaign.sh <results dir> [broker ...]       default: all five
#
# Brokers: xmq flashmq mosquitto emqx hivemq. Environment: HOST (ssh target of the broker machine,
# thinker11), ADDR (its address for the client), LIMIT_US (the gate, 100000).
#
set -u
OUT=${1:?results dir}; shift
BROKERS=${*:-xmq flashmq mosquitto emqx hivemq}
HOST=${HOST:-thinker11}
ADDR=${ADDR:-10.1.1.243}
LIMIT_US=${LIMIT_US:-100000}
LT=$(dirname "$(readlink -f "$0")")
mkdir -p "$OUT"
cd "$LT" || exit 1
say() { echo "$(date '+%F %T') $*" | tee -a "$OUT/chain.log"; }

# The versions, as installed on the bench (checked against the latest releases on 2026-10-15). On
# AWS they are set from what is installed there: VERSION_<broker>=... overrides.
version() {
    case $1 in
        xmq)       echo "${VERSION_xmq:-0.9.20}" ;;
        flashmq)   echo "${VERSION_flashmq:-1.27.2}" ;;
        mosquitto) echo "${VERSION_mosquitto:-2.1.2}" ;;
        emqx)      echo "${VERSION_emqx:-6.3.1}" ;;
        hivemq)    echo "${VERSION_hivemq:-CE-2026.5}" ;;
    esac
}

# unit, process to sample, plain port (the readiness probe), TLS port
spec() {
    case $1 in
        xmq)       echo "xmq_server   xmq_server  1884 8884" ;;
        flashmq)   echo "flashmq      flashmq     1885 8885" ;;
        mosquitto) echo "mosquitto    mosquitto   1884 8883" ;;
        emqx)      echo "emqx         beam.smp    1882 8882" ;;
        hivemq)    echo "hivemq-bench java        1886 8886" ;;
    esac
}

median_us() {  # the scenario's median, in microseconds, from its block file
    awk '/^Median/ {gsub("us","",$2); print $2; exit}' "$1" 2>/dev/null
}

for broker in $BROKERS; do
    read -r unit proc plain tls <<< "$(spec "$broker")"
    previous=0
    for rate in 50 75 100; do
        scenario=Point-To-Point-TLS-${rate}K.json
        if [ "$rate" != 50 ] && { [ -z "$previous" ] || [ "$previous" -ge "$LIMIT_US" ]; }; then
            say "$broker ${rate}K: not run - the median at the rate before was ${previous:-unknown} us, the limit is $LIMIT_US"
            continue
        fi
        set_file=$LT/.tls-p2p-set.txt            # names resolve beside the set file
        echo "$scenario" > "$set_file"
        cp "$LT/$scenario" "$OUT/$scenario"
        say "$broker ${rate}K: started"
        XMQ_SCN_EXTRA="--encrypted" ./run_scenario_set.sh \
            --restart-cmd "./restart_broker.sh --host $HOST --unit $unit --port $plain -u user -P secret" \
            --wait-for "$ADDR:$plain" --stats-host "$HOST" --stats-process "$proc" --nic enp86s0 \
            --out "$OUT/$broker-${rate}K" --record "$OUT/record-$broker-${rate}K.txt" --broker "$broker" --version "$(version "$broker")" \
            --server-note "$HOST over TLS (port $tls), self-signed certificate, no client verification" \
            "$set_file" -- \
            --subnet 10.1.1 --first 150 --count 29 -h "$ADDR" -p "$tls" -u user -P secret \
            > "$OUT/$broker-${rate}K.console" 2>&1
        rc=$?
        previous=$(median_us "$OUT/$broker-${rate}K/${scenario%.json}.block")
        say "$broker ${rate}K: exit $rc, median ${previous:-none} us"
    done
done
ssh "$HOST" "sudo -n systemctl stop xmq_server flashmq mosquitto emqx hivemq-bench 2>/dev/null; sudo -n systemctl start xmq_server"
say "finished"
rm -f "$LT/.tls-p2p-set.txt"
touch "$OUT/DONE"
