#!/usr/bin/env bash
# The XMQ set again, after the 2026-09-28 campaign (FlashMQ, EMQX) finishes on this client.
#
# The first XMQ set that day was voided twice over: unattended upgrades restarted the broker in the
# middle of three scenarios (needrestart, after libc6/libssl3), and the server still carried a
# leftover delivery_threads of 16. Both are fixed on the server now - the apt timers are disabled and
# delivery_threads is 2 - and this checks them again before measuring anything.
set -u

B=172.31.13.230
LT=$HOME/workspace/xmq_public/LoadTest
OUT=$HOME/aws-2026-09-28
NAME=XMQ-0.9.18-rerun
cd "$LT" || exit 1

say() { echo "$(date +%H:%M:%S) $*" | tee -a "$OUT/chain.log"; }
bssh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$B" "$@"; }

while pgrep -f "aws-2026-09-28/[c]ampaign.sh" > /dev/null; do sleep 30; done
say "XMQ rerun: the campaign is over, checking the server"

limits=$(bssh 'sudo -n jq -c .server_limits /etc/xmq/xmq_server.conf')
timers=$(bssh 'systemctl is-enabled apt-daily.timer apt-daily-upgrade.timer 2>&1 | tr "\n" " "')
say "XMQ rerun: server_limits $limits; apt timers: $timers"
if ! echo "$limits" | grep -q '"delivery_threads":"2"' || echo "$timers" | grep -qw enabled; then
    say "XMQ rerun: NOT STARTED - the server is not in the state this rerun needs"
    say "XMQ rerun finished"
    exit 1
fi

restart_cmd="ssh -o BatchMode=yes -o ConnectTimeout=10 $B 'sudo -n systemctl stop xmq_server flashmq mosquitto emqx nanomq 2>/dev/null; sleep 2; sudo -n systemctl start xmq_server; sleep 2; systemctl is-active --quiet xmq_server'"
host_note="AWS c5n.4xlarge, 16 vCPU (8 physical cores x 2 threads), 40Gb RAM, Ubuntu 26.04, kernel $(bssh uname -r), CONFIG_HZ=1000, no IOMMU, no CPU affinity, apt timers disabled"
client_note="AWS c5n.4xlarge, 29 bound source addresses, xmq_scn $(xmq_scn --version 2>/dev/null | head -1), no CPU affinity"

say "$NAME: set started"
./run_scenario_set.sh \
    --restart-cmd "$restart_cmd" \
    --wait-for "$B:1884" \
    --stats-host "$B" --stats-process xmq_server \
    --nic ens5 \
    --out "$OUT/$NAME" \
    --record "$OUT/record-$NAME.txt" \
    --version 0.9.18 \
    --server-note "$host_note; XMQ on port 1884 as systemd service, send_threads=3, receive_threads=4, delivery_threads=2; persistence on (local Redis, clean_start) only for Point-To-Point-30K-persistent" \
    --client-note "$client_note" \
    --note "Rerun of the XMQ set of the same day: the first one was restarted mid-scenario by unattended upgrades and ran with delivery_threads=16." \
    --setup-cmd "./set_persistence.sh --host $B --port 1884 --for \"\$XMQ_SCENARIO\"" \
    Scenario-Set-AWS.txt -- -h "$B" -p 1884 --skip-ip-setup -R \
    > "$OUT/$NAME.console" 2>&1
say "$NAME: set finished, exit $?"
bssh "sudo -n systemctl stop xmq_server" && say "$NAME: xmq_server stopped"
say "XMQ rerun finished"
