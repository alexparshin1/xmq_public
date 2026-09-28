#!/usr/bin/env bash
# Restart EMQX on the broker and wait until it can actually carry a message. Called by
# run_scenario_set --restart-cmd before every EMQX scenario. Exits 0 only when a publish reaches a
# subscriber, which is the readiness the open TCP port does not prove: EMQX 6.x accepts the port
# before its durable-storage shards are ready and refuses the connection behind it.
set -u
B=172.31.13.230
PORT=1882

ssh -o BatchMode=yes -o ConnectTimeout=10 "$B" \
    "sudo -n systemctl stop xmq_server flashmq mosquitto emqx nanomq 2>/dev/null; sleep 2; sudo -n systemctl start emqx" || exit 1

for _ in $(seq 1 60); do
    if timeout 10 bash -c '
            xmq_sub -h '"$B"' -p '"$PORT"' -t xmq/ready -C 1 -W 6 --quiet & sp=$!
            sleep 1
            xmq_pub -h '"$B"' -p '"$PORT"' -t xmq/ready -m 1 -q 1 --quiet >/dev/null 2>&1
            wait "$sp"' >/dev/null 2>&1; then
        exit 0
    fi
done
echo "EMQX did not carry a pub/sub round-trip on $B:$PORT after ~120s" >&2
exit 1
