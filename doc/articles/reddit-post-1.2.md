# XMQ today: data integrations, flexible authentication, and observability

XMQ is an open-source MQTT broker written in C++, with a web configuration UI, a native permissions model, and extensions for authentication and data export. This is what it can do today.

## Data integrations

- **Named integrations.** `message_export` for MQTT messages and `log_export` for broker log records can each be configured several times in one broker. Every settings set has its own name, filters, destination, delivery queue and durable queue identity, so renaming one never breaks its queue. The Extensions screen lists the integrations and offers an **Add Data Integration** dialog.
- **Destinations.** An integration can deliver to a database (`db_sink`), an HTTP endpoint (`http_sink`) or Kafka (`kafka_sink`). Persistent delivery uses a per-integration SQLite queue, retries with capped backoff, and an optional dead-letter topic for records that never make it out.
- **Kafka into MQTT too.** The standalone `kafka_source` extension subscribes to Kafka topics and publishes the records as MQTT messages, verifying the Kafka broker's certificate and hostname over TLS.
- **SQL rules.** An integration can select a topic filter, a typed predicate and JSON output fields with `sql_rule`; the rule is evaluated in the extension worker, without adding a database lookup to message delivery.
- **Validation before delivery.** `xmq_publish_schemas.json` maps topic filters to Draft 2020-12 JSON Schema files. Matching publications are checked before they are delivered and invalid ones are rejected.

## Authentication and permissions

- **Several ways to identify clients.** Alongside SQL-backed accounts, the broker supports JWTs verified locally against a configured key or JWKS endpoint (including key rotation and group claims), an HTTP service that answers allow/deny with optional group names, and LDAP or Active Directory with StartTLS/LDAPS, nested AD groups and disabled-account checks.
- **Certificate-bound identities.** An SSL key can require the MQTT username to match a certificate field and check client CRLs at CONNECT. Existing password and extension authentication, and native topic permissions, still apply.
- **Permissions stay native.** Topic access is managed in **User Management** with the broker's own rules. The separate ACL editor is gone, and returned directory groups are mapped onto those same native rules.

## Connectivity

- **MQTT in the browser.** `MQTT+WS` and `MQTT+WSS` listeners sit alongside TCP and TLS, handling the HTTP upgrade, the `mqtt` subprotocol and WebSocket ping/pong and close frames.
- **Bridge topic mapping.** `local_prefix` and `remote_prefix` map topic namespaces in both directions; bridges configured without them keep their existing topic names.

## Observability and operations

- **Metrics and probes.** A Prometheus endpoint serves `/metrics`, plus `/healthz` for liveness and `/readyz` for whether MQTT can serve clients. HTTP Basic auth is optional and the endpoint is off by default.
- **Logs you can query.** `logging.format=json` writes one JSON object per line with timestamp, level, node, subject, component and message. A Docker stack with Prometheus, Loki, Grafana, provisioned dashboards and alerts is included.
- **Metrics over MQTT.** `$SYS/broker/system/cpu/percent` and `$SYS/broker/system/memory/rss_bytes` expose the broker process's own CPU and memory, read by the same sampler that fills the Prometheus gauges.
- **Failures are visible.** Storage backpressure and cluster connectivity are on the metrics endpoint. A broker now refuses to start if another process already holds one of its listener ports, instead of silently splitting clients with it.

## Configuration and extensions

- Declarative extension forms describe sections, conditional fields and presets without scripts; settings can declare a format that both the UI and the broker enforce.
- The Extensions page lists installed extensions whether or not they are configured, including stopped ones with the version and description they report.
- Configuration is split into three pages with two independent switches: one loads an extension, the other persists its settings. Required extensions cannot be disabled.

## Durability

Experimental **ack-after-store** makes publish acknowledgements wait for the local AOF fsync when persistence is enabled, so a successful ack means the message is on disk. Storage errors withhold success. The broker tells clients its storage mode in CONNACK, and the load generator writes acknowledgements one at a time when it sees a durable broker.

## Performance

We re-ran the same bench set with the same client and configuration as the previous release and found **no regression**: point-to-point measured 234 µs, against 242 µs before. An earlier apparent regression turned out to be an unoptimised load-test client, not the broker.

## Worth knowing

- Custom extensions built for an older release need to be rebuilt for the version you install.
- JWT, HTTP auth and LDAP are explicitly experimental; their settings and cache behaviour may still change.

Links: [source and issues](https://github.com/alexparshin1/xmq_public), [documentation](https://xmq.sptk.net/), [downloads and changelog](https://xmq.sptk.net/downloads).

Happy to answer questions about any of the above — especially the auth extensions and data integrations.
