Our latest XMQ release is out. Here is where the broker stands today.

Data integrations: one extension, many named configurations. Export MQTT messages or broker logs to a database, HTTP or Kafka, with a durable queue, retries and a dead-letter topic. Incoming publishes can be validated against JSON Schema before delivery.

Authentication: JWT, an HTTP service, or LDAP/Active Directory alongside SQL accounts and certificate-bound identities. Topic permissions stay with the broker's own rules.

Operations: Prometheus metrics and health probes, structured JSON logs, a ready-made Grafana stack, MQTT over WebSockets, and bridge topic mapping.

No performance regression: the same bench set with the same client and configuration measured 234 µs, against 242 µs for the previous release.

Two things to know: the JWT, HTTP and LDAP modes are still experimental, and custom extensions need rebuilding for the new version.

Source and docs: github.com/alexparshin1/xmq_public — xmq.sptk.net

#MQTT #OpenSource #IoT #Kafka #Observability
