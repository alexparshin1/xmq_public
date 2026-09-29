# XMQ

A fast, free MQTT server for Linux and Windows. MQTT 3.1, 3.1.1 and 5.0, QoS 0/1/2,
TLS, message persistence, bridging, and a million concurrent connections on one node.

- Website and documentation: <https://xmq.sptk.net>
- Downloads: <https://xmq.sptk.net/downloads>
- Benchmarks: <https://xmq.sptk.net/xmq_tests_environment>

## Try it

```bash
docker run -d --name xmq -p 1883:1883 -p 18883:18883 -v xmq-config:/etc/xmq alexeyparshin/xmq
docker exec -it xmq xmq_server --set-password admin
docker restart xmq
```

MQTT is on `localhost:1883` at once. The configuration interface,
`https://localhost:18883`, opens once `admin` has a password - set once, as above; the
volume keeps it.

Or install the package for your distribution from the
[downloads page](https://xmq.sptk.net/downloads) — `.deb` and `.rpm` are built for
Debian, Ubuntu, Fedora and Oracle Linux.

## Performance

The numbers below are median end-to-end latency from runs on AWS `c5n.4xlarge`
(16 vCPU), MQTT 5, QoS 1, published in full — with methodology, raw output and the
scenario files — on the [test pages](https://xmq.sptk.net/xmq_tests_environment).
The load generator is part of the release, so the runs can be reproduced.

**Fan-in** — 50 000 publishers, 500 shared subscribers, 50k msg/s for 30 minutes:

| Broker | Median latency | CPU (mean) | Peak RAM |
|---|---:|---:|---:|
| FlashMQ | 200 µs | 201 % | 242 MB |
| **XMQ** | **217 µs** | **267 %** | **242 MB** |
| Mosquitto | 236 ms | 100 % | 31.4 GB |
| EMQX | 103 s | 1323 % | 9.78 GB |

**Fan-out** — 250 000 messages/s to subscribers:

| Broker | Median latency | CPU (mean) | Peak RAM |
|---|---:|---:|---:|
| FlashMQ | 2.13 ms | 384 % | 29 MB |
| **XMQ** | **2.48 ms** | **329 %** | **22 MB** |
| EMQX | 4.15 ms | 870 % | 430 MB |
| Mosquitto | 87 s | 74 % | 2.52 GB |

**500 000 concurrent connections:**

| Broker | Median latency | Peak RAM |
|---|---:|---:|
| Mosquitto | 305 µs | 0.43 GB |
| **XMQ** | **348 µs** | **2.1 GB** |
| EMQX | 687 µs | 6.2 GB |

What the numbers say, plainly: XMQ holds sub-millisecond latency in the regimes where
Mosquitto (single-threaded) and EMQX fall apart by three to five orders of magnitude,
and it does so in a few hundred megabytes. FlashMQ is the closest competitor and is
slightly ahead in several scenarios. On plain connection load Mosquitto is lighter on
memory. Point-to-point at 50K clients: FlashMQ 198 µs, XMQ 277 µs, EMQX 52 ms.

## What it does

- **MQTT 3.1, 3.1.1 and 5.0** with QoS 0, 1 and 2, retained messages, wildcards,
  shared subscriptions and topic aliases
- **TLS** on a separate listener, with your own certificates
- **Persistence** through Redis — sessions, queued and retained messages survive a
  restart; off by default, so an unconfigured server keeps everything in memory
- **Bridging** between XMQ nodes and other MQTT servers
- **Web interface** for configuration and monitoring, on its own port
- **Accounts** with per-user access, or anonymous access when you ask for it
- Runs on Linux (`.deb`, `.rpm`) and Windows; a BSD port is in progress
- Clustering is being built and is not in this release

## Configuration

One JSON file, `/etc/xmq/xmq_server.conf`, created on first start. Listeners,
threads, queue limits, persistence, bridges and logging live there; accounts are in
`xmq_users.conf` beside it. Everything is also editable through the web interface on
port 18883.

The [user manual](https://xmq.sptk.net/xmq_documentation) covers the settings, and
[configuration](https://xmq.sptk.net/xmq_configuration) walks through the interface.

## Testing your own broker

The release includes the load generator used for the benchmarks above — `xmq_scn`
runs scenario files, `xmq_pub` and `xmq_sub` do single-purpose load. They speak
standard MQTT, so they work against any broker, not just XMQ. Scenarios mirror the
Open MQTT Benchmark Suite cases, which is what makes the comparisons above
meaningful. See [the test suite page](https://xmq.sptk.net/xmq_mqtt_test_suite).

## Source and licence

The XMQ broker is open source under the [Mozilla Public License
2.0](https://mozilla.org/MPL/2.0/), and is also distributed as binary packages,
free to use.

The MPL is a file-level copyleft: changes to XMQ's own source files stay open,
while a work that merely combines them with other files — the licence calls it a
Larger Work — may be released under other terms, including proprietary ones.
That is deliberate. Extensions that use XMQ's published extension API and ship as
separate binaries are not derivative works of the broker, and may be licensed
however their author chooses.

Files under `service/` are generated during the build by `wsdl2cxx` from
`xmq.wsdl` and carry no banner of their own; they are covered by the `LICENSE`
file in this directory.

XMQ is built on [SPTK](https://github.com/alexparshin1/sptk5), the cross-platform
C++20 library developed alongside it, which is open source under the LGPL — the
networking, threading and database layers XMQ runs on are all there to read.

## Changes

[CHANGELOG.md](CHANGELOG.md) records what changed in each release, and what an upgrade asks of you.

## Issues

Bug reports and questions are welcome in this repository's issue tracker, or by
email to <alexeyp@gmail.com>.
