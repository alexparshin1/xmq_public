# Acknowledgement after durable storage (experimental)

`persistence.ack_after_store=true` enables a standalone publication transaction. It is opt-in;
without the setting the existing asynchronous delivery and write-behind behavior is unchanged.
This is the first implementation of acknowledgement after storage, not a replacement for Redis HA.

```json
"persistence": {
  "enabled": true,
  "redis_uri": "redis://127.0.0.1:6379/0",
  "clean_start": false,
  "max_queued_writes": 0,
  "ack_after_store": true
}
```

Use Redis with AOF enabled and the `WAITAOF` command (Redis 7.2 or newer). The Redis account needs
permission for that command, `SCRIPT LOAD`, `EVALSHA`, and the existing session commands. The
publication script is loaded before startup and invoked by SHA thereafter. A `NOSCRIPT` response
reloads it and retries once on the same connection; other errors are never retried. `WAITAOF`
still follows successful execution before any durable acknowledgement. XMQ never changes
Redis configuration. Enabling/disabling this mode, changing its storage endpoint or resetting its
state requires a stopped broker and configuration edit; live persistence changes are rejected.
When the node configuration lives in SQL, the switch is stored in `xmq_node.persist_ack_after_store`;
existing rows migrate with the previous behavior disabled.

## What an ACK means

For a permitted client publication XMQ matches local subscribers on the receiving thread and stages:

- queued QoS 1/2 deliveries to persistent sessions, with their packet identifiers;
- the node membership needed to discover those deliveries after restart;
  session records are confirmed separately before CONNACK/SUBACK;
- a retained update or clearing, even without subscribers;
- the inbound QoS 2 packet identifier, for a persistent publishing session.

A Lua script checks every involved Redis key type before writing and applies the batch atomically.
No delivery is queued or extension tap notified before the script succeeds. XMQ then requests
`WAITAOF 1 0 5000` on the **same connection** that performed the script. A successful PUBACK/PUBREC
requires confirmation that the local AOF has been fsynced. PUBREL deletes the inbound QoS 2 identifier
and confirms fsync before PUBCOMP; a subsequent publication may then reuse that identifier.
A repeated QoS 2 PUBLISH uses the existing identifier and does not create another delivery.

Publication staging is ordered per broker, but does not hold that lock while waiting for disk.
A dedicated writer combines up to 4096 requests (4 MiB normally) into one validated Lua operation
and one `WAITAOF` barrier. QoS 1 receive handlers submit their work and remain available for other
clients and keep-alives. SUBSCRIBE also returns without occupying a receive worker; SUBACK
remains behind the durable barrier and is bound to its original connection. The successful PUBACK is queued only after the batch is confirmed durable.
Delivery updates and ACK cleanup use that same ordered journal, avoiding competing AOF flushes
through separate pool sockets. After each barrier, a separate FIFO completion worker installs the
results and sends responses while the writer persists the next batch. Pending limits include both
unwritten and written-but-incomplete work. A completion failure withholds later success responses;
`drain()` waits for installation and completion, and shutdown joins both workers.
This is not ACK-before-store write-behind.

The ordinary pending journal is bounded to 131072 requests and 128 MiB.
In this standalone mode, a persisted delivery omits the publisher client id from the internal
sender string. Subscription matching (including No Local) already selected the recipient, and
recovery queues that delivery directly without matching again. Live messages keep their sender;
outgoing topic, payload, properties, flags, subscription ids, source node and packet id are retained.
The string field stays in its legacy position, so records with a sender remain readable and the
record layout is unchanged. The ordinary asynchronous mode still persists the original sender.

For outgoing QoS 1 only, cleanup of ACKed delivery records coalesces by recipient for up to
4 seconds, or 64 ACKs for that recipient. A group becomes one multi-field HDEL in the same
validated script; the account must permit HDEL. Deferred and already-submitted cleanup share
an additional budget of 262144 record identifiers and 128 MiB of conservative accounting.
When that budget fills, cleanup falls back to the ordinary bounded journal and requests an
early flush. `drain()` and shutdown flush deferred cleanup immediately.
At a buffering deadline the current group of recipients is frozen; new ACKs collect separately.
The writer enqueues at most 1024 frozen recipient groups per pass, avoiding one large queue-lock
hold. Older groups cannot be extended by new ACKs, and an explicit drain includes both groups.
This delays only removal of records whose subscriber has ACKed them. Every new publication
still crosses its fsync barrier before a PUBACK or delivery. A process crash can replay more
already-ACKed QoS 1 deliveries during this cleanup window; storage completion can lag the
buffering deadline if the backend is slow. QoS 2 cleanup is not deferred.
 Recipient queue limits reserve space
for uncommitted deliveries too; rollback releases it and commit converts it into a queued record.
A queue-limit race cannot discard a delivery already accepted into storage. A failed script rejects
the whole batch. After an ambiguous response or a failed durability barrier the store fails closed
until restart. Applied records are installed even after a fence failure, but durable-only responses
(including SUBACK) are withheld.

Persistent-session creation and subscription updates also wait for their local AOF fsync.
They use the reliable store's dedicated serialized connection too: a raw WAITAOF command must
never run on a shared pool socket concurrently with another session's commands. That
prevents an acknowledged publication from relying on a subscription that disappears on restart.
Session discovery membership is confirmed before CONNACK, so publications do not repeat SADD for
every recipient. Startup replays already-durable subscription records without rewriting partial
prefixes of them. A live restored CONNECT confirms discovery and stores the complete restored state
before CONNACK. A SUBSCRIBE fences pending retained changes before reading the retained view.
An AOF write error, fsync timeout or unavailable Redis withholds success; there is no memory fallback.
If the script's response is lost, its outcome is ambiguous. The broker refuses further reliable
publication transactions until restart, when persistent sessions and packet IDs are reloaded.
A successful script cannot be rolled back by an fsync timeout: its deliveries are installed, but the
publisher receives no success ACK. A retry of QoS 1 can therefore duplicate a delivery, as MQTT permits.

If MQTT startup fails in this mode, the controller keeps MQTT stopped instead of falling back to an
older configuration that might disable the guarantee. The management interface can remain available.

## Boundaries

- **Standalone only.** Configured cluster membership or enabled bridges are rejected. Live cluster
  joins and bridge changes are also rejected. Durable transfer between brokers and recovery of shared
  deliveries on another node require a separate implementation; this switch must not imply them.
- **Local disk only.** The barrier requests zero replica confirmations. Losing the Redis machine or
  promoting a replica that lacks the write is outside this guarantee. Redis HA, replica persistence,
  failover fencing and a tested promotion policy remain separate work.
- MQTT QoS 0 and clean-session subscriptions retain their normal MQTT semantics; no offline queue is
  promised for them. Expiry, session termination and deliberate administrative deletion still apply.
- The crash tests exercise the broker's incoming QoS 2 exchange and persistent outgoing queues. They
  do not establish full MQTT conformance or transactional behavior of downstream applications.
- fsync is on the critical path. With Redis `appendfsync everysec`, a single publication can wait for
  the next fsync cycle. Evaluate `appendfsync always` and measure latency/throughput on the target disk.
  Group commit amortizes this cost under load; a replicated message journal remains future work.
- An abrupt host/power failure is not simulated by killing processes. The storage and filesystem must
  honor fsync, and their actual failure model needs independent acceptance testing.

## Verification

Run the `XMQ_ReliableStoreTests.*` GTest suite with `XMQ_TEST_REDIS` pointing to a **private** Redis
instance with AOF enabled. These tests skip with a prerequisite explanation if AOF/WAITAOF is missing;
a skipped test is not acceptance evidence. Ordinary persistence/storage tests must also pass.

The process crash campaign creates its own Redis, broker, configuration and ports:

```sh
python3 test/integration/reliable_store_crash.py --broker /absolute/path/to/xmq_server
```

It uses raw MQTT packets, reads the actual PUBACK/PUBREC/PUBCOMP, and immediately SIGKILLs both
processes. After restart it verifies queued messages, retained updates/clearings and QoS 2 retries.
It also checks write-error atomicity, Redis unavailability and rejection of AOF-disabled startup.
The script prints its artifact directory and retains logs and `results.json` for review.

Do not merge this branch into `1.1.1` until the user accepts the test results.
