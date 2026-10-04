# XMQ MQTT Load Testing Suite

This directory contains scripts and scenario files for use with xmq_scn MQTT Load Testing utility on Linux.

The main script is run_load_test.sh. If you run it without command line arguments, or with the wrong arguments, it will
print help explaining its command line options.

The run_load_test.sh expects the MQTT server running on the localhost or on a remote server. It can simulate multiple
(up to 1M) clients working with such server.

The scenario files can be customized to your requirements. The supplied scenario files are designed to use the approach
described by EMQ team in https://www.emqx.com/en/blog/open-mqtt-benchmarking-comparison-mqtt-brokers-in-2023.

## Recording broker versions with results

Results under `results/` are assembled by hand from `xmq_scn` output plus a few meta
lines. **Include a `Version:` line for every broker**: none of the broker install scripts
here pin a version, so a published latency figure cannot be reproduced later without it,
and a re-run months on may silently compare against a different build. `broker_versions.sh`
prints the lines ready to paste:

```
./broker_versions.sh                  # brokers on this host
./broker_versions.sh -H <host>        # over ssh, e.g. the server host
./broker_versions.sh -s EMQX          # one broker only
```

```
Server:   EMQX
Version:  5.8.9
```

The web pages pick the field up automatically and render it as a table column, so no code
change is needed when it is present — and results predating this convention simply show
`-`.

## Environment setup

High connection counts need kernel and per-user limits raised beyond their defaults, or
`xmq_scn`/`xmq_server` will hit `EMFILE`/port-exhaustion errors well short of 1M connections.
`setup/` has drop-in config files for both, on both client and server machines:

- `setup/sysctl.d/mqtt.conf` — file-descriptor, backlog, and TCP buffer/port tuning
  (`fs.nr_open`, `net.core.somaxconn`, `net.ipv4.tcp_mem`/`tcp_rmem`/`tcp_wmem`, conntrack).
- `setup/sysctl.d/port_range.conf` — widens the ephemeral port range
  (`net.ipv4.ip_local_port_range`).
- `setup/limits.d/mqtt.conf` — raises the per-process open-file limit (`nofile`) to match
  `fs.nr_open` above; without this, `ulimit -n` stays at the (typically 1024) default even
  after the sysctl change.
- `setup/nftables.d/xmq-notrack.nft` + `setup/systemd/xmq-notrack.service` — exempt the
  MQTT ports from `nf_conntrack`. **Required on any host running Docker or libvirt**, which
  otherwise cap a run at ~262K connections and stop the host accepting SSH.
- `setup/modules-load.d/nf_conntrack.conf` — loads `nf_conntrack` before `systemd-sysctl`,
  without which the `nf_conntrack_max` settings above fail silently at boot.

**Read `setup/README.md` before the first run on a new host.** It documents each limit, the
exact symptom when it is wrong, and a verification checklist — including two traps that
cost a day of debugging: a `ulimit -n` line in `~/.profile` that silently caps every run,
and `nf_conntrack` packet drops that look exactly like a broker fault.

Install once per machine (client and server both need this — see `setup/README.md` for the
full command list):

```
sudo cp setup/sysctl.d/*.conf /etc/sysctl.d/
sudo cp setup/limits.d/mqtt.conf /etc/security/limits.d/mqtt.conf
sudo install -D -m 0644 setup/modules-load.d/nf_conntrack.conf /etc/modules-load.d/nf_conntrack.conf
sudo install -D -m 0644 setup/nftables.d/xmq-notrack.nft /etc/nftables.d/xmq-notrack.nft
sudo cp setup/systemd/xmq-notrack.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now xmq-notrack.service
sudo modprobe nf_conntrack && sudo sysctl --system
# limits.d takes effect on next login/service start — log out and back in,
# or restart the service, before running a large test. Check ~/.profile too.
```

`run_load_test.sh --apply-sysctl` applies a subset of this at runtime for convenience, but
the drop-in files persist across reboots and are the recommended approach for a dedicated
load-test box. See `1M-Connection-Test-Plan.md` section 4 for the role-specific values
(client vs. server) these numbers are drawn from, and `results/` for measured
per-connection memory costs.

### Secondary bind IPs must not collide with other traffic

`make_ip_addresses.sh` statically assigns a range of local IPs (default `--subnet`/`--first`/
`--count`) for `xmq_scn` to bind clients across. What "don't collide" requires depends on
where the client box lives:

- **Shared LAN** (office/home network, as opposed to an isolated AWS VPC): the chosen range
  **must** be excluded from your router's DHCP pool first. Otherwise some addresses in the
  range can already be leased to other live devices (phones, laptops, etc.): the peer server
  resolves that IP to the *other* device's MAC instead of yours, so replies silently vanish
  instead of coming back to `xmq_scn`. This shows up as a confusing, intermittent subset of
  connections stalling or timing out with no obvious server-side cause — see incident
  2026-07-25, three conflicting IPs out of 29 caused ~10% of connects to hang.
- **AWS VPC** (see `AWS-Setup.md` and `1M-Connection-Test-Plan.md` section 3): there's no
  router or DHCP pool to exclude anything from — secondary private IPs are a static
  allocation you control within your own subnet. The equivalent failure mode there is
  different: AWS's hypervisor drops all traffic for a secondary IP that isn't explicitly
  registered on the instance's ENI first, no matter what `ip addr add` did locally (that
  registration step is what `AWS-Setup.md` walks through). The only DHCP-style collision risk
  on AWS is picking a range that's already statically assigned to a *different* instance in
  the same VPC — avoid that by choosing from a range you know is unused in your subnet.

`make_ip_addresses.sh` pings each candidate address before claiming it and skips (with a
warning) anything that's already answering, which catches the shared-LAN case above (and a
same-VPC collision on AWS) — but it's only a best-effort, point-in-time check: a mobile
device that's asleep at setup time and wakes mid-test won't be caught, and it does nothing
for the AWS ENI-registration requirement, which is a separate step regardless. Excluding the
range from DHCP (LAN) or registering it on the ENI (AWS) is the actual fix in each case; the
ping check just catches the LAN scenario earlier instead of surfacing as a mysterious
load-test failure.

## Contents

- `run_load_test.sh` — main entry point; runs `xmq_scn` against a scenario file.
- `make_ip_addresses.sh` / `remove_ip_addresses.sh` — add/remove the secondary bind IPs
  `run_load_test.sh` needs to spread client connections across source addresses.
- `init_environment_aws.sh` — example environment variables (server host/port, client subnet)
  for a given client VM; source it before running `run_load_test.sh` to avoid repeating flags.
- `setup/sysctl.d/`, `setup/limits.d/` — OS tuning drop-in files, see "Environment setup" above.
- `1M-Connection-Test-Plan.md` — full runbook for the 1M-connection scenario: topology,
  instance sizing, and multi-VM setup.
- `AWS-Setup.md` — per-instance AWS runbook for registering secondary IPs on the ENI so the
  hypervisor doesn't drop their traffic.
- `emqx/`, `mosquitto/`, `nanomq/` — install and config files for running the same scenarios
  against these brokers for comparison.
- `Scenario-Set.txt` — the common, ordered scenario set for all four brokers on both the home
  bench and AWS. `Scenario-Set-Bench.txt` and `Scenario-Set-AWS.txt` are links to this one file,
  so the two environments cannot silently select different tests.
- `results/` — `AWS/<version>/` and `Bench/<version>/`, one file per test; see
  `results/README.md`. Every version needs both before it is released.
- `file_results.py` — files a scenario-set record into `results/`.

## Repeatable comparison runs

Run every broker against the same `Scenario-Set.txt` and scenario JSON files. Record the broker
and `xmq_scn` versions; the exact client flags; the broker's effective listener, thread, queue,
authentication and persistence settings; the Redis settings used by the persistent scenario;
the host OS and kernel; CPU affinity; the client source address range; and NIC drops. Restart
the selected broker before each scenario, with the other brokers stopped. Start the persistent
scenario with empty storage and all other scenarios with persistence disabled. The run record
should contain each scenario's result and duration, plus the total set execution time.

Compare runs on the same host with matching settings. Use the home bench to detect regressions
before spending time on AWS; publish only AWS measurements. A failure or an incomplete scenario
is a result to investigate, not a latency value to include in a comparison.
