# LavinMQ 2.10.3: exploratory runs

Not published. These are the first runs against LavinMQ, to learn what its MQTT and cluster
implementations can do before deciding whether and how to put it on the test pages.

## What was tested

- LavinMQ 2.10.3 from the packagecloud Debian/Ubuntu repository, on AWS `c5n.2xlarge`
  nodes (8 vCPU, 21 GiB), one node for the single-node runs and four nodes for the cluster,
  with a separate `c5n.4xlarge` load client.
- The same `xmq_scn` client and scenario shape as the other tests, with one necessary change:
  **LavinMQ supports MQTT 3.1.0/3.1.1 only.** MQTT 5 connections are refused, so every scenario
  here sets `protocol_version` to 4 and no run uses MQTT 5 features.
- `clean_session=true` throughput scenarios, and one `clean_session=false` persistence scenario.
  LavinMQ implements a persistent session as a durable queue, and QoS 1 publishes are synced to
  disk before PUBACK.
- Cluster mode: four LavinMQ nodes, three `etcd` 3.5.16 members, leader-based replication.
  Followers proxy MQTT, AMQP and HTTP to the leader.

## Limitations found before any load

- **MQTT 5 is not supported.** A protocol-5 CONNECT is refused; only 3.1 and 3.1.1 work.
- **QoS 2 is downgraded to QoS 1.**
- **Shared subscriptions are not supported**, so the Fan-In scenario (`$share/...`) cannot run.
- A cluster follower does not serve clients itself: it proxies every connection to the leader.
  A LavinMQ cluster is therefore an availability design, not a scale-out design.

## Single node

| Scenario | Clients | Offered | Median latency | Average latency | Result |
|---|---:|---:|---:|---:|---|
| Point-to-point | 1,000 pub + 1,000 sub | 2,000 msg/s | 156 µs | 145 µs | pass |
| Fan-out | 5 pub + 100 sub | 25,000 msg/s delivered | 492 µs | 493 µs | pass |
| Point-to-point, persistent sessions | 1,000 pub + 1,000 sub | 1,000 msg/s | 157 µs | 141 µs | pass |
| Point-to-point | 2,000 + 2,000 | 4,000 msg/s | 143 µs | 384 µs | pass |
| Point-to-point | 3,000 + 3,000 | 6,000 msg/s | 158 µs | 803 µs | pass |
| Point-to-point | 4,000 + 4,000 | 8,000 msg/s | — | — | fail: 3,591/4,000 subscriptions in 30 s |
| Point-to-point | 5,000 + 5,000 | 10,000 msg/s | — | — | fail: 3,681/5,000 subscriptions in 30 s |
| Point-to-point | 10,000 + 10,000 | 20,000 msg/s | — | — | fail: 3,734/10,000 subscriptions in 30 s |

The 30-second limit is the scenario engine's subscription timeout, not a client failure: the
connections were made, but LavinMQ did not acknowledge every SUBSCRIBE in time. The practical
ceiling on this stand is between 3,000 and 4,000 concurrent sessions. Each session is an AMQP
queue and each subscription creates queue state, which is the likely cost.

## Cluster, four nodes

The cluster leader was `node-a`; runs through a follower go through the proxy.

| Scenario | Path | Clients | Offered | Median latency | Result |
|---|---|---|---:|---:|---|
| Point-to-point | through follower b | 1,000 + 1,000 | 2,000 msg/s | 382 µs | pass |
| Fan-out | through follower b | 5 + 100 | 25,000 msg/s | 849 µs | pass |
| Persistent sessions | through follower c | 1,000 + 1,000 | 1,000 msg/s | — | fail: 709/1,000 subscriptions in 30 s |
| Persistent sessions | through follower c | 100 + 100 | 100 msg/s | 316 µs | pass |

**Failover:** the leader was stopped. `node-b` became leader, and a publish on follower `node-d`
reached a subscriber on follower `node-c` (2 ms connect, message delivered). `node-a` was started
again and rejoined as a follower. The etcd members were `a`, `b`, `c`; replication uses port 5679.

## Files

`*.console` are the raw `xmq_scn` outputs. Scenarios are in `LoadTest/LavinMQ/`.
The cluster was installed with LavinMQ's `[clustering]` section pointing at the three etcd
endpoints, a 5679 advertised URI, and `etcd_prefix = lavinmq`. The systemd unit needed
`LimitNOFILE=1048576` on these hosts: the packaged `LimitNOFILE=infinity` makes LavinMQ's
`setrlimit` fail with `Operation not permitted` on this image.

## Not done

- No 500K-connection run: LavinMQ did not reach 4,000 sessions in the standard subscription
  timeout, so the connection test is not meaningful yet.
- No Fan-In: shared subscriptions are unsupported.
- No long runs and no repeated runs; these are single exploratory passes.
