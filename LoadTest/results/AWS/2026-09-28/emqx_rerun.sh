#!/usr/bin/env bash
# EMQX 6.3.1 rerun of the 2026-09-28 AWS set, after the XMQ rerun on this client finishes.
#
# The first EMQX run failed every scenario: 6.x durable sessions were on, so after each restart the
# durable-storage shards had to elect a replica (shard_replica_not_ready) and the broker refused
# connections for tens of seconds while the TCP port was already open. This turns durable sessions
# off - we measure the non-persistent scenarios, as the other brokers do here - raises the broker's
# file-descriptor limit for the 1M test, and gates every scenario on a real publish/subscribe
# round-trip rather than on the port opening.
set -u

B=172.31.13.230
LT=$HOME/workspace/xmq_public/LoadTest
OUT=$HOME/aws-2026-09-28
NAME=EMQX-6.3.1-rerun
cd "$LT" || exit 1

say() { echo "$(date +%H:%M:%S) $*" | tee -a "$OUT/chain.log"; }
bssh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$B" "$@"; }

# Wait out the XMQ rerun: one broker at a time, and it owns the server until it is done.
while pgrep -f "aws-2026-09-28/[x]mq_rerun.sh" > /dev/null; do sleep 30; done
say "$NAME: XMQ rerun is over, preparing EMQX"

# Durable sessions off, so there is no shard election on boot. Stopped first - EMQX rewrites nothing,
# but a reconfigure while running is needless. Read back to be sure the value took.
bssh "sudo -n systemctl stop emqx 2>/dev/null; \
      sudo -n sed -i '/durable_sessions[[:space:]]*{/,/}/ s/enable[[:space:]]*=[[:space:]]*true/enable = false/' /etc/emqx/emqx.conf; \
      sudo -n awk '/durable_sessions/{f=1} f&&/enable/{print \$0; f=0}' /etc/emqx/emqx.conf" | sed 's/^/    durable_sessions: /'

# The 1M test needs more descriptors than the service's default 1048576 (the first run logged
# max_connections_clamped_by_system_limit). A drop-in override, applied with daemon-reload.
bssh "sudo -n mkdir -p /etc/systemd/system/emqx.service.d && \
      printf '[Service]\nLimitNOFILE=2000000\n' | sudo -n tee /etc/systemd/system/emqx.service.d/nofile.conf >/dev/null && \
      sudo -n systemctl daemon-reload"

host_note="AWS c5n.4xlarge, 16 vCPU (8 physical cores x 2 threads), 40Gb RAM, Ubuntu 26.04, kernel $(bssh uname -r), CONFIG_HZ=1000, no IOMMU, apt timers disabled"
client_note="AWS c5n.4xlarge, 29 bound source addresses, xmq_scn $(xmq_scn --version 2>/dev/null | head -1)"

# Stop every broker, start EMQX, then wait until a message published to the broker comes back to a
# subscriber - the readiness the port alone does not prove. The logic is its own script, to keep it
# clear of the quoting a --restart-cmd string would need.
say "$NAME: set started"
./run_scenario_set.sh \
    --restart-cmd "bash $OUT/emqx_restart.sh" \
    --wait-for "$B:1882" \
    --stats-host "$B" --stats-process beam.smp \
    --nic ens5 \
    --out "$OUT/$NAME" \
    --record "$OUT/record-$NAME.txt" \
    --version 6.3.1 \
    --server-note "$host_note; EMQX 6.3.1 (emqx-enterprise, community license) on port 1882, LoadTest/emqx/emqx.conf, durable_sessions disabled, LimitNOFILE 2000000; each scenario gated on a pub/sub round-trip" \
    --client-note "$client_note" \
    --note "Rerun: the first EMQX run failed every scenario because 6.x durable sessions were on and the shards were not ready within the restart window. Durable sessions off here, so the persistent scenario is in-memory for EMQX." \
    Scenario-Set-AWS.txt -- -h "$B" -p 1882 --skip-ip-setup -R \
    > "$OUT/$NAME.console" 2>&1
say "$NAME: set finished, exit $?"
bssh "sudo -n systemctl stop emqx" && say "$NAME: emqx stopped"
say "EMQX rerun finished"
