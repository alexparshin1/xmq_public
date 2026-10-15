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
ENVIRONMENT=${ENVIRONMENT:-bench}     # bench (thinker10 -> thinker11) or aws (the client instance -> the server)
case $ENVIRONMENT in
    bench) HOST=${HOST:-thinker11};     ADDR=${ADDR:-10.1.1.243};   SUBNET=${SUBNET:-10.1.1};    NIC=${NIC:-enp86s0}
           HIVEMQ_UNIT=hivemq-bench;    MOSQUITTO_PLAIN=1884 ;;
    aws)   HOST=${HOST:-172.31.13.230}; ADDR=${ADDR:-172.31.13.230}; SUBNET=${SUBNET:-172.31.14}; NIC=${NIC:-ens5}
           HIVEMQ_UNIT=hivemq;          MOSQUITTO_PLAIN=1883 ;;
    *) echo "ENVIRONMENT is bench or aws" >&2; exit 2 ;;
esac
LIMIT_US=${LIMIT_US:-100000}
CONNECT_RATE=${CONNECT_RATE:-}         # set: passed to every scenario as --connection-rate; the scenario files say 10000
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
        hivemq)    echo "${VERSION_hivemq:-2026.5}" ;;
    esac
}

# The name the records and the site give the broker, and its settings line: what the site shows under
# "Settings". On AWS set SETTINGS_<broker> from the configuration there.
display() {
    case $1 in
        xmq) echo "XMQ" ;; flashmq) echo "FlashMQ" ;; mosquitto) echo "Mosquitto" ;; emqx) echo "EMQX" ;; hivemq) echo "HiveMQ CE" ;;
    esac
}
settings() {
    local v; v=$(version "$1")
    case $1 in
        xmq)       echo "${SETTINGS_xmq:-XMQ $v over TLS on port 8884, send_threads=3, receive_threads=4, delivery_threads=2, no persistence}" ;;
        flashmq)   echo "${SETTINGS_flashmq:-FlashMQ $v over TLS on port 8885, thread_count at its default (one per hardware thread)}" ;;
        mosquitto) echo "${SETTINGS_mosquitto:-Mosquitto $v over TLS on port 8883, persistence off, max_inflight_messages 65534, max_queued_messages unlimited}" ;;
        emqx)      echo "${SETTINGS_emqx:-EMQX $v (emqx-enterprise, community license) over TLS on port 8882, max_connections 2M, acceptors 64, backlog 8192}" ;;
        hivemq)    echo "${SETTINGS_hivemq:-HiveMQ CE $v over TLS on port 8886, JVM -Xms2g -Xmx20g -XX:MaxDirectMemorySize=5g, max-connections 2M, in-memory persistence}" ;;
    esac
}
host_note() { echo "${HOST_NOTE:-$HOST, i7-8705G, 4 cores/8 threads, 32Gb RAM, Ubuntu 26.04, no CPU affinity}"; }

# unit, process to sample, plain port (the readiness probe), TLS port
spec() {
    case $1 in
        xmq)       echo "xmq_server   xmq_server  1884 8884" ;;
        flashmq)   echo "flashmq      flashmq     1885 8885" ;;
        mosquitto) echo "mosquitto    mosquitto   $MOSQUITTO_PLAIN 8883" ;;
        emqx)      echo "emqx         beam.smp    1882 8882" ;;
        hivemq)    echo "$HIVEMQ_UNIT java        1886 8886" ;;
    esac
}

median_us() {  # the scenario's median, in microseconds, from its block file
    awk '/^Median/ {gsub("us","",$2); print $2; exit}' "$1" 2>/dev/null
}

tls_carries_a_message() {   # TLS port: a subscriber and a publisher over it, as the scenarios connect
    local port=$1 out i
    for i in $(seq 1 10); do
        out=$( (xmq_sub -h "$ADDR" -p "$port" --encrypted -V 5 -q 1 -u user -P secret -i tls-check-sub -t tlscheck/x -C 1 -W 6 2>&1 &
                sleep 2
                xmq_pub -h "$ADDR" -p "$port" --encrypted -V 5 -q 1 -u user -P secret -i tls-check-pub -t tlscheck/x -m ping > /dev/null 2>&1
                wait) )
        echo "$out" | grep -aq "Received 1 message" && return 0
        sleep 3
    done
    return 1
}

for broker in $BROKERS; do
    read -r unit proc plain tls <<< "$(spec "$broker")"
    previous=0
    ssh "$HOST" "sudo -n systemctl stop xmq_server flashmq mosquitto emqx hivemq hivemq-bench 2>/dev/null; sleep 2; sudo -n systemctl start $unit" > /dev/null 2>&1
    if ! tls_carries_a_message "$tls"; then
        say "$broker: no message over TLS on port $tls - the broker is not measured; is its TLS listener set up (brokers_tls_setup.sh)?"
        for rate in 50 75 100; do
            { echo "Server:   $(display "$broker")"; echo "Version:  $(version "$broker")"
              echo "Host:     $(host_note); $(settings "$broker")"; echo "Client:   ${CLIENT_NOTE:-$(hostname), xmq_scn $(xmq_scn --version 2>/dev/null | head -1 | awk '{print $NF}')}"
              echo "Date:     $(date +%F)"; echo; echo "Summary - failed"; echo
              echo "Scenario: Point-To-Point-TLS-${rate}K"
              echo "FAILED - the broker's TLS listener on port $tls did not carry a message"
            } > "$OUT/record-$broker-${rate}K.txt"
        done
        continue
    fi
    for rate in 50 75 100; do
        scenario=Point-To-Point-TLS-${rate}K.json
        if [ "$rate" != 50 ] && { [ -z "$previous" ] || [ "$previous" -ge "$LIMIT_US" ]; }; then
            say "$broker ${rate}K: not run - the median at the rate before was ${previous:-unknown} us, the limit is $LIMIT_US"
            # A record all the same, so that the site says why the broker is not in this rate.
            { echo "Server:   $(display "$broker")"; echo "Version:  $(version "$broker")"
              echo "Host:     $(host_note); $(settings "$broker")"; echo "Client:   ${CLIENT_NOTE:-$(hostname), xmq_scn $(xmq_scn --version 2>/dev/null | head -1 | awk '{print $NF}')}"
              echo "Date:     $(date +%F)"; echo; echo "Summary - not run"; echo
              echo "Scenario: Point-To-Point-TLS-${rate}K"
              echo "NOT RUN - the median at the rate before was $(awk -v u="${previous:-0}" 'BEGIN { if (u >= 1000000) printf "%.1f s", u / 1e6; else printf "%d ms", u / 1000 }'), over the limit of $((LIMIT_US / 1000)) ms"
            } > "$OUT/record-$broker-${rate}K.txt"
            continue
        fi
        set_file=$LT/.tls-p2p-set.txt            # names resolve beside the set file
        echo "$scenario" > "$set_file"
        cp "$LT/$scenario" "$OUT/$scenario"
        say "$broker ${rate}K: started"
        XMQ_SCN_EXTRA="--encrypted" ./run_scenario_set.sh \
            --restart-cmd "./restart_broker.sh --host $HOST --unit $unit --port $plain -u user -P secret" \
            --wait-for "$ADDR:$plain" --stats-host "$HOST" --stats-process "$proc" --nic "$NIC" \
            --out "$OUT/$broker-${rate}K" --record "$OUT/record-$broker-${rate}K.txt" --broker "$(display "$broker")" --version "$(version "$broker")" \
            --server-note "$(host_note); $(settings "$broker")" \
            "$set_file" -- \
            --subnet "$SUBNET" --first 150 --count 29 -h "$ADDR" -p "$tls" -u user -P secret ${CONNECT_RATE:+--connection-rate "$CONNECT_RATE"} \
            > "$OUT/$broker-${rate}K.console" 2>&1
        rc=$?
        previous=$(median_us "$OUT/$broker-${rate}K/${scenario%.json}.block")
        say "$broker ${rate}K: exit $rc, median ${previous:-none} us"
    done
done
ssh "$HOST" "sudo -n systemctl stop xmq_server flashmq mosquitto emqx hivemq hivemq-bench 2>/dev/null; sudo -n systemctl start xmq_server"
say "finished"
rm -f "$LT/.tls-p2p-set.txt"
touch "$OUT/DONE"
