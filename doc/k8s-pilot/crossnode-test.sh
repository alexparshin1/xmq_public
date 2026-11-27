#!/usr/bin/env bash
# Verify that a message published to xmq-2 reaches a subscriber on xmq-0 through a cluster link.
# Usage: XMQ_NAMESPACE=xmq bash crossnode-test.sh
set -uo pipefail
NS=${XMQ_NAMESPACE:-xmq}

kubectl -n "$NS" delete pod xmq-sub-check --ignore-not-found >/dev/null 2>&1
kubectl -n "$NS" run xmq-sub-check --image=alexeyparshin/xmq:1.2.1 --restart=Never \
  --command -- xmq_sub -h "xmq-0.xmq.$NS.svc" -p 1883 -t model/test \
  -C 1 -W 60 -v -i xmq-check-sub >/dev/null 2>&1
sleep 4

echo "=== publish on xmq-2 ==="
kubectl -n "$NS" exec xmq-2 -c xmq -- xmq_pub -h 127.0.0.1 -p 1883 \
  -t model/test -m hello-from-pod2 -q 1 -i xmq-check-pub

sleep 10
echo "=== subscriber on xmq-0 ==="
kubectl -n "$NS" logs xmq-sub-check
kubectl -n "$NS" delete pod xmq-sub-check --ignore-not-found >/dev/null 2>&1
