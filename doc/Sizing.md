# Sizing an XMQ broker, and what a restart costs

This is what the measurements behind 1.0.1 say about planning a deployment, written down so the
next investigation starts from them instead of repeating them. The numbers come from the load
scenarios in `LoadTest/` and from the two-machine test stand (broker on one host, generator on
another), not from AWS; the AWS sets are filed under `LoadTest/results/AWS/`.

## What a broker holds for one session

A session is a connection, its subscriptions, and whatever is waiting to be delivered to it. The
last part is the one that grows without a bound of its own, and it is bounded by `queue_limits`:

| Key | Shipped value | Meaning |
|---|---:|---|
| `max_inflight_messages` | 128 | Messages sent but not yet acknowledged. Also sent to the client as `Receive Maximum`. |
| `max_size` | 1024 | Messages the session may hold, in flight and waiting together. |
| `max_bytes` | 1 MiB | Payload and topic bytes the session may hold, in flight and waiting together. `0` means no byte limit. |

`max_size` and `max_bytes` are two limits on the same thing - what the session holds - and they are
independent: whichever is reached first is what refuses the message. One is not converted into the
other, because a message count says nothing about a queue of 256 KB payloads. `max_inflight_messages`
is a third and different thing: the protocol's window, which decides how many messages may be in
flight before the rest wait, and which does not override the two limits.

They apply per session and per QoS 1 and 2 message; QoS 0 is not queued. When either limit is
reached the **newest** message is dropped, because what the session already holds has waited longest
and is the closest to being delivered. The drop is logged once and then every thousandth message,
and the total is published on `$SYS/broker/messages/publish/dropped` and returned by the control API
as `messages_publish_dropped`.

So a session holds at most `max_size` messages and at most `max_bytes` bytes - with the shipped
values 1024 messages or 1 MiB, whichever its traffic reaches first. A client that stops reading is
not hypothetical: the case the limits were written for was 3000 subscriptions fanned out to one
topic, which took each of two Windows brokers to 16-17 GB of virtual memory on 8 GB machines and
stopped both services.

**Plan for the worst case, not for the average.** A broker on 8 GB of RAM serving a fan-out
workload should not be planned at all: with the shipped 1024 messages and a 16-byte payload the
queues are small, but a topic carrying larger payloads or a client base that keeps many sessions
away at once eats that budget quickly. The limits above are the first thing to lower when the
machine is small - they are per session, so their cost multiplies with the number of sessions that
are not keeping up.

## Latency and throughput are not the same budget

The queue's cost is memory, not speed: filling and draining it takes the session's lock, and the
broker's throughput does not fall as the limits get smaller. What limits a deployment is the
machine, not the configuration, in both of the directions below.

## A restart: sessions come back before clients do

Two different things happen after a broker restart, and they have different costs.

**Restoring sessions** is the broker's own work: reading what the storage holds for each session
and re-creating it. It is batched and pipelined, so 60 000 persistent sessions are restored in
about 1.1 seconds (~52 000 sessions/s). A million sessions would be around twenty seconds of
storage reads before the first client is served.

**Accepting the reconnecting clients** is the clients' work, and it is slower because every
connection costs a TCP handshake, a CONNECT and a CONNACK, on both sides. Measured end to end at
about 220 µs per connection on the test stand, so:

| Clients reconnecting | Time to full service |
|---:|---:|
| 10 000 | ~2 s |
| 60 000 | ~13 s |
| 120 000 | ~35-40 s |

The 120 000 figure is measured, with the broker back at its full 59 593 connections/s once the
storm is over. It is a property of the machine - the client-side generator saturates before the
broker does - and it does not improve with a smaller backoff, a larger connection pool or more
client worker threads. Those were measured and ruled out. If a restart window matters, the way to
shorten it is more client machines spreading the reconnects, not broker tuning.

## Client-side reconnects

`xmq_scn` reconnects every lost client through one shared scheduler: a reactor waits for the
non-blocking connects, and a small worker pool does what still blocks (TLS handshake, CONNECT, the
wait for CONNACK). Before 1.0.1 it started a thread per client and slept in it between attempts,
which at 120 000 clients meant 120 000 threads and 600 000 wake-ups a second; the generator died
with `Resource temporarily unavailable` before it measured anything.

The settings are `auto_reconnect`, `reconnect_interval` and `reconnect_attempts` in a scenario
file. Backoff, jitter and the attempt limit behave as they did: a client that loses its connection
waits `reconnect_interval`, retries, and gives up after `reconnect_attempts` - it does not pile up
retries while the broker is still coming back, because one attempt per client is in flight at a
time.

For a client application rather than the generator, the same rule applies: a client that
reconnects on a fixed interval with no jitter synchronises with every other client that was
dropped at the same moment, and turns a restart into a thundering herd. Exponential backoff with
jitter costs nothing and keeps the reconnect rate under what the broker can accept.

## Clusters: no load balancer is needed

A client may connect to any node of a cluster and takes its session, with its subscriptions and
what was queued, wherever that session was before. Nothing in front of the cluster is required for
that; a load balancer only distributes the initial connections. `xmq_scn` connects to nodes
round-robin, and on a failed connection moves to the next node, which is what a client library
should do.

What a load balancer does not solve is the reconnect storm above: it spreads the connections, not
the work of accepting them, and every node still pays the same per-connection cost.
