# XMQ 1.2.1 — Kubernetes cluster on a fresh Ubuntu VM

This guide builds a three-node XMQ 1.2.1 cluster on Kubernetes with one shared certificate
authority. It uses plain manifests and the scripts in `doc/k8s-pilot/`. It does not install a
Helm chart or operator: those are a separate, productised layer.

## Scope and limits

What this cluster gives you:

- a StatefulSet of three brokers with stable names, stable `xmq_node.id` and per-node storage;
- cluster links over MQTT+SSL, verified against one CA;
- PostgreSQL for configuration/accounts and Redis for runtime state;
- Prometheus metrics and `/healthz` / `/readyz` probes;
- a repeatable way to verify that a message crosses from one broker to another.

What it deliberately does not cover:

- HA of PostgreSQL and Redis — both run as single instances;
- automatic certificate rotation — replacing a certificate requires a pod rollout;
- multi-tenancy, admin RBAC, audit log, OpenTelemetry;
- multi-architecture images or mixed-version rolling upgrades.

These are differences from a commercial Kubernetes operator, not defects of the broker.

## 0. Deployment values

Put the deployment-specific values in a file with restricted permissions and source it before
running any command in this guide. Choose your own passwords; the examples are placeholders.

```bash
cat > ~/xmq-k8s.env <<'EOF'
export XMQ_VERSION=1.2.1
export XMQ_SPTK_VERSION=5.6.23
export XMQ_NAMESPACE=xmq
export XMQ_DB_USER=xmq
export XMQ_DB_PASSWORD='change-this-database-password'
export XMQ_DB_NAME=xmq
export XMQ_ADMIN_PASSWORD='change-this-admin-password'
export XMQ_CLUSTER_PASSWORD='change-this-cluster-password'
EOF
chmod 600 ~/xmq-k8s.env
set -a; . ~/xmq-k8s.env; set +a
```

**Check.** The values are visible in this shell only:

```bash
echo "$XMQ_VERSION $XMQ_SPTK_VERSION $XMQ_NAMESPACE"
```

## 1. Prepare the VM

Use an Ubuntu VM with 4 vCPU, 8 GB RAM and at least 60 GB of free disk. Docker needs room for
kind's node images and the container layers.

```bash
sudo apt update
sudo apt install -y docker.io openssl jq curl python3
sudo usermod -aG docker "$USER"        # log out and back in
mkdir -p ~/.local/bin
curl -fsSL -o ~/.local/bin/kubectl \
  "https://dl.k8s.io/release/$(curl -fsSL https://dl.k8s.io/release/stable.txt)/bin/linux/amd64/kubectl"
curl -fsSL -o ~/.local/bin/kind \
  https://kind.sigs.k8s.io/dl/v0.33.0/kind-linux-amd64
chmod +x ~/.local/bin/kubectl ~/.local/bin/kind
export PATH="$HOME/.local/bin:$PATH"
```

**Check.**

```bash
docker info | grep 'Server Version'
kubectl version --client
kind version
df -h / /var/lib/docker
```

## 2. Build the XMQ image

The image installs the published `.deb`, so its binaries are exactly the released ones.

```bash
cd ~/workspace/xmq_public
docker build --no-cache --pull \
  --build-arg XMQ_VERSION="$XMQ_VERSION" \
  --build-arg XMQ_SPTK_VERSION="$XMQ_SPTK_VERSION" \
  -t "alexeyparshin/xmq:$XMQ_VERSION" docker
```

**Check.**

```bash
docker run --rm --entrypoint xmq_server "alexeyparshin/xmq:$XMQ_VERSION" --version
# XMQ server 1.2.1
```

## 3. Create the Kubernetes cluster

`doc/k8s-pilot/kind-config.yaml` describes one control-plane node and three workers. It also maps
host ports: `11883 → MQTT`, `18884 → MQTT+SSL`, `19184 → metrics`, `18885 → web interface`.

```bash
kind create cluster --config doc/k8s-pilot/kind-config.yaml --wait 180s
```

**Check.** All four nodes must be `Ready`:

```bash
kubectl get nodes -o wide
```

## 4. Put the images on the nodes

```bash
docker pull -q redis:8-alpine
docker pull -q postgres:16-alpine
docker pull -q alpine:3.20
for image in "alexeyparshin/xmq:$XMQ_VERSION" redis:8-alpine postgres:16-alpine alpine:3.20; do
  kind load docker-image "$image" --name xmq-model
done
```

**Check.** Repeat on all three workers:

```bash
for n in xmq-model-worker xmq-model-worker2 xmq-model-worker3; do
  docker exec "$n" ctr -n k8s.io images ls | grep alexeyparshin/xmq
done
```

## 5. Create the CA and the broker certificates

One CA signs a certificate for each pod. The SANs include the pod DNS name, the service DNS name,
`localhost` and `127.0.0.1`; the last two allow a client on the host to verify TLS through a
port-forward. The script creates Secrets `xmq-ca` and `xmq-tls-0..2`.

```bash
bash doc/k8s-pilot/gen-certs.sh
```

**Check.**

```bash
kubectl -n "$XMQ_NAMESPACE" get secrets
openssl x509 -in ~/k8s-certs/ca.crt -noout -subject -fingerprint -sha256
openssl x509 -in ~/k8s-certs/xmq-0.crt -noout -text | grep -A1 'Subject Alternative Name'
```

## 6. Deploy the cluster

`doc/k8s-pilot/deploy.py` creates:

- `xmq-configs` — one broker configuration per pod. Each has `cluster.enabled=true`,
  `this_node.host_port = xmq-N.<namespace>.svc.cluster.local:8883`, `cafile` pointing at the
  mounted CA, the PostgreSQL and Redis URIs, and Prometheus enabled.
- `xmq-ext` — the shipped `user-database` authenticator, enabled and required.
- a headless service `xmq` and a NodePort service `xmq-nodeport`;
- Redis and PostgreSQL deployments;
- the XMQ StatefulSet with a PVC template for `/etc/xmq` and per-pod certificate mounts.

Start with one replica so the first broker creates the SQL schema, then scale to three:

```bash
python3 doc/k8s-pilot/deploy.py
kubectl -n "$XMQ_NAMESPACE" scale statefulset xmq --replicas=1
sleep 60
kubectl -n "$XMQ_NAMESPACE" scale statefulset xmq --replicas=3
```

**Check.** PVCs are `Bound` and all pods are `1/1 Running`:

```bash
kubectl -n "$XMQ_NAMESPACE" get pvc
kubectl -n "$XMQ_NAMESPACE" get pods -o wide
kubectl -n "$XMQ_NAMESPACE" get svc
```

## 7. Set the administrator and cluster passwords

Two places must agree for cluster links:

- the `cluster` account hash in `xmq_user`, used by the authenticator;
- the plain cluster secret in `xmq_cluster_credential`, used for outgoing cluster links.

```bash
printf '%s\n' "$XMQ_ADMIN_PASSWORD" | kubectl -n "$XMQ_NAMESPACE" exec -i xmq-0 -- \
  xmq_server -c /etc/xmq/xmq_server.conf --set-password admin
printf '%s\n' "$XMQ_CLUSTER_PASSWORD" | kubectl -n "$XMQ_NAMESPACE" exec -i xmq-0 -- \
  xmq_server -c /etc/xmq/xmq_server.conf --set-password cluster
kubectl -n "$XMQ_NAMESPACE" exec deploy/postgres -- psql -U "$XMQ_DB_USER" -d "$XMQ_DB_NAME" \
  -c "UPDATE xmq_cluster_credential SET secret='$XMQ_CLUSTER_PASSWORD' WHERE id=1;"
kubectl -n "$XMQ_NAMESPACE" delete pods -l app=xmq --wait=false
```

**Check.** After about 70 seconds the cluster links must be up:

```bash
kubectl -n "$XMQ_NAMESPACE" logs xmq-0 -c xmq --tail=200 | grep -E 'Connected node|Attached to cluster|Rejoined'
kubectl -n "$XMQ_NAMESPACE" exec deploy/postgres -- psql -U "$XMQ_DB_USER" -d "$XMQ_DB_NAME" \
  -c 'select username,is_enabled from xmq_user; select length(secret) from xmq_cluster_credential;'
```

You want `Connected node 'xmq-1'`, `Attached to cluster node [...]`, `Rejoined the cluster`, and
both accounts enabled with a non-empty credential.

## 8. Check probes, metrics and TLS

```bash
curl -s -o /dev/null -w 'healthz=%{http_code}\n' http://127.0.0.1:19184/healthz
curl -s -o /dev/null -w 'readyz=%{http_code}\n'  http://127.0.0.1:19184/readyz
curl -s http://127.0.0.1:19184/metrics | head -5
```

Both probes must answer `200`; the metrics must name version 1.2.1.

TLS with the shared CA, using the broker utilities (an explicit client id is required):

```bash
kubectl -n "$XMQ_NAMESPACE" port-forward pod/xmq-0 11882:8883 >/tmp/pft.log 2>&1 &
kubectl -n "$XMQ_NAMESPACE" exec xmq-0 -- xmq_pub \
  -h 127.0.0.1 -p 8883 --cafile /etc/xmq/certs/ca.crt \
  -i tls-pub -t model/tls -m hello-over-tls -q 1
```

**Check.** `xmq_pub` completes without an error. A client that does not trust the CA fails the
handshake — that is the negative test.

## 9. Check delivery between brokers

`doc/k8s-pilot/crossnode-test.sh` subscribes on `xmq-0`, publishes on `xmq-2`, and prints the
subscriber's output. The `-v` flag of `xmq_sub` prints the received payload.

```bash
bash doc/k8s-pilot/crossnode-test.sh
```

**Check.** The subscriber log contains the published payload, for example:

```text
Publish id=0 sender=xmq-check-pub qos=1, payload=15 bytes, topic='model/test' [hello-from-pod2]
Receive 1 messages ...
Received 1 message(s).
```

That proves the message crossed a cluster link instead of being delivered locally.

## 10. Check that identity survives a pod restart

```bash
id_before=$(kubectl -n "$XMQ_NAMESPACE" exec xmq-0 -c xmq -- cat /etc/xmq/xmq_node.id)
kubectl -n "$XMQ_NAMESPACE" delete pod xmq-0 --wait=false
sleep 70
id_after=$(kubectl -n "$XMQ_NAMESPACE" exec xmq-0 -c xmq -- cat /etc/xmq/xmq_node.id)
echo "$id_before"; echo "$id_after"
kubectl -n "$XMQ_NAMESPACE" logs xmq-0 -c xmq --tail=200 | grep Rejoined
```

**Check.** The two GUIDs are identical and `Rejoined the cluster` appears in the log. Without the
per-pod PVC the restarted broker would be refused as a different node.

## Result

After step 10 you have a three-node XMQ 1.2.1 cluster on Kubernetes with one shared CA, stable
node identities, working inter-node delivery, and known verification commands for each property.
The remaining differences from a commercial operator are the ones listed under **Scope and
limits**: automation of these steps, HA of the dependencies, certificate rotation, upgrades and
enterprise administration.
