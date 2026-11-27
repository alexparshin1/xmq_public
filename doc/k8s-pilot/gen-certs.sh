#!/usr/bin/env bash
set -euo pipefail
D=$HOME/k8s-certs
rm -rf "$D"; mkdir -p "$D"
cd "$D"

openssl req -x509 -newkey rsa:2048 -nodes -days 365 \
  -keyout ca.key -out ca.crt -subj "/CN=xmq-model-ca" >/dev/null 2>&1

for i in 0 1 2; do
  name="xmq-$i"
  openssl req -newkey rsa:2048 -nodes -keyout "$name.key" -out "$name.csr" \
    -subj "/CN=$name.xmq.default.svc.cluster.local" >/dev/null 2>&1
  cat > "$name.ext" <<EOF
subjectAltName=DNS:$name.xmq.xmq.svc,DNS:$name.xmq.xmq.svc.cluster.local,DNS:xmq.xmq.svc,DNS:xmq.xmq.svc.cluster.local,DNS:xmq,DNS:theater,DNS:localhost,IP:127.0.0.1
extendedKeyUsage=serverAuth,clientAuth
basicConstraints=CA:FALSE
EOF
  openssl x509 -req -in "$name.csr" -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out "$name.crt" -days 365 -extfile "$name.ext" >/dev/null 2>&1
done

NS=${XMQ_NAMESPACE:-xmq}
kubectl create namespace "$NS" 2>/dev/null || true
kubectl -n "$NS" create secret generic xmq-ca --from-file=ca.crt=ca.crt --dry-run=client -o yaml | kubectl apply -f -
for i in 0 1 2; do
  kubectl -n "$NS" create secret generic "xmq-tls-$i" \
    --from-file=node.crt="xmq-$i.crt" --from-file=node.key="xmq-$i.key" \
    --dry-run=client -o yaml | kubectl apply -f -
done
echo "CA fingerprint: $(openssl x509 -in ca.crt -noout -fingerprint -sha256)"
for i in 0 1 2; do echo "xmq-$i: $(openssl x509 -in xmq-$i.crt -noout -fingerprint -sha256)"; done
