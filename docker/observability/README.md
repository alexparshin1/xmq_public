# XMQ, Prometheus and Grafana

A small stack that shows what the broker exposes, without assembling the monitoring side by hand:

```text
XMQ (9184/metrics)  ->  Prometheus (9090)  ->  Grafana (3001)
```

## Requirements

- Docker and Docker Compose.
- An XMQ image with the Prometheus endpoint: **1.1.1 or later**. Set `XMQ_VERSION` to one that
  has it; `latest` follows the published image.

## Start

```bash
cd docker/observability
XMQ_VERSION=1.1.1 docker compose up -d
```

Grafana is published on **3001**, not its usual 3000: the XMQ unit tests bind 3000, and a stack
left running would make them fail with "address already in use".

| What | Where | Credentials |
|---|---|---|
| Grafana | http://localhost:3001 | `admin` / `admin`, or `GRAFANA_PASSWORD` |
| Prometheus | http://localhost:9090 | none |
| XMQ configuration interface | https://localhost:18883 | none until the administrator password is set |
| XMQ metrics | http://localhost:9184/metrics | none |
| MQTT | `localhost:1883`, TLS `localhost:8883` | anonymous |

The dashboard is provisioned from `grafana/dashboards/xmq-overview.json` and appears under the
**XMQ** folder. Alert rules are in `prometheus/xmq-alerts.yml`.

Anonymous MQTT access is on so the stack works before any account exists. Set
`XMQ_ALLOW_ANONYMOUS=false` and create accounts in the interface before the ports are reachable
from a network you do not control.

Stop it with:

```bash
docker compose down          # keeps Prometheus and Grafana data
docker compose down -v       # also removes the volumes
```

## Protecting the metrics endpoint

Set a username and password on the **Prometheus** page of the interface. Then add credentials to
`prometheus/prometheus.yml`:

```yaml
    basic_auth:
      username: metrics
      password: "the same secret entered there"
```

and reload the configuration:

```bash
curl -X POST http://localhost:9090/-/reload
```

Scrapes without valid credentials are answered with `401`. The password is stored as a verifier,
not as text; the interface shows it masked.

## Health probes

The same listener answers two endpoints that need no credentials, for a load balancer or a
Kubernetes probe:

```bash
curl -i http://localhost:9184/healthz   # 200 {"status":"ok"} while the process is alive
curl -i http://localhost:9184/readyz    # 200 ready, 503 not ready with a reason
```

`/readyz` is `503` while MQTT is stopped or starting, and on a cluster node that does not hold its
lease - the same condition under which the node refuses clients. A Kubernetes probe looks like:

```yaml
livenessProbe:
  httpGet: {path: /healthz, port: 9184}
  periodSeconds: 10
readinessProbe:
  httpGet: {path: /readyz, port: 9184}
  periodSeconds: 5
```

## Is Grafana needed for testing?

No. The broker's tests exercise `/metrics` directly (format, values, authentication, status codes);
Prometheus and Grafana are for people looking at the result. The stack is here to validate the
dashboards and alert rules by eye, and to give a working example to copy from - not as a test
dependency.
