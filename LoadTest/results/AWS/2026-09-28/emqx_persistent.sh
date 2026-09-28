#!/usr/bin/env bash
# One honest EMQX persistence measurement: Point-To-Point-30K-persistent with EMQX durable sessions
# ON, so the broker actually persists (recoverable after a crash) - the criterion for the site's
# persistence chart. The non-persistent EMQX rerun before this ran with durable OFF, so its
# persistent number was in-memory and not valid for the chart; this replaces it.
set -u

B=172.31.13.230
LT=$HOME/workspace/xmq_public/LoadTest
OUT=$HOME/aws-2026-09-28
NAME=EMQX-6.3.1-persistent
cd "$LT" || exit 1

say() { echo "$(date +%H:%M:%S) $*" | tee -a "$OUT/chain.log"; }
bssh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$B" "$@"; }

# The non-persistent EMQX rerun owns the broker until it is done.
while pgrep -f "aws-2026-09-28/[e]mqx_rerun.sh" > /dev/null; do sleep 30; done
say "$NAME: EMQX rerun over, turning durable sessions back on"

bssh "sudo -n systemctl stop emqx 2>/dev/null; \
      sudo -n sed -i '/durable_sessions[[:space:]]*{/,/}/ s/enable[[:space:]]*=[[:space:]]*false/enable = true/' /etc/emqx/emqx.conf; \
      sudo -n awk '/durable_sessions/{f=1} f&&/enable/{print \$0; f=0}' /etc/emqx/emqx.conf" | sed 's/^/    durable_sessions: /'

# A set of exactly one scenario.
set_file="$OUT/set-persistent.txt"
echo "Point-To-Point-30K-persistent.json" > "$set_file"

say "$NAME: run started"
./run_scenario_set.sh \
    --restart-cmd "bash $OUT/emqx_restart.sh" \
    --wait-for "$B:1882" \
    --stats-host "$B" --stats-process beam.smp \
    --nic ens5 \
    --out "$OUT/$NAME" \
    --record "$OUT/record-$NAME.txt" \
    --version 6.3.1 \
    --server-note "AWS c5n.4xlarge, 16 vCPU, 40Gb RAM, Ubuntu 26.04; EMQX 6.3.1 with durable_sessions ENABLED (real persistence, crash-recoverable), LimitNOFILE 2000000, port 1882; scenario gated on a pub/sub round-trip" \
    --client-note "AWS c5n.4xlarge, 29 bound source addresses, xmq_scn $(xmq_scn --version 2>/dev/null | head -1)" \
    --note "EMQX persistence number for the site chart: durable sessions on, so the broker persists and can recover after a crash. Compared against XMQ 0.9.17 (Redis). FlashMQ is excluded - it only snapshots periodically." \
    "$set_file" -- -h "$B" -p 1882 --skip-ip-setup -R \
    > "$OUT/$NAME.console" 2>&1
say "$NAME: run finished, exit $?"

# Leave the config as the campaign found it this morning: durable on.
bssh "sudo -n systemctl stop emqx" && say "$NAME: emqx stopped"
say "EMQX persistent finished"
