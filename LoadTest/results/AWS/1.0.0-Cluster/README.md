# XMQ 1.0.0, four-node AWS cluster

The campaign behind the cluster and survivability pages of xmq.io, and the diagnostics that
explain the two numbers that are not obvious from the tables.

**Only these AWS runs are published.** The same tests were also run on the home stand
(`thinker10`); those runs are not part of this record and do not appear on the site, because the
stand does not exist for anyone outside the project.

## What ran

- Four brokers on four AWS `c5n.2xlarge` instances (8 vCPU, 21 GiB each), one placement group,
  `us-east-1a`; one broker installed at a time, systemd service, no CPU affinity.
- The shared Redis (8.0.5, db 4) and the PostgreSQL accounts database are separate instances and
  are not counted as broker resources.
- One `c5n.4xlarge` client, 29 bound source addresses, `xmq_scn` 1.0.0, no CPU affinity.
- MQTT 5, port 1884, QoS 1, 16-byte payload, cluster links on 8884.
- The cluster is restarted and the shared store flushed before every scenario.

XMQ numbers are from the farm build of `4919dd6`:

    xmq-server_1.0.0_amd64.deb
    sha256 7f4635570c613ef1ef40d8dea19bea73508a055e6943250a51d1a93cd52ea4c0

The comparison is EMQX 5.8.9, Apache 2.0 — the last Apache-licensed release that clusters:

    emqx-5.8.9-ubuntu24.04-amd64.deb
    sha256 2ebb47cb14118af0345e1af518509279506ab3ea9e57a3a0f879f0d443a8f9c7

## Layout

    xmq/              the four published XMQ runs: console, node samples, formatted record
    emqx/             the same four runs on EMQX 5.8.9
    ramp/             XMQ-only connection-rate ladder (10K/20K/40K/80K per second)
    chaos/            the survivability run: 6 crashes over 600 s, one node down 20 s per cycle
    p2p-diagnostics/  the P2P latency investigation and the first P2P run (not published)

`metadata.json` has the stand, packages, method and per-scenario medians.

## The P2P run was repeated

The first point-to-point run (10:36) reported a 20 ms median, flat for the whole ten minutes,
while its fan-out run at a higher rate reported 1.2 ms. That did not look like a cluster, so it was
investigated and repeated before anything was published:

- a 120 s run with `XMQ_LATENCY_TRACE=1`: **513 µs**, with the per-hop breakdown in
  `p2p-diagnostics/persistence-on.console` — no phase anywhere near 20 ms;
- the same 600 s command again, no trace: **489 µs** over 60 million messages.

The published P2P record is the repeat. The first run is kept as
`p2p-diagnostics/p2p-first-run-20ms.console`: it did not reproduce, and the campaign does not know
what made it slow, so it is recorded as an unexplained outlier rather than a result. `metadata.json`
says so next to the P2P entry.

The persistence-off A/B that was planned to test the 20 ms guess was not run: the scenario uses
`clean_session=true`, so no delivery record is written at all, and the trace run shows no store
cost in the path. The write-behind delay could not have produced that number.

## The connection-rate ceiling

The comparison paces 500K connections at 5,000/s because that is a rate both brokers sustain.
XMQ's own ladder holds 10,000/s with sub-millisecond connect latency, reaches the knee at
20,000/s, and settles near 21,000/s above that while all four brokers stay under one core each.

The connection path makes about seven round trips to the shared Redis per connection (3.5 million
commands for 500,000 clients, from `INFO commandstats`), and a round trip on this stand is about
0.1 ms. With four listener threads per node that is roughly 4,500 connections/s per node, about
18,000 for the cluster — which is where the ladder lands. Redis itself stays near half a core: the
cost is the round trips, not its CPU.
