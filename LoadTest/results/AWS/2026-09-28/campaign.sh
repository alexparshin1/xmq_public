#!/usr/bin/env bash
# AWS campaign 2026-09-28: Scenario-Set-AWS.txt against XMQ 0.9.18, FlashMQ 1.27.1 and EMQX 6.3.1,
# one broker at a time. Runs on the client instance, detached; progress goes to chain.log.
#
# Before every scenario all brokers on the server are stopped and only the one under test is started
# (as its systemd service - XMQ without CPU affinity). After each broker's set that broker is stopped.
set -u

B=172.31.13.230
LT=$HOME/workspace/xmq_public/LoadTest
OUT=$HOME/aws-2026-09-28
mkdir -p "$OUT"
cd "$LT" || exit 1

say() { echo "$(date +%H:%M:%S) $*" | tee -a "$OUT/chain.log"; }
bssh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$B" "$@"; }

restart_cmd() {
    echo "ssh -o BatchMode=yes -o ConnectTimeout=10 $B 'sudo -n systemctl stop xmq_server flashmq mosquitto emqx nanomq 2>/dev/null; sleep 2; sudo -n systemctl start $1; sleep 2; systemctl is-active --quiet $1'"
}

host_note="AWS c5n.4xlarge, 16 vCPU (8 physical cores x 2 threads), 40Gb RAM, Ubuntu 26.04, kernel $(bssh uname -r), CONFIG_HZ=1000, no IOMMU, no CPU affinity"
client_note="AWS c5n.4xlarge, 29 bound source addresses, xmq_scn $(xmq_scn --version 2>/dev/null | head -1), no CPU affinity"

run_set() {   # name unit process port version note [setup-cmd]
    local name=$1 unit=$2 proc=$3 port=$4 version=$5 note=$6 setup=${7:-}
    local args=(
        --restart-cmd "$(restart_cmd "$unit")"
        --wait-for "$B:$port"
        --stats-host "$B" --stats-process "$proc"
        --nic ens5
        --out "$OUT/$name"
        --record "$OUT/record-$name.txt"
        --version "$version"
        --server-note "$host_note; $note"
        --client-note "$client_note"
    )
    [ -n "$setup" ] && args+=(--setup-cmd "$setup")
    say "$name: set started"
    ./run_scenario_set.sh "${args[@]}" Scenario-Set-AWS.txt -- -h "$B" -p "$port" --skip-ip-setup -R \
        > "$OUT/$name.console" 2>&1
    say "$name: set finished, exit $?"
    bssh "sudo -n systemctl stop $unit" && say "$name: $unit stopped"
}

say "campaign started"
say "server: $(bssh 'xmq_server --version 2>&1 | head -1; dpkg-query -W -f="\${Package} \${Version}\n" flashmq emqx-enterprise' | tr '\n' ' ')"

run_set XMQ-0.9.18 xmq_server xmq_server 1884 0.9.18 \
    "XMQ on port 1884 as systemd service, send_threads=3, receive_threads=4, delivery_threads=16; persistence on (local Redis, clean_start) only for Point-To-Point-30K-persistent" \
    "./set_persistence.sh --host $B --port 1884 --for \"\$XMQ_SCENARIO\""

run_set FlashMQ-1.27.1 flashmq flashmq 1885 1.27.1 \
    "FlashMQ on port 1885, thread_count 16, rlimit_nofile 1200000, fs.nr_open=4194304; no broker-side persistence toggled"

run_set EMQX-6.3.1 emqx beam.smp 1882 6.3.1 \
    "EMQX 6.3.1 (emqx-enterprise, community license) on port 1882, LoadTest/emqx/emqx.conf (acceptors 64, max_connections 2M); no broker-side persistence toggled"

say "campaign finished"
