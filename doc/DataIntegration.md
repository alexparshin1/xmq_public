# Data integration

Use `message_export` for MQTT messages and `log_export` for broker logs.
Each has its own filters, destination and delivery queue. One extension can be configured several
times: the Extensions screen lists **settings sets**, not extensions, so a single `message_export`
can have one set of filters and a database sink for `orders/#` and another set with a different
sink for `audit/#`. Give each set its own `extension_instance` label and, with persistent delivery enabled,
its own `delivery_queue_path`; two sets that subscribe to the same topic each receive a copy.
The old `webhook` still works in 1.2.x: **Deprecated. Will be disabled in version 1.3.** To migrate,
copy each source's settings to its replacement and turn off the old extension to
avoid duplicate deliveries.

## Validate incoming MQTT publications

Create `xmq_publish_schemas.json` beside the broker configuration and restart MQTT
to enforce JSON Schema on incoming publications. Each key is an MQTT topic
filter and each value is a Draft 2020-12 schema file. Relative paths are resolved
from the policy file directory.

```json
{
  "sensors/+/reading": "sensor-reading.schema.json"
}
```

An absent file or an empty object disables ingress validation. The broker
compiles each schema at startup and refuses to start MQTT if a configured schema
cannot be loaded or if the SPTK build lacks validation support. Configure the
same policy files on every broker accepting client traffic.

Matching authorized client publishes must satisfy every matching schema before
routing, retained storage, or export. A rejected MQTT 5 QoS 1 or 2 publish
receives `Payload format invalid`. QoS 0 has no acknowledgement, and MQTT 3.x
acknowledgements carry no failure reason. Empty retained publishes can still
clear a retained value. Internal cluster control and broker-originated messages
are outside client ingress validation.

## Validate the sink payload

Set `json_schema` on a message or log exporter to a JSON Schema Draft 2020-12
document. For example, `{"type":"object","required":["device_id"]}` requires an
object containing `device_id`. An omitted, empty, or whitespace-only setting skips schema validation.

The worker validates the rendered template or SQL result before it is queued
in SQLite or sent to a sink. With a dead-letter topic configured for persistent
delivery, the worker attempts to republish invalid bodies there. Otherwise it
drops them. Validation applies to new exports. Rows already in SQLite replay
as originally rendered.

Build SPTK with `SPTK_ENABLE_JSON_SCHEMA=ON` and build XMQ against that SPTK
installation. Invalid schemas and unavailable validation support cause the
exporter to reject the configuration. Validation affects export only. MQTT
subscribers still receive the original publication. The Extensions screen shows
schema rejection counts and the last failing JSON Pointer and keyword, along
with sink, SQLite queue, and dead-letter outcomes. It does not show payloads.

## Persistent delivery queue

With SQLite support, set `persistent_delivery = true` to opt in, then set
`delivery_queue_path` to an absolute SQLite file path inside an existing private
directory (mode 0700 on Unix). Persistent delivery is off by default. Use one file
per exporter and sink settings. An empty path while the switch is on prevents the
extension from starting. `delivery_queue_mib` caps live encoded data (default 256 MiB,
excluding SQLite overhead). A full queue rejects new records, which can be routed
to the dead-letter topic.

A separate writer stores the rendered body and bindings. MQTT acknowledgement
does not imply export persistence. The same queue and worker serve every sink —
`http_sink`, `db_sink` and `kafka_sink` — so retry, backoff and dead-lettering
behave alike. One worker delivers the queue head and removes it when the sink
reports success. With `kafka_sink`, this requires an acknowledgement
from Kafka. Persistent mode overrides the HTTP retry count and concurrency.
Failures retry with a delay capped at 60 seconds. Without a dead-letter topic,
permanent failures also block later records. Restart replays persisted records;
a crash after delivery but before removal can cause duplicates. Changing sink
settings requires a different queue file.

## Dead-letter topic

Set `dead_letter_topic = integrations/dead-letter/orders` on an individual
`message_export` or `log_export` with persistent delivery enabled. Leave it blank
to disable dead-letter publishing. Use a concrete MQTT topic and restrict
subscription access because the payload is the original rendered export body.

When the sink reports a permanent rejection, the exporter republishes the body
to that topic. For HTTP, 4xx responses other than 429 are permanent. Transient
errors continue to retry at the sink. A rejected head stays in SQLite until the
broker accepts the dead-letter publication and is retried with backoff if
acceptance fails. On restart the head can be tried at the primary sink again.
New records that exceed the SQLite queue limit also get one dead-letter attempt.
If the broker refuses that attempt, the record is dropped. Losses before the
SQLite writer receives a record are outside this path.

Dead-letter publications are local, non-retained QoS 0 messages that bypass
the export tap. The local publisher accepts payloads up to 16 MiB. Broker
acceptance is asynchronous and does not confirm
persistence or subscriber receipt. A crash after acceptance and before SQLite
deletion can result in duplicate delivery. Run a subscriber on the dead-letter
topic to collect the messages.

## Kafka sink

On Linux builds with librdkafka, select `kafka_sink` under Delivery. Set
`bootstrap_servers` to a comma-separated `host:port` list and `kafka_topic` to the
target topic. `kafka_timeout_ms` bounds the wait for the record's delivery report. The exported JSON
is the Kafka record value. A nonempty selected field or alias named `key` becomes
the record key. The sink uses PLAINTEXT by default and reports success only after
Kafka acknowledges the record (`acks=all`). For TLS, set
`kafka_security_protocol=SSL`; the broker certificate and hostname are verified
against the system trust store or a PEM CA file at an absolute path in
`kafka_ca_file`. For SASL, use `SASL_SSL` or `SASL_PLAINTEXT`, select
`SCRAM-SHA-256` or `SCRAM-SHA-512`, and set `kafka_sasl_username` and
`kafka_sasl_password`. On the test stand, Kafka offers SASL/SCRAM at
`theater:9094` for user `xmqtest`; the unauthenticated listener remains at
`theater:9092`. With persistent delivery, a timeout or outage leaves the record
on disk for retry. A lost acknowledgement can cause a duplicate.

## Kafka source

On Linux builds with librdkafka, select the standalone `kafka_source` extension.
Set `bootstrap_servers`, `kafka_topic`, `group_id`, and `mqtt_topic`. The Kafka topic
must already exist. Use `security_protocol=SSL` (default) or `PLAINTEXT` on a trusted
test network. For TLS, the Kafka certificate must match the Kafka broker hostname
and chain to a CA trusted on the XMQ host; hostname verification is enabled explicitly
in both TLS transports. The MQTT listener's certificate is not
used, and client certificates for mTLS are not configurable. For SASL, select
`SASL_SSL` or `SASL_PLAINTEXT`, a SCRAM mechanism, and the username and password.
`session_timeout_ms` (default 10000, allowed 6000–60000) controls how long Kafka
waits for this consumer before rejoining the group; it also bounds the wait when
the extension is applied or disabled.
The record value becomes the MQTT payload unchanged; Kafka null
values become empty payloads. Imports are not retained or passed to the export tap.

New groups read from the earliest available offset; existing groups resume from
committed offsets. Offsets are committed after the broker accepts a record into
its in-memory delivery queue, not after persistence or subscriber receipt.
A crash can lose an accepted publish or cause a duplicate on replay.

## Sinks for the export extensions

The SQL rule engine, syntax validation, Test rule and the rule editor are shared
by both export extensions. Delivery is a sink selected inside each extension:

- `http_sink` — POST the JSON produced by the rule.
- `kafka_sink` — sends the exported JSON to a Kafka topic (Linux builds with librdkafka).
- `db_sink` — parameterized INSERT through SPTK into PostgreSQL, MySQL, Oracle,
  SQLite or MSSQL. Parameters are bound, never concatenated into the SQL text.
  A bounded queue and a worker thread keep the MQTT path free. That in-memory
  queue is used only when persistent delivery is off: it is not durable and a
  full queue drops records. Enable persistent delivery on the exporter to route
  INSERTs through the same SQLite queue as the other sinks; committed records
  are then replayed after a restart. On a connection failure the worker reconnects
  with 1, 2, 4, 8, 16, 32 and then 60-second intervals, continuing once a minute
  until the record is written or the extension stops. A successful write resets
  the interval. A disconnect after a server-side commit may also cause a duplicate
  on retry. Use an idempotent INSERT if needed.

Provide a database URI and an INSERT statement, for example
`INSERT INTO exported (body) VALUES (:fields)`. The destination table must exist.
The sink binds the entire exported body as the `fields` parameter.

The engine is shared; sinks are selected per exporter. Adding a later sink
changes no existing extension's SQL rule code. If upgrading from a
development build with `messages_web_hook` and `log_web_hook`, rename the
configuration entries and update their library paths to the new installed names.

## SQL rules for MQTT messages and logs (1.2.0)

Set the optional `sql_rule` to filter MQTT messages and construct the JSON request body:

```sql
SELECT payload.device_id, payload.temperature AS temperature, qos
FROM "sensors/1", "sensors/2"
WHERE CAST(payload.temperature AS DOUBLE) > 80 AND NOT retain
```

Enable each exporter independently. MQTT **Filter** selects
**Topics** or **SQL rules**, not both. `FROM` accepts comma-separated quoted MQTT filters
and matches any of them. After a topic match, the export
worker dequeues the message and evaluates `WHERE`; only TRUE is sent. `SELECT` replaces
the body template in SQL mode. Separate rules with semicolons; the first matching rule
produces one request. Logs have their own **Filter** choice and accept, for example,
`SELECT timestamp, level, message FROM logs WHERE level = 'error'`. Log SQL mode replaces
the level/subject filter and log template. Existing configurations retain their behavior.
No SQL database is involved.

Use **Test rule** beside the SQL editor to evaluate the current draft against a
sample MQTT message or log record. The result shows whether it matched, the
number of the first matching rule, and its JSON field names and values. Testing
does not save settings, publish an MQTT message, or send an HTTP request.

Fields include `topic`, `client_id`, `username`, numeric `qos`, boolean `retain`, and
scalar `payload.field` paths. `SELECT *` also includes the complete `payload` as a UTF-8
JSON string (`null` for invalid UTF-8). Only rules using a payload path require valid JSON.
Comparisons do not coerce mixed types. Missing fields and failed casts yield NULL;
ordinary comparisons involving NULL yield UNKNOWN, including under NOT. Use
`IS [NOT] NULL` for explicit tests. `AND` binds before `OR`; parentheses override it.
Numeric comparisons use double precision. Strings are compared bytewise.

### WHERE syntax

- Comparisons: `=`, `!=`, `<>`, `<`, `<=`, `>`, `>=`.
- Boolean expressions: `AND`, `OR`, `NOT`, parentheses, and boolean fields such as `NOT retain`.
- Null tests: `IS NULL` and `IS NOT NULL`.
- Numeric conversion: `CAST(payload.temperature AS DOUBLE) > 80`.

Only `TRUE` matches; `FALSE` and `UNKNOWN` do not. `LIKE`, `IN`, `BETWEEN`,
regular expressions and `==` are not supported. Log `level` is text, not a severity
rank. To select warnings and errors:

```sql
SELECT * FROM logs WHERE level = 'warning' OR level = 'error'
```

## Original webhook implementation (1.1.0)

`[x]` means done; `[ ]` means not done, with what is left said underneath.

The goal is the first slice of the data-integration gap against EMQX and HiveMQ: a broker can hand
the messages it accepts to an external endpoint, with a filter, a secret and a retry policy. EMQX
calls the whole thing a rule engine; HiveMQ does it with extensions. This slice is the extension
path, and the foundation both a rule engine and a connector would need.

## What 1.1.0 delivers

- An extension can subscribe to a topic filter and receive the message body, not just the event.
- A reference `webhook` extension posts those messages as JSON over HTTP(S).
- The publish path is not blocked by a slow endpoint: a bounded queue, one worker, and a visible
  drop count.
- The endpoint can verify the sender with an HMAC signature.
- The settings appear on the Extensions screen like any other extension's.

## What it does not deliver

- No rule language, no JSONPath, no transformations. The payload arrives as it was published; a
  body template substitutes fields into a fixed body but does not reshape the payload.
- No Kafka, database, S3 or other connectors. Those are extensions of the same tap later.
- No replay, no exactly-once, no durable queue across a restart. Delivery is at-most-once through a
  bounded in-memory queue, and the drop count says what was lost.
- No per-tenant routing and no UI rule builder.

## Tasks

### DI-1. Message tap in the extension ABI

`[x]` Add `XMQ_CAP_MESSAGE_TAP` and bump `XMQ_EXTENSION_ABI_MINOR` to 1. Append to `xmq_extension`
a `message_tap` table and to `xmq_host` a `tap_messages(context, filter, max_payload)` function.
Add `xmq_message` with topic, payload, client id, username, qos, retain and timestamp. Pointers are
valid only during the callback; payloads are binary and not NUL-terminated.

Acceptance: an extension built against the new header subscribes in `start()`; an older broker
refuses it through the existing minor-version check; an older extension loads unchanged.

### DI-2. Tap delivery in the host

`[x]` `ExtensionHost` keeps one bounded queue per tapped extension, matching by topic filter. The
publish path copies the payload only for matching taps and never waits for the endpoint. A worker
thread delivers; a full queue drops and counts. `stop()` drains or discards deterministically.

Acceptance: with no taps, publish-path timing is unchanged; with a tap that sleeps, publishing
continues and the drop counter rises; a binary payload arrives byte-for-byte.

### DI-3. SDK wrapper

`[x]` `XmqExtensionBase` gains `MessageTap` and `subscribeMessages(filter, maxPayload)`; `onMessage`
receives a view over the payload. The existing authenticator, authorizer and observer are untouched.

### DI-4. Reference webhook extension

`[x]` `extension/webhook`: settings `url`, `secret`, `topic_filter`, `max_payload`, `timeout_ms`,
`template`, `headers`. POSTs the template body; with no template, one line per source:

```json
{"topic": "sensors/water/1", "message": "<payload in base64>", "level": "message"}
```

A log record arrives in the same three fields, with `topic` empty, `message` the log text and
`level` its severity. The other names (`id`, `timestamp`, `qos`, `retain`, `client_id`,
`username`, `payload_base64`, `subject`, `source`) are available to a template of the operator's
own.

HTTPS with certificate verification; `X-XMQ-Signature: sha256=<hmac>` over the body; retries with
backoff on 5xx, timeout and connection failure; no retry on 4xx except 429. At most one request in
flight per worker.

### DI-5. Settings and screen

`[x]` The extension declares its settings; the Extensions screen shows them. The URL and secret are
marked secret where the field supports it. A missing or malformed URL is refused in `start()` and
logged, not silently ignored.

### DI-6. Tests

`[x]` A test extension taps `sensors/#` and records payloads, including a binary one. A local HTTP
server receives the webhook, checks the HMAC and the JSON, and can answer 500 once to prove the
retry. A queue full test proves publishing is not blocked. A stopped extension delivers nothing.

### DI-7. Documentation

`[x]` A Webhook page under Extensions in the manual: what is sent, the signature, retries, the drop
counter and the security of the endpoint secret. It was first published as "Data Integration"; the
page and its Extensions-menu entry now carry the extension's own name.

### DI-8. Build and packaging

`[x]` The webhook extension is built by the normal extension machinery on Unix and on Windows
(Winsock for the socket layer, the same OpenSSL for TLS and the signature), installed with its
configuration template, and is off unless enabled. The Linux, FreeBSD and Windows packages all
carry it; the MSI names the DLL and the fragment explicitly, beside the user-database extension.

## Sequence

DI-1 -> DI-2 -> DI-3 -> DI-4 -> DI-5 -> DI-6 -> DI-7 -> DI-8.

DI-1 and DI-2 are the part that is certainly needed: every later connector, and a rule engine,
would use the same tap. DI-4 is the smallest useful endpoint; if Tomasz's answer asks for a
different envelope or a different first connector, only DI-4 and DI-5 change.

## Historical comparison (XMQ 1.1.0)

| | XMQ 1.1.0 after this | EMQX | HiveMQ |
|---|---|---|---|
| MQTT to HTTP | one filtered webhook extension | rule engine + HTTP action | extension |
| Transformations | none | SQL-like rules | extension |
| Kafka/DB | later extensions on the same tap | built-in connectors | extensions |
| Delivery guarantee | at-most-once, bounded queue | configurable, rules | extension-defined |

The tap is deliberately below the rule engine: it hands over a message, not a query language. That
keeps the ABI small and lets a rule engine be written as an extension later, without changing the
broker again.

### DI-9. Log records as a second input (prototype)

`[x]`

The webhook carries the broker's log as well as accepted messages, with one `sources` choice
(messages, log or both) and one endpoint for both. Records come from the single logging funnel
(`ServerData::logMessage`), so they include refusals from the account database, the permission
evaluator and the extension host - the faults that stop messages from existing at all.

### DI-10. Body templates and headers; several destinations - decided against

`[x]`

A prototype posted to several endpoints, each with its own template, filters and headers, described
by a JSON array in one `destinations` setting. It is removed: a JSON document typed by hand in a
string setting is not an interface, its `secret` was not masked, and an operator who wants several
receivers can put a web server in front of this one and fan the stream out there. The settings
screen and the broker stay one extension, one endpoint.

What the prototype proved is kept as ordinary settings: `template` (a body with `{{topic}}`,
`{{message}}`, `{{level}}`, ... substituted and JSON-escaped) and `headers` (a list of
`Name: value` items), beside the `sources` choice for what is posted.
