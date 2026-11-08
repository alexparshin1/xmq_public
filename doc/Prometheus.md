# Prometheus metrics

XMQ can expose its counters to Prometheus on a listener of its own. It is off unless the
configuration enables it, because a new listening port is not something a broker should open
without being asked.

## Configuration

The endpoint is configured on the **Prometheus** page of the configuration interface. Saving
replaces the listener at once: no broker restart, and no MQTT client is disconnected. The same
section can be edited in `xmq_server.conf` when the interface is not available:

```json
"prometheus": {
  "enabled": true,
  "bind_ip": "0.0.0.0",
  "port": 9184,
  "username": "",
  "password": ""
}
```

`username` and `password` enable HTTP Basic authentication for scrapes. Both empty leaves the
endpoint open, which is the default. The file keeps a one-way verifier for the password, not the
password itself, so set it through the interface rather than by hand.

| Key | Meaning | Default |
|---|---|---|
| `enabled` | Whether the listener is created. | `false` |
| `bind_ip` | Address to accept scrapes on. | `0.0.0.0` |
| `port` | Port to accept scrapes on. Must be above 1024. | `9184` |
| `username` | HTTP Basic username; empty means no authentication. | empty |
| `password` | HTTP Basic password, stored as a verifier; at least 8 characters. | empty |

The listener is not encrypted. With no username it does not authenticate the scraper either:
bind it to an address the scraper can reach and nothing else can, or put it behind the same network
policy as the metrics you already collect. With a username and password, every scrape must carry
HTTP Basic credentials. It answers one path:

```text
GET /metrics
```

Anything else gets `404`.

## A scrape configuration

```yaml
scrape_configs:
  - job_name: xmq
    metrics_path: /metrics
    static_configs:
      - targets: ["broker-1:9184", "broker-2:9184"]
    # Only when the endpoint has a username and password.
    basic_auth:
      username: metrics
      password: "the same secret entered on the Prometheus page"
```

A request without credentials, or with the wrong ones, is answered with `401 Unauthorized` and a
`WWW-Authenticate: Basic` challenge. The password is checked against the stored verifier, so a
copy of the configuration file does not yield the scrape password.

The endpoint is per node. A cluster of three nodes answers three series sets, each labelled by
`node` in `xmq_build_info`; aggregate in the query, not in the broker.

## Metrics

`xmq_up` is `1` while the MQTT broker is running and `0` while it is stopped or starting. The
process and host figures stay available in both cases, so a scrape can say why the broker is not
up.

| Metric | Type | Meaning |
|---|---|---|
| `xmq_build_info` | gauge | Version and node name, as labels. |
| `xmq_up` | gauge | Whether MQTT is being served. |
| `xmq_broker_clients_connected` | gauge | Clients connected now. |
| `xmq_broker_clients_maximum` | gauge | Most clients connected at one time. |
| `xmq_broker_clients_total` | counter | Clients that have connected since the start. |
| `xmq_broker_clients_disconnected_total` | counter | Clients that have disconnected since the start. |
| `xmq_broker_messages_received_total` | counter | MQTT messages received. |
| `xmq_broker_messages_sent_total` | counter | MQTT messages sent. |
| `xmq_broker_publish_received_total` | counter | PUBLISH messages received. |
| `xmq_broker_publish_sent_total` | counter | PUBLISH messages sent. |
| `xmq_broker_publish_dropped_total` | counter | PUBLISH dropped because a session queue was full. |
| `xmq_broker_retained_messages` | gauge | Retained messages held. |
| `xmq_broker_stored_messages` | gauge | Messages held for later delivery. |
| `xmq_broker_bytes_received_total` | counter | Bytes received from clients. |
| `xmq_broker_bytes_sent_total` | counter | Bytes sent to clients. |
| `xmq_broker_subscriptions` | gauge | Subscriptions held. |
| `xmq_broker_cluster_messages_received_total` | counter | Publications received from other cluster nodes. |
| `xmq_broker_queue_receive` | gauge | Messages waiting in the receive queues. |
| `xmq_broker_queue_send` | gauge | Messages waiting in the send queues. |
| `xmq_broker_queue_delivery` | gauge | Messages waiting in the delivery queues. |
| `xmq_broker_uptime_seconds` | gauge | Seconds since the broker started serving. |
| `xmq_host_cpu_percent` | gauge | Whole-machine CPU use, percent of all cores. |
| `xmq_host_cpu_cores` | gauge | Hardware threads on this machine. |
| `xmq_host_memory_used_bytes` | gauge | Memory in use, excluding reclaimable cache. |
| `xmq_host_memory_available_bytes` | gauge | Memory available to start new work. |
| `xmq_host_memory_total_bytes` | gauge | Physical memory installed. |
| `xmq_process_cpu_percent` | gauge | This process's CPU use, percent of one core. |
| `xmq_process_resident_memory_bytes` | gauge | Resident memory of this process. |
| `xmq_process_uptime_seconds` | gauge | Seconds since this process started. |

## What it does not do

- No OpenTelemetry export, traces or logs. Only the text exposition format over HTTP.
- No TLS and no per-metric filtering. HTTP Basic authentication is available; TLS is expected to
  be provided by a proxy or a private network.
- No cluster-wide aggregation: each node answers for itself.
- No historical samples. Prometheus is expected to keep them.
