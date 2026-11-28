# XMQ 1.2.1 operator — creating an XMQCluster in kind

This guide builds the operator image once, installs it into a local kind cluster, and creates an
`XMQCluster`. Redis and PostgreSQL are external to the operator: this guide deploys them as simple
single-instance workloads inside the same kind cluster, but the operator only connects to their
URIs. No Go is needed on the Kubernetes nodes or on the broker hosts — Go is used only to build the
operator image.

The operator and the broker image are both version-pinned. This guide supports **XMQ 1.2.1**.

## Values

```bash
cat > ~/xmq-operator.env <<'EOF'
export XMQ_VERSION=1.2.1
export XMQ_SPTK_VERSION=5.6.23
export XMQ_NAMESPACE=xmq
export XMQ_OPERATOR_NS=xmq-operator-system
export XMQ_DB_USER=xmq
export XMQ_DB_PASSWORD='change-this-database-password'
export XMQ_DB_NAME=xmq
export XMQ_ADMIN_PASSWORD='change-this-admin-password'
export XMQ_CLUSTER_PASSWORD='change-this-cluster-password'
EOF
chmod 600 ~/xmq-operator.env
set -a; . ~/xmq-operator.env; set +a
```

**Check.**

```bash
echo "$XMQ_VERSION $XMQ_NAMESPACE $XMQ_OPERATOR_NS"
```

## 1. Tools on the build machine

Ubuntu VM with 4 vCPU, 8 GB RAM, 60 GB free disk. Install Docker, kubectl, kind, openssl, jq and
Go 1.27 (Go is needed only here, for the operator image):

```bash
sudo apt update
sudo apt install -y docker.io openssl jq curl
sudo usermod -aG docker "$USER"          # log out and back in
mkdir -p ~/.local/bin
curl -fsSL -o ~/.local/bin/kubectl \
  "https://dl.k8s.io/release/$(curl -fsSL https://dl.k8s.io/release/stable.txt)/bin/linux/amd64/kubectl"
curl -fsSL -o ~/.local/bin/kind https://kind.sigs.k8s.io/dl/v0.33.0/kind-linux-amd64
curl -fsSL -o /tmp/go.tgz "https://go.dev/dl/$(curl -fsSL 'https://go.dev/VERSION?m=text' | head -1).linux-amd64.tar.gz"
mkdir -p ~/.local/go-root && tar -C ~/.local/go-root -xzf /tmp/go.tgz
export PATH="$HOME/.local/bin:$HOME/.local/go-root/go/bin:$PATH"
```

**Check.**

```bash
docker info | grep 'Server Version'
kubectl version --client
kind version
go version
```

## 2. Build both images

```bash
cd ~/workspace/xmq_public
docker build --no-cache --pull \
  --build-arg XMQ_VERSION="$XMQ_VERSION" \
  --build-arg SPTK_VERSION="$XMQ_SPTK_VERSION" \
  -t "alexeyparshin/xmq:$XMQ_VERSION" docker

cd operator
make build                              # go build ./...
make image IMG="xmq-operator:$XMQ_VERSION"
```

**Check.**

```bash
docker run --rm --entrypoint xmq_server "alexeyparshin/xmq:$XMQ_VERSION" --version
docker image inspect "xmq-operator:$XMQ_VERSION" >/dev/null && echo operator-image-ok
```

## 3. Create the kind cluster

`doc/k8s-pilot/kind-config.yaml` describes one control-plane and three workers, and maps host ports
`11883 → MQTT`, `18884 → MQTT+SSL`, `19184 → metrics`, `18885 → web interface`.

```bash
kind create cluster --config doc/k8s-pilot/kind-config.yaml --wait 180s
```

**Check.** All four nodes are `Ready`:

```bash
kubectl get nodes -o wide
```

## 4. Load images into the nodes

```bash
docker pull -q redis:8-alpine
docker pull -q postgres:16-alpine
docker pull -q alpine:3.20
for image in "alexeyparshin/xmq:$XMQ_VERSION" "xmq-operator:$XMQ_VERSION" \
             redis:8-alpine postgres:16-alpine alpine:3.20; do
  kind load docker-image "$image" --name xmq-model
done
```

**Check.** The two XMQ images must be present on every worker:

```bash
for n in xmq-model-worker xmq-model-worker2 xmq-model-worker3; do
  docker exec "$n" ctr -n k8s.io images ls | grep -E 'alexeyparshin/xmq|xmq-operator'
done
```

## 5. Deploy the external dependencies

They are ordinary Kubernetes workloads, but **not managed by the operator**.

```bash
kubectl create namespace "$XMQ_NAMESPACE"
kubectl -n "$XMQ_NAMESPACE" create deployment redis --image=redis:8-alpine -- \
  redis-server --appendonly yes
kubectl -n "$XMQ_NAMESPACE" expose deployment redis --port=6379

kubectl -n "$XMQ_NAMESPACE" create deployment postgres --image=postgres:16-alpine
kubectl -n "$XMQ_NAMESPACE" set env deployment/postgres \
  POSTGRES_USER="$XMQ_DB_USER" POSTGRES_PASSWORD="$XMQ_DB_PASSWORD" POSTGRES_DB="$XMQ_DB_NAME"
kubectl -n "$XMQ_NAMESPACE" expose deployment postgres --port=5432
```

**Check.**

```bash
kubectl -n "$XMQ_NAMESPACE" get pods
kubectl -n "$XMQ_NAMESPACE" exec deploy/redis -- redis-cli ping
kubectl -n "$XMQ_NAMESPACE" exec deploy/postgres -- psql -U "$XMQ_DB_USER" -d "$XMQ_DB_NAME" -c 'select 1;'
```

## 6. Create the CA, broker certificates and credentials

```bash
bash doc/k8s-pilot/gen-certs.sh
kubectl -n "$XMQ_NAMESPACE" create secret generic xmq-credentials \
  --from-literal=admin="$XMQ_ADMIN_PASSWORD" \
  --from-literal=cluster="$XMQ_CLUSTER_PASSWORD"
```

`gen-certs.sh` creates `xmq-ca` (key `ca.crt`) and `xmq-tls-0`, `xmq-tls-1`, `xmq-tls-2`
(keys `node.crt`, `node.key`). The operator expects exactly these names by default.

**Check.**

```bash
kubectl -n "$XMQ_NAMESPACE" get secrets
openssl x509 -in ~/k8s-certs/ca.crt -noout -subject -fingerprint -sha256
```

## 7. Install the operator

```bash
cd ~/workspace/xmq_public/operator
make install                             # applies the CRD
kubectl apply -f config/rbac/rbac.yaml
kubectl apply -f config/manager/manager.yaml
```

**Check.** The manager pod must be `Running` without restarts:

```bash
kubectl -n "$XMQ_OPERATOR_NS" get pods
kubectl -n "$XMQ_OPERATOR_NS" logs deploy/xmq-operator-controller-manager --tail=20
```

## 8. Create the XMQCluster

Create the resource with the URIs of the dependencies. The DNS names assume namespace `xmq`;
adjust them if you chose another namespace.

```bash
cat > /tmp/xmqcluster.yaml <<EOF
apiVersion: xmq.sptk.net/v1
kind: XMQCluster
metadata:
  name: xmq
  namespace: $XMQ_NAMESPACE
spec:
  replicas: 3
  version: $XMQ_VERSION
  redisURI: redis://redis.$XMQ_NAMESPACE.svc.cluster.local:6379/0
  databaseURI: postgresql://$XMQ_DB_USER:$XMQ_DB_PASSWORD@postgres.$XMQ_NAMESPACE.svc.cluster.local:5432/$XMQ_DB_NAME
  caSecret: xmq-ca
  tlsSecretPrefix: xmq-tls
  credentialsSecret: xmq-credentials
EOF
kubectl apply -f /tmp/xmqcluster.yaml
```

**Check after about 60 seconds.** All conditions must be `True`:

```bash
kubectl -n "$XMQ_NAMESPACE" get xmqcluster xmq -o jsonpath={.status}
kubectl -n "$XMQ_NAMESPACE" get pods -o wide
kubectl -n "$XMQ_NAMESPACE" get job xmq-setup
```

Expected: `readyReplicas: 3`, `StatefulSetReady=True`, `SetupComplete=True`,
`ClusterCredentialReady=True`, `Ready=True`; setup Job `Complete 1/1`.

## 9. Verify the cluster works

Probes and metrics:

```bash
curl -s -o /dev/null -w 'healthz=%{http_code}\n' http://127.0.0.1:19184/healthz
curl -s -o /dev/null -w 'readyz=%{http_code}\n'  http://127.0.0.1:19184/readyz
curl -s http://127.0.0.1:19184/metrics | head -5
```

Both probes must answer `200`.

Inter-node delivery (subscriber on `xmq-0`, publisher on `xmq-2`):

```bash
bash doc/k8s-pilot/crossnode-test.sh
```

**Check.** The subscriber output contains the published payload, for example
`[hello-from-pod2]` and `Received 1 message(s).`

Identity survives a pod restart:

```bash
id_before=$(kubectl -n "$XMQ_NAMESPACE" exec xmq-0 -c xmq -- cat /etc/xmq/xmq_node.id)
kubectl -n "$XMQ_NAMESPACE" delete pod xmq-0 --wait=false
sleep 70
id_after=$(kubectl -n "$XMQ_NAMESPACE" exec xmq-0 -c xmq -- cat /etc/xmq/xmq_node.id)
echo "$id_before"; echo "$id_after"
```

**Check.** The two GUIDs are identical and `kubectl -n "$XMQ_NAMESPACE" logs xmq-0 -c xmq | grep Rejoined`
shows the rejoin.

## Scope

This operator automates CRD, workloads, static certificates and initial setup for XMQ 1.2.1.
It does not manage Redis/PostgreSQL HA, integrate with cert-manager, rotate certificates
automatically, or perform mixed-version upgrades. The manual guide
`doc/KubernetesPilot-1.2.1.md` remains the description of what happens underneath.
