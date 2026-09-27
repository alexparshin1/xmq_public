# 1M Connection Load Test — Plan

Baseline reference: EMQX's [2023 MQTT broker benchmark](https://www.emqx.com/en/blog/open-mqtt-benchmarking-comparison-mqtt-brokers-in-2023)
(broker: c5.4xlarge 16C32G, 1M connections, 5,000 conn/s, QoS 1, 16B payload, 300s keep-alive,
clean session, 30-minute run). This plan reproduces those parameters against `xmq_server`,
using two `xmq_scn` load-generator machines (500K connections each) instead of XMeter.

All of the CLI options referenced below (`--id-prefix`, `--keep-alive`, `--bind-to-interfaces`,
`--connection-rate`) exist in this branch as of this test-planning session.

## 1. Topology

- **Server:** 1× broker host running `xmq_server`.
- **Client 1 ("vm1"):** `xmq_scn`, 500,000 connections, `--id-prefix vm1-`.
- **Client 2 ("vm2"):** `xmq_scn`, 500,000 connections, `--id-prefix vm2-`.
- Same AZ, same VPC, to keep network latency out of the measurement.

## 2. Instance sizing

| Role | Instance | vCPU | RAM | Why |
|---|---|---|---|---|
| Client ×2 | c5n.4xlarge | 16 | 42 GiB | c5n.xlarge (10.5 GiB) is too tight once kernel socket buffers for 500K connections are counted; resized up from c5n.2xlarge (15 IPs/ENI cap) to c5n.4xlarge, which gives ENIs with 30 IPs each — enough for 29 secondary bind IPs plus the primary. |
| Server (pilot) | c5.4xlarge | 16 | 32 GiB | Matches EMQX's broker instance exactly — needed to make the comparison meaningful. Our code-level estimate puts XMQ's footprint at 1M connections at ~14.5–26.5 GB raw, ~19–40 GB with margin, so 32 GiB is plausible but has little headroom. **Do not commit to this size for the full run until the pilot (step 6) confirms it.** |
| Server (fallback) | c5n.9xlarge | 36 | 96 GiB | Use this instead if the pilot's measured bytes/connection extrapolate above ~28–30 GB for 1M, or if you want a safety margin from the start and don't need a strict apples-to-apples hardware comparison. |

## 3. Network setup (per client machine)

Bind the outbound MQTT clients to multiple local IPs so no single source IP hits the ~64K
ephemeral-port ceiling. Secondary IPs on one ENI work identically to separate physical
interfaces for this purpose — `xmq_scn` just calls `bind()` on the local IP.

On AWS, these secondary IPs must also be registered on the instance's ENI before the OS-level
`ip addr add` below will actually pass traffic (AWS's hypervisor drops unregistered source
IPs) — see `AWS-Setup.md` for the full per-instance runbook.

**vm1** (confirmed working): subnet `<vpc-subnet-cidr>`, interface `ens5`, instance resized to
`c5n.4xlarge` for the larger per-ENI IP allowance.
```
# Adds 29 secondary IPs (up to 30 total per ENI, 1 is the primary) to the interface
./make_ip_addresses.sh -s "$XMQ_CLIENT_SUBNET" -p 24 -f 150 -c 29
```

**vm2**: not provisioned yet. Until it exists, reuse vm1's `${XMQ_CLIENT_SUBNET}.150`–`.178` range as a
placeholder below — replace with vm2's own subnet/range (distinct from vm1's, to avoid
collisions) once that instance and its AWS-side IP registration exist.

500,000 connections ÷ 29 IPs ≈ 17,240 conns/IP — comfortably below the recommended 30–50K/IP
band, which gives extra headroom against the ephemeral-port-search slowdown seen when an IP's
local port table gets too full (see incident 2026-07-23: a client run appeared healthy up to
~450K connections, then dropped 10-100x in throughput — leading theory is TIME_WAIT sockets
from a prior run occupying a large fraction of the then-14 IPs' port ranges, not yet confirmed
by direct measurement). Adjust `-s`/`-f`/`-c`
to your actual VPC CIDR; pass `-i <iface>` if autodetection picks the wrong interface. Tear
down with the matching `remove_ip_addresses.sh` between test iterations.

## 4. OS tuning

### Client machines (×2)

```
sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535"
sudo sysctl -w net.ipv4.tcp_tw_reuse=1
sudo sysctl -w net.ipv4.tcp_max_tw_buckets=2000000
sudo sysctl -w fs.nr_open=2000000
ulimit -n 700000   # or set LimitNOFILE in a systemd unit / /etc/security/limits.conf
```

Why: normal `xmq_scn` teardown always closes gracefully (FIN, not RST — see
`client/MqttClient.cpp:149`), so all ~500K connections per client box land in TIME_WAIT at
once when the test ends. `tcp_tw_reuse` + a raised `tcp_max_tw_buckets` let you rerun the test
soon after without waiting out the ~60s TIME_WAIT window or exhausting ports.

### Server

```
sudo sysctl -w net.core.somaxconn=65535
sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535
sudo sysctl -w fs.nr_open=2000000
ulimit -n 1200000
# Only if conntrack is loaded (check: lsmod | grep nf_conntrack):
sudo sysctl -w net.netfilter.nf_conntrack_max=2000000
```

Why: `xmq_server`'s `listen()` backlog is hardcoded to `SOMAXCONN`
(`SocketVirtualMethods.cpp:210`), which the kernel silently caps at `net.core.somaxconn` —
xmq has no config knob for this, so the sysctl is the only lever. There is also no
max-connections safety cap anywhere in the server (`allowConnection()` hook exists but is
never overridden), so it will not gracefully refuse connections under resource pressure —
watch memory instead of expecting backpressure.

## 5. Scenario files

`xmq_scn` has no CLI override for connection *count* — it's JSON-only. The same file can be
reused for both machines: `--id-prefix` is *prepended* to the JSON file's `id_prefix` (not a
replacement), specifically so one scenario file works unmodified across hosts — pass
`--id-prefix vm1-` / `--id-prefix vm2-` on the command line and each machine's client IDs come
out as `vm1-client-N` / `vm2-client-N`.

**Full run** — `Connections-500k.json`:
```json
{
  "name": "1M Connection Test - 500K per host",
  "type": "Connections",
  "publishers": {
    "id_prefix": "client-",
    "protocol_version": 5,
    "client_count": 500000,
    "qos": 1,
    "clean_session": true
  },
  "server": {
    "hostname": "<server-ip>",
    "port": 1883,
    "username": "user",
    "password": "secret"
  },
  "parameters": {
    "duration_sec": 1800,
    "payload_size": 16,
    "connection_rate": 2500,
    "keep_alive_sec": 300
  }
}
```

**Pilot run** — `Connections-50k-pilot.json`: same file with `"client_count": 50000` and
`"duration_sec": 300`.

## 6. Pilot before the full run

Run the pilot (100K total, 50K/machine) on the **exact instance types you intend for the full
run**, especially the server, before committing to 1M. This validates three unknowns at once
that no amount of code-reading settles definitively:

1. **Real server RSS growth per connection** — extrapolate to 1M and compare against the
   32 GiB c5.4xlarge estimate. If it doesn't fit, move to c5n.9xlarge before the full run.
2. **Real achieved connection rate** — the server funnels every connection through one shared
   epoll reactor thread that drains only 32 events per wake (`server/ServerData.cpp:26`) and
   one global mutex for session-map inserts (`Server.cpp:484-487`). At 2,500 conn/s/machine
   this shouldn't matter, but the pilot is the cheap way to confirm the requested rate is what
   you actually get, rather than finding out at connection 800,000.
3. **No cross-machine ID collisions** — confirm server logs show no unexpected session
   take-overs/kicks between vm1 and vm2's client IDs.

```bash
# Machine 1 (pilot)
./xmq_scn --scenario Connections-50k-pilot.json \
  --host "$XMQ_SERVER_HOST" \
  --bind-to-interfaces "${XMQ_CLIENT_SUBNET}.150/24" \
  --id-prefix vm1- \
  --connection-rate 2500 --qos 1 --payload-size 16 --keep-alive 300 --duration 300

# Machine 2 (pilot)
./xmq_scn --scenario Connections-50k-pilot.json \
  --host "$XMQ_SERVER_HOST" \
  --bind-to-interfaces "${XMQ_CLIENT_SUBNET}.150/24" \
  --id-prefix vm2- \
  --connection-rate 2500 --qos 1 --payload-size 16 --keep-alive 300 --duration 300
```

While it runs, watch on the server: `ps -o rss -p $(pgrep xmq_server)` (or `/proc/<pid>/status`
`VmRSS`) sampled every ~10s, plus `ss -s` for TCP state counts.

## 7. Full run

```bash
# Machine 1
./xmq_scn --scenario Connections-500k.json \
  --host "$XMQ_SERVER_HOST" \
  --bind-to-interfaces "${XMQ_CLIENT_SUBNET}.150/24" \
  --id-prefix vm1- \
  --connection-rate 2500 --qos 1 --payload-size 16 --keep-alive 300 --duration 1800

# Machine 2
./xmq_scn --scenario Connections-500k.json \
  --host "$XMQ_SERVER_HOST" \
  --bind-to-interfaces "${XMQ_CLIENT_SUBNET}.150/24" \
  --id-prefix vm2- \
  --connection-rate 2500 --qos 1 --payload-size 16 --keep-alive 300 --duration 1800
```

Timeline per machine: ramp 500,000 ÷ 2,500/s = 200s (~3.3 min), then hold for the remainder of
the 1800s duration (~26.7 min), matching EMQX's 30-minute window.

## 8. Known risks to watch, not yet fixed in code

- **Single shared epoll reactor + 32-events-per-wake on the server** — first place to look if
  connection setup throughput stalls below the requested rate.
- **No max-connections cap** — the server will not refuse connections under memory pressure;
  an OOM is possible instead of a clean rejection if sizing is wrong. This is why the pilot's
  RSS measurement matters more than the desk estimate.
- **c5.4xlarge (32 GiB) for the server is a real OOM risk**, not a comfortable fit — it's
  included here only because it matches EMQX's published hardware for a fair comparison.
