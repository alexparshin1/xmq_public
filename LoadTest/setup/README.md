# Load-test host setup

Host configuration required to run the `Connections` scenarios at high connection counts.
Everything here applies to **both** the server host and the client host — every limit
below is per-host, and each one has been hit on both ends. Fixing only the server is the
single most common mistake (see [conntrack](#1-nf_conntrack-etcnftablesd-etcmodules-loadd)).

## Test hosts

Hosts are referred to by role throughout this document. Substitute your own machines; the
hardware is listed only so the measured numbers below can be read in context.

| Name | Role | CPU | RAM | OS / kernel |
|---|---|---|---|---|
| `server1` | broker under test | Intel i7-6770HQ, 4 cores / 8 threads, 2.6 GHz (3.5 turbo) | 29 GB | Ubuntu 26.04, stock kernel, HZ=1000 |
| `server2` | broker under test | Intel i7-8705G, 4 cores / 8 threads, 3.1 GHz (4.1 turbo) | 31 GB | Debian testing, rebuilt kernel, HZ=1000 |
| `client1` | load generator | Intel Core Ultra 9 288V, 8 cores / 8 threads, up to 5.1 GHz | 30 GB | Debian testing, stock kernel, HZ=250 |

All three are on the same 1 Gb switched segment. The 1M-connection run on 2026-07-30 used
`server1` and `client1`; the tuning measurements in section 4 were taken on `server2`.

## Files in this directory

| Path here | Install to | Purpose |
|---|---|---|
| `sysctl.d/mqtt.conf` | `/etc/sysctl.d/mqtt.conf` | fd ceiling, socket buffers, conntrack max, TCP memory |
| `sysctl.d/port_range.conf` | `/etc/sysctl.d/port_range.conf` | widen the ephemeral port range |
| `limits.d/mqtt.conf` | `/etc/security/limits.d/mqtt.conf` | `nofile` for login sessions |
| `modules-load.d/nf_conntrack.conf` | `/etc/modules-load.d/nf_conntrack.conf` | load `nf_conntrack` before `systemd-sysctl` |
| `nftables.d/xmq-notrack.nft` | `/etc/nftables.d/xmq-notrack.nft` | exempt MQTT ports from connection tracking |
| `systemd/xmq-notrack.service` | `/etc/systemd/system/xmq-notrack.service` | apply the NOTRACK rules at boot |

Install:

```sh
sudo install -m 0644 sysctl.d/mqtt.conf          /etc/sysctl.d/mqtt.conf
sudo install -m 0644 sysctl.d/port_range.conf    /etc/sysctl.d/port_range.conf
sudo install -m 0644 limits.d/mqtt.conf          /etc/security/limits.d/mqtt.conf
sudo install -D -m 0644 modules-load.d/nf_conntrack.conf /etc/modules-load.d/nf_conntrack.conf
sudo install -D -m 0644 nftables.d/xmq-notrack.nft       /etc/nftables.d/xmq-notrack.nft
sudo install -m 0644 systemd/xmq-notrack.service /etc/systemd/system/xmq-notrack.service
sudo systemctl daemon-reload
sudo systemctl enable --now xmq-notrack.service
sudo modprobe nf_conntrack
sudo sysctl --system
```

Then **log out and back in** so the `nofile` change takes effect, and check `~/.profile`
(below). Verify everything with the [checklist](#verification-checklist).

---

## 1. nf_conntrack (`/etc/nftables.d/`, `/etc/modules-load.d/`)

**The single most destructive limit, and the least obvious.**

If Docker or libvirt is installed, the `nf_conntrack` module is loaded and **every** TCP
connection consumes a conntrack entry — even with no NAT or filtering on the MQTT path.
The kernel default `nf_conntrack_max` is derived from RAM and comes out at **262144** on a
30 GB host. Past that the kernel silently drops packets:

- `dmesg` fills with `nf_conntrack: table full, dropping packet`
- `xmq_scn` reports `Connection timeout in SocketVirtualMethods.cpp:169` and
  `Client ... failed: Server not available in ScenarioEngine.cpp:173`
- the run limps forward only as fast as entries age out
- **the host stops accepting SSH**, which is the giveaway that it is kernel-level and not
  an application fault

`nftables.d/xmq-notrack.nft` exempts ports 1880-1886 (the range used by XMQ, Mosquitto,
EMQX and NanoMQ) from tracking entirely. This is preferred over simply raising
`nf_conntrack_max`, because it also removes the per-packet conntrack lookup from the path
being benchmarked. Measured effect: conntrack stayed at **~20 entries** through a full 1M
run, versus filling a 262144-entry table at ~262K connections without it.

`modules-load.d/nf_conntrack.conf` fixes a boot-ordering bug: `systemd-sysctl` runs
*before* Docker loads `nf_conntrack`, so the `nf_conntrack_max` lines in `sysctl.d/mqtt.conf`
are written to a knob that does not exist yet, fail **silently**, and the module later comes
up with its own default. Symptom is `sysctl.d/mqtt.conf` specifying 1000000 while
`sysctl -n net.netfilter.nf_conntrack_max` reports 262144.

Use `nft` for these rules, not `iptables`. On an nftables-based distribution (Ubuntu 26.04 among
them) `iptables -t raw -A ...` fails with `table 'raw' is incompatible, use 'nft' tool` — and the
`-A` may fail while the surrounding script reports success, leaving the host tracking every packet
while the log says the rule was added.

The NOTRACK rules are **not** installed into `/etc/nftables.conf`, because that file begins
with `flush ruleset` and would wipe Docker's and libvirt's own rules at boot. The systemd
unit adds only our `raw` table, ordered `After=docker.service libvirtd.service`. The `.nft`
file uses the `table ip raw {}` / `delete table ip raw` / re-create idiom so it is
idempotent and safe to re-apply.

## 2. File descriptors (`/etc/security/limits.d/`, `~/.profile`)

One fd per connection, plus a small fixed overhead. `xmq_server` idles at **17 fds**, so a
1M-connection run needs 1000017 — a limit of exactly 1000000 is not enough.

Three separate mechanisms set this, and the **lowest wins**:

| Mechanism | Applies to | Value needed |
|---|---|---|
| `/etc/security/limits.conf`, `/etc/security/limits.d/` | PAM login sessions | 2000000 |
| systemd `DefaultLimitNOFILE` | systemd services | often 524288 — **enough for 500K, not 1M** |
| `ulimit -n` in `~/.profile` / `~/.bashrc` | that user's shells | see below |

### The `~/.profile` trap

Both hosts had this at `~/.profile:32`:

```sh
ulimit -n 500000        # WRONG - caps every run at 499983 connections
```

`ulimit -n` sets the soft **and hard** limit. Once the hard limit is lowered, nothing
unprivileged can raise it again — so a later `ulimit -n 2000000` in `~/.bashrc`, and
`run_load_test.sh`'s own attempt, both fail with
`cannot modify limit: Operation not permitted`. Everything else on the box was already
provisioned for 2097152; this one line was the binding constraint.

Change it to:

```sh
ulimit -n 2000000
```

Symptoms of getting this wrong:

- **client side:** `Can't create socket: Too many open files in SocketVirtualMethods.cpp:81`
  near the end of the ramp
- **server side:** the run stops at exactly **499983** (`500000 - 17`) and reports
  `N connection(s) appear stalled (no response from server)` — the server cannot open more

Two things to know:

- The change only takes effect in a **new login session**. The old hard limit is inherited
  by every process in the existing tree, so an already-running `xmq_server` keeps the old
  value — restart it from a fresh login before a run above ~499K.
- To run before re-login, set the limit explicitly:

  ```sh
  sudo systemd-run --uid=$(id -u) --gid=$(id -g) \
    --property=LimitNOFILE=2000000 \
    --working-directory="$PWD" --unit=xmq-run \
    --setenv=XMQ_SERVER_HOST="$XMQ_SERVER_HOST" --setenv=XMQ_SERVER_PORT="$XMQ_SERVER_PORT" \
    --setenv=XMQ_CLIENT_SUBNET="$XMQ_CLIENT_SUBNET" --setenv=XMQ_CLIENT_FIRST=150 \
    --setenv=XMQ_CLIENT_COUNT=29 --setenv=XMQ_CLIENT_PREFIX=vm1- \
    --setenv=PATH=/usr/local/bin:/usr/bin:/bin \
    /bin/bash ./run_load_test.sh <scenario>.json -p 1884 --skip-ip-setup
  ```

## 3. sysctl (`/etc/sysctl.d/`)

See `sysctl.d/mqtt.conf` for the annotated values. The ones that matter most:

- **`fs.nr_open` / `fs.file-max` = 2097152** — the system-wide ceiling the `nofile` limits
  above must fit under.
- **`net.netfilter.nf_conntrack_max` = 1000000** — belt-and-braces alongside NOTRACK.
  Requires the `modules-load.d` fix above to actually apply.
- **`net.core.somaxconn` = 65535** — caps the effective `listen()` backlog. Note the kernel
  applies `min(backlog, somaxconn)`, so this only helps if the application asks for more.
- **`net.ipv4.tcp_mem` = 1048576 1572864 2097152** (4/6/8 GB, in 4 KB pages) — a global
  ceiling on TCP buffer memory. **Do not quote these values**: `sysctl` does not strip
  quotes, so a quoted setting fails to apply. That is why the line was previously dead.

  Honest note on sizing: this was raised expecting 1M connections to need ~2 GB of
  buffers. Measured usage never exceeded **1437 pages (5.6 MB)** on either host, because
  idle connections hold almost no buffer memory. Useful headroom for message-passing
  scenarios, but it is *not* what limits a connection-count test.

`port_range.conf` widens the ephemeral range to `1024 65535`, matching what
`run_load_test.sh --apply-sysctl` sets. Client-side port capacity is
`(number of bound source IPs) x 64512`; with the 30 interfaces
`make_ip_addresses.sh` creates that is 1935360 tuples, ample for 1M.

## 4. Kernel boot parameters and kernel flavour

Neither of these is a sysctl: both are fixed at boot, and both were found by profiling rather
than by suspicion. Together they moved this workload more than every application-level change
measured so far.

### `iommu.passthrough=1` — record it, or the comparison is meaningless

A profile of the server under Fan-Out showed **~8% of its CPU inside the IOMMU**:
`clflush_cache_range` at 4.28%, plus `__map_range_leaf`, `__map_range` and `__iova_to_phys1` —
the NIC mapping DMA buffers through the IOMMU on every packet. Booting with DMA passthrough
removes those symbols from the profile entirely.

```sh
sudo sed -i 's/^GRUB_CMDLINE_LINUX_DEFAULT="\(.*\)"/GRUB_CMDLINE_LINUX_DEFAULT="\1 iommu.passthrough=1"/' /etc/default/grub
sudo update-grub && sudo reboot
```

**Verify the state, not the flag.** `/etc/default/grub`, `grub.cfg` and `/proc/cmdline` only prove
that a string is present; none of them says what the IOMMU is actually doing, and the flag is not
the only thing that decides it — firmware settings turn the unit off entirely, and a VM may have no
IOMMU to begin with. The one answer that matters is the domain type:

```sh
cat /sys/class/iommu/*/devices/*/iommu_group/type | sort | uniq -c   # want all "identity"
```

`identity` means addresses pass through untranslated. Anything else (`DMA`, `DMA-FQ`) means the
per-packet mapping cost is still being paid. Note `dmesg` keeps printing `DMAR:` lines either way —
the unit is still present and initialised, so its presence in the log proves nothing.

An empty `/sys/class/iommu/` is its own answer: there is no IOMMU here, the flag is inert, and
these 8% never existed on this host.

**On AWS, only that last check means anything.** An EC2 guest usually has no IOMMU of its own —
the host does the isolation — so the boot flag can be present and change nothing at all, and
reading `/etc/default/grub` or `/proc/cmdline` there tells you precisely nothing about what the
DMA path costs. Check `/sys/class/iommu/` and nothing else. It also follows that a bare-metal
host and an EC2 guest are **not comparable** on this axis: one of them can pay a per-packet
mapping cost that the other cannot even express.

Measured on `server2`, Fan-Out 250K, CPU compared at the same offered rate — CPU is
the right instrument here, since latency drifts about two-fold over a few hours while CPU holds
to ±1%:

| | CPU before | CPU after | latency before | latency after |
|---|---|---|---|---|
| XMQ | 273.4% | 254.2% (−7.0%) | 2 568 µs | 2 363 µs (−8.0%) |
| FlashMQ | 311.5% | 374.2% (**+20%**) | 22 880 µs | **2 796 µs (−88%)** |

**The two brokers are not equally sensitive, and that is the point.** FlashMQ had been hitting a
wall on DMA-mapping cost and queueing behind it; freed of it, it does more work (CPU up 20%) and
its latency collapses eightfold. So this single boot flag moved the standing from *XMQ 9x faster*
to *XMQ 18% faster*.

Treat the IOMMU mode as a **condition of the comparison**, not a tuning tip: record it for every
cross-broker run and make it identical on every host in the campaign. It has no equivalent on
Windows and may be unavailable inside containers, so it is a property of the test bed rather than
of any broker.

#### That 8% is a small-scenario figure — the real cost scales with the connection count

Fan-Out 250K holds only ~1005 connections, and there the IOMMU costs XMQ the 7-8% above. The cost is
paid per DMA mapping, so it grows with packet rate and with the number of sockets the NIC scatters
packets across. Measured on `server1` with Point-To-Point 50K — 50 000 publishers against 50 000
subscribers, so **100 000 connections** and 50K msg/s in each direction — one factor removed at a
time, same binary throughout:

| `server1` configuration | latency |
|---|---|
| as the distribution leaves it: IOMMU translating, `nf_conntrack` loaded by Docker | 485 / 519 / 546 **ms** |
| NOTRACK for the broker port (section 1) | 60.6 / 24.8 **ms** |
| **and `intel_iommu=off`** | 0.9 / 1.4 / 1.6 / 6.7 / 22.1 ms, median **1.6 ms** |
| `server2`, which had both from the start | **0.386 ms** |

Three orders of magnitude between the first row and the last, on one machine and one binary. Neither
factor is visible at Fan-Out scale, and both are defaults: a host with Docker installed and no boot
flags gets **both**. So for a production host expecting tens of thousands of connections this is not
a tuning tip at all — in translation mode the broker simply does not keep up, and the queue grows
without bound.

Two practical notes. `iommu.passthrough=1` and `intel_iommu=off` cost the same and either will do;
prefer **passthrough** on a machine that is not a dedicated bench, since it keeps device isolation
enabled while skipping the per-packet mapping. And note the remaining 4x between the last two rows
is *not* IOMMU — those hosts differ in clock speed and kernel — but the run-to-run spread on the
repaired host (0.9 to 22 ms) is far wider than on the host that never had the problem, which is a
reason to prefer a bench that was never in translation mode.

### Kernel tick: `CONFIG_HZ=1000`

Distributions disagree here, and it matters more than anything else measured: Ubuntu builds its
generic kernel with **HZ=1000**, Debian ships **HZ=250**. On identical hardware and an identical
binary, the Debian stock kernel cost XMQ **65%** (22 775 µs against 13 773) while costing FlashMQ
only 8.7% — we are roughly seven times more sensitive to it. With a rebuilt HZ=1000 kernel the same
machine went to 6 140 µs, i.e. 3.7x better, and from *losing* to `server1` to beating it 2.2x.

HZ is a compile-time setting; no boot parameter changes it. Check it before trusting any number
from a new host:

```sh
grep '^CONFIG_HZ=' /boot/config-$(uname -r)     # want 1000
```

On Debian, rebuild the stock kernel with the tick changed — `linux-source-7.1`, the running
config, then `scripts/config --disable HZ_250 --enable HZ_1000 --set-val HZ 1000`, and disable
`DEBUG_INFO_BTF` / enable `DEBUG_INFO_NONE` (Debian force-selects debug info through BTF, which
otherwise eats the disk and hours of build time). Two traps: `libdw-dev` is an undeclared build
dependency, and **Secure Boot will refuse the unsigned result** ("bad shim loader signature") —
either turn Secure Boot off or sign with a MOK, and prefer `grub-reboot` for a one-shot boot so a
failure falls back automatically instead of needing hands at the console.

## 5. Client-side secondary IP addresses

Not a file — but the client needs multiple source IPs, since a single IP allows only
~64512 concurrent connections to one destination. `make_ip_addresses.sh` adds 29
secondary addresses (`${XMQ_CLIENT_SUBNET}.150`-`.178`), giving 30 source IPs with the primary.

These are transient `ip addr add` entries and **do not survive** a reboot or a
NetworkManager reconfiguration. They were found silently missing between two runs. Re-add
with:

```sh
./make_ip_addresses.sh -s "$XMQ_CLIENT_SUBNET" -p 24 -f 150 -c 29
ip -4 -o addr show | grep -c "${XMQ_CLIENT_SUBNET}\.1[5-7][0-9]"   # expect 29
```

`run_load_test.sh` does this itself unless `--skip-ip-setup` is passed.

## 6. Broker thread counts (`server_limits`)

Not a host setting, but it belongs with the rest because the wrong value here is worth more than
everything above. `send_threads` and `receive_threads` each default to **3**, in `Settings.cpp` and
in `xmq_server.conf.template`. Three is not a guess — it is the point where both curves have already
flattened, on every machine measured:

| | 1 | 2 | 3 | 4 | 6 | 8 |
|---|---|---|---|---|---|---|
| `send_threads`, 16 vCPU (AWS), Fan-Out 250K | 2 849 | **1 746** | 1 810 | 1 947 | 2 319 | 2 725 µs |
| `receive_threads`, 8-core host, Fan-In 30K | 2 264 ms, **loses messages** | 485 | — | 464 | — | 461 µs |
| `receive_threads`, 4-core host, Point-To-Point 50K | 540 | 375 | — | 386 | — | 374 µs |

Read three things from this:

- **One thread is the only setting that fails outright.** A single receive thread cannot absorb
  30K msg/s: latency goes to milliseconds and the server drops messages (17 434 of 9 000 000 in one
  run). One send thread is merely slow.
- **Past two or three the curves are flat, but CPU keeps climbing** — 226% → 242% → 254% across
  `receive_threads` 2 → 4 → 8 for identical latency. Extra threads buy nothing and cost cores.
- **More is actively worse on a bigger machine.** On 16 vCPU, 8/8 is the *worst* measured point,
  56% behind 2/2, and on the pre-2026-08 build it could not sustain the offered rate at all. Do not
  scale these numbers with the core count.

A four-core bench cannot resolve the difference between 1 and 4 send threads at all — the spread
between repeated runs of one setting exceeds the spread between settings — so the optimum of 2 comes
from AWS, and 3 is chosen as the value that is within noise of it while staying clear of 1.

## 7. Receive packet steering on a single-queue NIC (`/sys/class/net/*/queues/`)

For users this is published in the XMQ user manual, the only official documentation: <https://xmq.sptk.net/xmq_documentation#receive-steering>. What follows is the bench's own record.

A network card with **one receive queue** hands every incoming packet to one CPU, and the kernel then
runs the entire IP and TCP receive path there. Under enough load that CPU sits at 100% softirq while
the rest idle, the card's ring overflows, and the dropped frames come back as TCP retransmissions in
exponential backoff. A session stuck in backoff for 1.5 x its keep-alive looks silent to the broker,
which closes it - and the client reports `Not connected`, although every session was carrying traffic.

That is exactly how the 100k msg/s Point-To-Point run failed on thinker11 (e1000e, one queue) on
2026-09-13, after two client-side fixes aimed at the wrong cause. Measured during publishing:

| | before | with RPS on CPUs 4-6 |
|---|---|---|
| frames dropped per second (`rx_no_buffer_count`) | 255 891 | 0 |
| softirq on the interrupt's CPU | 100% | 41% |
| broker keep-alive timeouts per run | 9-12 | 0 |
| scenario result | `Not connected` | completed, 100 000 msg/s |

A bigger ring (`ethtool -G ... rx 4096`) changed nothing: the ring was not too small, it was drained
too slowly.

### Do you need it? Three signs, all at once

1. **One receive queue:** `ls /sys/class/net/<if>/queues/` shows only `rx-0`, and
   `grep <if> /proc/interrupts` shows one line. Several queues means the card already spreads
   interrupts (RSS) and RPS is not needed.
2. **One CPU pinned in softirq, under load:** `mpstat -P ALL 1` shows `%soft` near 100 on one CPU while
   others idle. Sample *during* the load - a sample taken after the run shows nothing.
3. **Drops growing over the run:** the increase, not the absolute value, of
   `ethtool -S <if> | grep -E 'no_buffer|missed|fifo|drop'`, together with `nstat -az TcpRetransSegs`.

With only the first sign, leave it off: moving packets between CPUs costs a little at low rates.
`xmq_server` checks the first sign at start-up and logs a recommendation when the interface is
single-queue and unsteered on a machine with more than two CPUs; the other two need load to see.

### Enabling it

```sh
IF=enp0s31f6
echo 70    | sudo tee /sys/class/net/$IF/queues/rx-0/rps_cpus        # hex mask: CPUs 4, 5, 6
echo 32768 | sudo tee /proc/sys/net/core/rps_sock_flow_entries        # RFS: steer to the reading CPU
echo 32768 | sudo tee /sys/class/net/$IF/queues/rx-0/rps_flow_cnt
```

Choosing the mask: CPUs that are idle under load, **not** the CPU the card's interrupt lands on (it is
already busy draining the ring), preferably not the ones the broker's threads are pinned to, and on the
card's NUMA node. On thinker11 CPU7 takes the interrupt and CPUs 4-6 are the idle SMT siblings of the
cores the broker runs on, hence `70`.

None of this survives a reboot. To keep it:

```sh
# /etc/sysctl.d/60-rfs.conf
net.core.rps_sock_flow_entries = 32768

# /etc/udev/rules/60-rps.rules
ACTION=="add", SUBSYSTEM=="net", KERNEL=="enp0s31f6", ATTR{queues/rx-0/rps_cpus}="70", ATTR{queues/rx-0/rps_flow_cnt}="32768"
```

Undo by writing `0` to all three.

## Verification checklist

Run on **both** hosts. Note `sysctl` lives at `/usr/sbin/sysctl` and is **not** on the
non-root PATH on these boxes — a bare `sysctl` returns empty, which reads misleadingly
like "not configured".

```sh
/usr/sbin/sysctl -n net.netfilter.nf_conntrack_max     # 1000000, not 262144
/usr/sbin/sysctl -n net.ipv4.tcp_mem                   # 1048576 1572864 2097152
/usr/sbin/sysctl -n fs.nr_open                         # 2097152
/usr/sbin/sysctl -n net.core.somaxconn                 # 32768
sudo nft list table ip raw | grep -c notrack           # 4
systemctl is-enabled xmq-notrack                       # enabled
bash -lc 'ulimit -Hn'                                  # 2000000
grep -n 'ulimit -n' ~/.profile ~/.bashrc               # no value below 2000000
cat /sys/class/iommu/*/devices/*/iommu_group/type \
  | sort | uniq -c                                     # all "identity", and the same on every host
grep '^CONFIG_HZ=' /boot/config-$(uname -r)            # 1000, not 250
cat /sys/class/net/<if>/queues/rx-0/rps_cpus           # non-zero on a single-queue NIC; see section 7
ls /sys/class/net/<if>/queues/ | grep -c '^rx-'        # 1 means single-queue
```

And on the server, against the *running* process — this is the one that actually matters,
since it may predate the config change:

```sh
PID=$(pgrep -x xmq_server | head -1)
grep 'open files' /proc/$PID/limits                    # 2000000, not 500000
```

## Known-good reference numbers

From `../results/1M-connections-local.txt` and `../results/500K-connections-local.txt`:

| | 500K | 1M |
|---|---|---|
| xmq_server peak RSS | 2.17 GB | 4.32 GB |
| per connection (server) | 4.51 KB | 4.51 KB |
| xmq_scn peak RSS | 1.46 GB | 2.91 GB |
| conntrack entries in use | ~20 | ~20 |
| `tcp_mem` pages in use | — | 0-245 |
| `TcpExtListenOverflows` | 0 | 0 |

Server-side memory scales linearly at 4.51 KB/connection with no inflection, so RSS is not
the constraint at these counts.

### RSS is retained after disconnect — this is not a leak

RSS does not fall when clients disconnect: the server sat at 4.32 GB with zero connections
after the 1M run. This is glibc allocator retention, not a leak, and it was measured
directly — a 500K run against that same unrestarted process moved `VmRSS` and `VmHWM` by
**zero bytes** (4535392 kB before, 4535392 kB at 494604 connections), versus the linear
325 MB → 2116 MB growth the same scenario shows on a freshly started server. Every byte of
those 500K sessions came from free lists the 1M run left behind.

Cause: glibc gives each thread its own arena (up to `8 x nproc`) and grows each in 64 MB
`mmap` chunks — `/proc/<pid>/smaps` shows ~59 mappings of exactly 65536 kB, with the main
`[heap]` at only 708 kB. Freed per-session chunks return to their arena's free list, and
glibc only releases pages when the *top* of an arena heap is contiguously free, which
fragmentation across 59 arenas prevents.

Implications:

- **For benchmarking:** restart the server between runs, so a run's memory figures reflect
  that run rather than a warm pool. Do not try to make the server release memory — the next
  run would only fault it back in.
- **For production under a container memory limit:** an idle broker holding peak RSS has no
  headroom for the next spike. `malloc_trim(0)` on an idle trigger (e.g. connection count
  dropping well below peak) is the cheap fix; it walks every arena and costs nothing while
  busy. Avoid `MALLOC_ARENA_MAX` as a first move — fewer arenas means more lock contention
  across the server's ~72 threads.
