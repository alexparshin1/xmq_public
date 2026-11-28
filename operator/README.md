# XMQ Kubernetes operator (MVP)

This directory contains the first half of the operator discussed in
`doc/KubernetesPilot-1.2.1.md`. It automates the steps that the runbook performs by hand:
CRD, workloads, certificates and initial setup. Redis and PostgreSQL stay **external** — the
operator connects to them through URIs and never creates them.

## What it does

Given an `XMQCluster`, the controller creates or updates:

- a ConfigMap with one broker configuration per pod, generated for XMQ 1.2.1;
- a ConfigMap with the `user-database` authenticator fragment;
- a headless Service and a NodePort Service;
- a StatefulSet with `OrderedReady` pod management (so only one node creates the SQL schema),
  a PVC per pod for `/etc/xmq`, and certificate/CA mounts from Secrets;
- a one-shot Job that sets the administrator and cluster account passwords through the broker CLI.

After the Job succeeds, the operator writes the plain cluster secret into
`xmq_cluster_credential` in PostgreSQL, which is required by outgoing cluster links.

`status.conditions` reports `StatefulSetReady`, `SetupComplete`, `ClusterCredentialReady` and
`Ready`. `Ready` becomes true only when all brokers are ready and cluster links can authenticate.

## Supported versions and platforms

- Supported XMQ version: **1.2.1** only. The operator generates a configuration from the 1.2.1
  schema; a version with new required fields needs an operator update. `spec.version` is validated
  and an unsupported value sets `Ready=False` with an explanation.
- Target cluster: Kubernetes 1.37 and close minor versions (controller-runtime v0.25 / k8s.io v0.37).
- Image platform: Linux/amd64, matching the published XMQ image. arm64 needs a separate image.
- Go is needed **only to build the operator image**. Kubernetes nodes and the load-test farm do not
  need Go. The operator container is built `CGO_ENABLED=0` on a distroless base.

## Prerequisites

Before applying an `XMQCluster`, create:

- a Secret with a common CA (`ca.crt`) and one Secret per pod (`node.crt`, `node.key`) — see
  `doc/k8s-pilot/gen-certs.sh`;
- a Secret with keys `admin` and `cluster` for the passwords;
- externally reachable Redis and PostgreSQL, and the `postgres` Go driver can connect via
  `spec.databaseURI`.

## Build and install

```bash
cd operator
make build          # go build ./...
make image          # docker build -t xmq-operator:1.2.1 .
make deploy         # CRD + RBAC + manager
kubectl -n xmq apply -f config/samples/xmq_v1_xmqcluster.yaml
kubectl -n xmq get xmqcluster xmq -o yaml
```

## Scope / not included

This MVP does not manage Redis or PostgreSQL, does not integrate with cert-manager, does not
rotate certificates automatically, and has no mixed-version upgrade logic. Those are the next
steps toward a commercial operator. The manual runbook remains the specification and the fallback.

## Layout

| Path | Purpose |
|---|---|
| `api/v1/` | `XMQCluster` CRD types and generated deep-copy methods |
| `controllers/` | reconcile loop: ConfigMaps, Services, StatefulSet, setup Job, SQL credential sync |
| `config/crd/`, `config/rbac/`, `config/manager/`, `config/samples/` | deployment manifests |
| `main.go`, `Dockerfile`, `Makefile` | manager entrypoint and build |
