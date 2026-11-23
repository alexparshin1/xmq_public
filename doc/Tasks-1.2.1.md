# XMQ 1.2.1 — tasks

Work planned for 1.2.1. An item leaves this file when it is released, not when it is coded: the
changelog records what shipped, this file records what is still owed.

These are the data-integration functions that an ordinary user of a small or home MQTT deployment
actually feels. The large connector catalog, a visual flow designer, an Avro/Protobuf schema
registry, behavior policies and warehouse-specific integrations (Snowflake, Kinesis, Pub/Sub)
belong to the enterprise tier of HiveMQ and EMQX and are deliberately **out of scope** here.

## Planned

- [ ] **Make the Kafka sink and source available on every farm system.**
  They are Linux-only today because they link librdkafka (`doc/DataIntegration.md`). A user on
  Windows or FreeBSD cannot send MQTT data to Kafka or consume from it at all, while the HTTP and
  database sinks work there. Acceptance: `kafka_sink` and `kafka_source` are built, packaged and
  tested on the Windows and FreeBSD farm images, with the same settings and the same TLS/SASL
  behavior as on Linux; the farm is green on every system.

- [ ] **Add an InfluxDB (time-series) sink.**
  `db_sink` covers PostgreSQL, MySQL, Oracle, SQLite and MSSQL only. A time-series database is the
  most common destination for sensor data in a small installation; today the operator has to put a
  proxy in front of `http_sink`. Acceptance: a `time_series_sink` module that writes line protocol
  over HTTP(S) to InfluxDB, configured from the same exporter settings form, using the shared
  persistent delivery queue, dead-letter topic and sink validation; documented in
  `doc/DataIntegration.md` and on the website, with tests on the farm systems.

- [ ] **Add an S3-compatible object-storage sink.**
  Cheap archival of exported data is a common small-scale need; HiveMQ covers it with a Data Lake
  extension. Acceptance: an `object_storage_sink` module that writes each exported body as an
  object to an S3-compatible endpoint (path-style access, configurable bucket, prefix and
  credentials), reusing the shared persistent delivery queue and dead-letter topic; documented and
  tested like the InfluxDB sink.

- [ ] **Widen the SQL rule syntax for ordinary filters.**
  `LIKE`, `IN` and `BETWEEN` are explicitly unsupported today (`doc/DataIntegration.md`, "WHERE
  syntax"), so a user who wants "topic contains" or "temperature in range" has to write awkward
  comparisons or several rules. Acceptance: those three operators work in both message and log
  rules, with the same NULL and type rules as existing comparisons, covered by
  `SqlEngine`/`WebhookTests` and documented; no change to the existing wire protocol.

- [ ] **Make the rule editor more approachable.**
  The `Test rule` button and field list already exist, but the editor offers no completion and the
  supported syntax is only discoverable from the manual. Acceptance: the rule editor suggests the
  available fields and operators, and reports an unknown field or operator at the position where
  it was typed; the extras are presentation-only and do not change what the broker accepts. A
  visual drag-and-drop designer is explicitly **not** part of 1.2.1.

## Out of scope for 1.2.1

The following were compared against HiveMQ and EMQX in the 1.2.0 discussion and are not planned:
50+ built-in connectors, Flow Designer, Avro/Protobuf schema registry and encode/decode, behavior
policies based on client state machines, Snowflake/Kinesis/GCP Pub/Sub/Pulsar/RabbitMQ integrations,
and per-tenant routing. They serve large fleets and corporate data platforms rather than the
ordinary user this release is for.
