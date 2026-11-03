# Data integration, 1.1.0

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

- No rule language, no JSONPath/templates, no transformations. The payload arrives as it was
  published.
- No Kafka, database, S3 or other connectors. Those are extensions of the same tap later.
- No replay, no exactly-once, no durable queue across a restart. Delivery is at-most-once through a
  bounded in-memory queue, and the drop count says what was lost.
- No per-tenant routing and no UI rule builder.

## Tasks

### DI-1. Message tap in the extension ABI

`[ ]` Add `XMQ_CAP_MESSAGE_TAP` and bump `XMQ_EXTENSION_ABI_MINOR` to 1. Append to `xmq_extension`
a `message_tap` table and to `xmq_host` a `tap_messages(context, filter, max_payload)` function.
Add `xmq_message` with topic, payload, client id, username, qos, retain and timestamp. Pointers are
valid only during the callback; payloads are binary and not NUL-terminated.

Acceptance: an extension built against the new header subscribes in `start()`; an older broker
refuses it through the existing minor-version check; an older extension loads unchanged.

### DI-2. Tap delivery in the host

`[ ]` `ExtensionHost` keeps one bounded queue per tapped extension, matching by topic filter. The
publish path copies the payload only for matching taps and never waits for the endpoint. A worker
thread delivers; a full queue drops and counts. `stop()` drains or discards deterministically.

Acceptance: with no taps, publish-path timing is unchanged; with a tap that sleeps, publishing
continues and the drop counter rises; a binary payload arrives byte-for-byte.

### DI-3. SDK wrapper

`[ ]` `XmqExtensionBase` gains `MessageTap` and `subscribeMessages(filter, maxPayload)`; `onMessage`
receives a view over the payload. The existing authenticator, authorizer and observer are untouched.

### DI-4. Reference webhook extension

`[ ]` `extension/webhook`: settings `url`, `secret`, `topic_filter`, `max_payload`, `timeout_ms`.
POSTs JSON:

```json
{
  "id": "node-timestamp-sequence",
  "version": 1,
  "timestamp": "2026-11-03T19:45:00.123Z",
  "topic": "sensors/water/1",
  "qos": 1,
  "retain": false,
  "client_id": "device-1",
  "username": "user7",
  "payload_base64": "..."
}
```

HTTPS with certificate verification; `X-XMQ-Signature: sha256=<hmac>` over the body; retries with
backoff on 5xx, timeout and connection failure; no retry on 4xx except 429. At most one request in
flight per worker.

### DI-5. Settings and screen

`[ ]` The extension declares its settings; the Extensions screen shows them. The URL and secret are
marked secret where the field supports it. A missing or malformed URL is refused in `start()` and
logged, not silently ignored.

### DI-6. Tests

`[ ]` A test extension taps `sensors/#` and records payloads, including a binary one. A local HTTP
server receives the webhook, checks the HMAC and the JSON, and can answer 500 once to prove the
retry. A queue full test proves publishing is not blocked. A stopped extension delivers nothing.

### DI-7. Documentation

`[ ]` A Data Integration page under Documentation: what is sent, the signature, retries, the drop
counter and the security of the endpoint secret.

### DI-8. Build and packaging

`[ ]` The webhook extension is built by the normal extension machinery, installed with its
configuration template, and is off unless enabled.

## Sequence

DI-1 -> DI-2 -> DI-3 -> DI-4 -> DI-5 -> DI-6 -> DI-7 -> DI-8.

DI-1 and DI-2 are the part that is certainly needed: every later connector, and a rule engine,
would use the same tap. DI-4 is the smallest useful endpoint; if Tomasz's answer asks for a
different envelope or a different first connector, only DI-4 and DI-5 change.

## Comparison

| | XMQ 1.1.0 after this | EMQX | HiveMQ |
|---|---|---|---|
| MQTT to HTTP | one filtered webhook extension | rule engine + HTTP action | extension |
| Transformations | none | SQL-like rules | extension |
| Kafka/DB | later extensions on the same tap | built-in connectors | extensions |
| Delivery guarantee | at-most-once, bounded queue | configurable, rules | extension-defined |

The tap is deliberately below the rule engine: it hands over a message, not a query language. That
keeps the ABI small and lets a rule engine be written as an extension later, without changing the
broker again.
