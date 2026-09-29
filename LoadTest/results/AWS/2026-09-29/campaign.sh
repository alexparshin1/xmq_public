#!/usr/bin/env bash
# XMQ 0.9.19 persistence on AWS: Super-Durable (Redis appendfsync always) at 40K, then Durable
# (appendfsync everysec) at 60K - the two headline figures, nothing else, to keep AWS time short. Point-To-Point persistent, slow connect
# (400/s), 10 minutes each, one scenario per record file.
#
# Redis gets exactly what the site's Persistence page tells users to set - appendonly yes and the
# appendfsync of the mode - and nothing else, so the figures are the ones a user following that
# page gets. The broker: send 3 / receive 4 / delivery 2, max_queued_writes 1000,
# max_redis_connections 2 (the 0.9.19 defaults, written out because this host's configuration
# predates them).
#
# Left behind: Redis in Durable (appendonly yes, everysec) - the state the next all-broker
# campaign runs in - and xmq_server stopped.
#
# Runs on the AWS client as alexeyp, from ~/workspace/xmq_public/LoadTest.
set -u

B=172.31.13.230
LT=$HOME/workspace/xmq_public/LoadTest
OUT=$HOME/aws-2026-09-29
mkdir -p "$OUT"
cd "$LT" || exit 1

say()  { echo "$(date +%H:%M:%S) $*" | tee -a "$OUT/chain.log"; }
bssh() { ssh -o BatchMode=yes -o ConnectTimeout=10 "$B" "$@"; }

say "campaign start: XMQ $(bssh 'dpkg-query -W -f=\${Version} xmq-server'), $(bssh 'redis-server --version | cut -d" " -f3')"

# The broker's configuration, edited with the broker stopped: it rewrites its own file shortly
# after starting, and an edit under a running broker races that write. tee keeps the file's owner
# and mode.
bssh "sudo -n systemctl stop xmq_server; \
      sudo -n jq '.persistence.enabled = true | .persistence.clean_start = true
                  | .persistence.max_queued_writes = 1000 | .persistence.max_redis_connections = 2
                  | .server_limits.send_threads = \"3\" | .server_limits.receive_threads = \"4\"
                  | .server_limits.delivery_threads = \"2\"' /etc/xmq/xmq_server.conf > /tmp/xmq_server.conf.new \
      && sudo -n tee /etc/xmq/xmq_server.conf < /tmp/xmq_server.conf.new > /dev/null; \
      rm -f /tmp/xmq_server.conf.new; \
      sudo -n jq -c '.persistence, .server_limits' /etc/xmq/xmq_server.conf" | sed 's/^/    config: /' | tee -a "$OUT/chain.log"

redis_mode() {
    bssh "sudo -n sed -i -e 's/^appendonly .*/appendonly yes/' -e 's/^appendfsync .*/appendfsync $1/' /etc/redis/redis.conf; \
          sudo -n systemctl restart redis-server; sleep 5; \
          echo \"appendonly=\$(redis-cli CONFIG GET appendonly | tail -1) appendfsync=\$(redis-cli CONFIG GET appendfsync | tail -1)\""
}

run() {
    local mode=$1 fsync=$2 rate=$3
    local name="XMQ-0.9.19-$mode-${rate}K"
    local set_file="$LT/set-$name.txt"
    echo "Point-To-Point-${rate}K-persistent-slowconnect.json" > "$set_file"

    # Redis's own CPU, sampled every 10 s for the whole run; the steady part is picked out later.
    bssh "nohup top -b -d 10 -p \$(pidof redis-server) > /tmp/redis-top-$name.log 2>&1 < /dev/null &"

    say "$name: started"
    ./run_scenario_set.sh \
        --restart-cmd "ssh -o BatchMode=yes $B sudo -n systemctl restart xmq_server" \
        --wait-for "$B:1884" \
        --stats-host "$B" --stats-process xmq_server \
        --nic ens5 \
        --out "$OUT/$name" \
        --record "$OUT/record-$name.txt" \
        --version 0.9.19 \
        --server-note "AWS c5n.4xlarge, 16 vCPU, 40Gb RAM, Ubuntu 26.04; XMQ 0.9.19 send 3 / receive 4 / delivery 2, max_queued_writes 1000, max_redis_connections 2, no CPU affinity; Redis 8.0.5 on the same host, appendonly yes, appendfsync $fsync, otherwise default" \
        --client-note "AWS c5n.4xlarge, 29 bound source addresses, xmq_scn $(xmq_scn --version 2>/dev/null | head -1)" \
        --note "$mode: Redis appendfsync $fsync. Point-To-Point with persistent sessions, connections paced at 400/s." \
        "$set_file" -- -h "$B" -p 1884 --duration 600 \
        > "$OUT/$name.console" 2>&1
    local rc=$?

    bssh "pkill -f 'top -b -d 10 -p' ; true"
    scp -q -o BatchMode=yes "$B:/tmp/redis-top-$name.log" "$OUT/" && bssh "rm -f /tmp/redis-top-$name.log"
    rm -f "$set_file"
    say "$name: finished, exit $rc; $(grep -h -m1 '^Average' "$OUT/record-$name.txt" 2>/dev/null)"
    say "    server disk: $(bssh "df -h / | tail -1 | awk '{print \$4\" free\"}'"), AOF: $(bssh 'sudo -n du -sh /var/lib/redis/appendonlydir 2>/dev/null | cut -f1')"
}

say "Super-Durable: $(redis_mode always)"
run super-durable always 40

say "Durable: $(redis_mode everysec)"
run durable everysec 60

bssh "sudo -n systemctl stop xmq_server"
say "xmq_server stopped; Redis left in Durable: $(bssh 'redis-cli CONFIG GET appendfsync | tail -1')"
say "CAMPAIGN DONE"
