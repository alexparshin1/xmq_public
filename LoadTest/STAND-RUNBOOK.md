# Home stand: one bench run

Internal runbook for running the bench set on the home stand. Not product documentation and not
mirrored to the website: it names our hosts, our accounts and our paths.

## The stand

- **thinker10** - client and load generator. Runs everything below. 29 source addresses
  `10.1.1.150`-`178` on `enp86s0`.
- **thinker11** - broker under test, started from a per-version bench directory
  `~/bench-<something>/` holding `xmq_server.conf` and `restart.sh`.
- **theater** - cluster tests only, not part of this set.

The systemd `xmq_server` unit must not be running on thinker11: the bench broker owns port 1883.

## The one command

From `~/workspace/xmq_public/LoadTest` on thinker10:

```bash
./stand_run.sh --version 1.1.2 --bench-dir bench-1.1.2
```

`--version` is the XMQ version the run measures; `--bench-dir` is the profile on thinker11 that
holds `restart.sh`. Those two are the only inputs that change between runs. Everything else -
restart command, wait for 1883, CPU/RSS sampling, client NIC, source subnet and address range,
record path, log directory - is fixed in the script.

Add `--file` to file the record into `LoadTest/results/Bench/<version>/` after a successful run.
Filing replaces a version's directory, so compare against the previous version's numbers first.

Check the plan without starting anything:

```bash
./stand_run.sh --version 1.1.2 --bench-dir bench-1.1.2 --dry-run
```

Prove the wiring in about two minutes, before committing forty:

```bash
./stand_run.sh --version 1.1.2 --bench-dir bench-1.1.2 --set Scenario-Set-Smoke.txt --stop-other-brokers
```

That set is one short scenario and exists only to show that restart, readiness wait, stats
sampling and record writing all work against this profile. Its numbers are not a measurement:
never file or publish them.

The full set takes about 40 minutes. Scenario by scenario it prints `restarting the broker`,
`running`, then `median Nus over M samples`. The end looks like:

```
== result (40m10s) ==
Summary - interval medians, as xmq_scn reports them:

  Scenario                              Median   Samples
  ──────────────────────────────────────────────────────
  Fan-Out-5-1000-5-250K-5min             2256us        11
  ...
  record: .../results/raw/2026-11-16-1800-xmq1.1.2-bench/record.txt
```

## What it refuses to do

- **It does not invent a profile.** If `~/bench-<dir>/restart.sh` is missing on thinker11 the
  script stops before starting anything. Build and install the version on thinker11 first
  (`cmake . && make -j8 install` in `~/workspace/xmq`), then create or copy the bench directory.
- **It does not run two sets at once.** `xmq_scn` already running means exit 1.
- **It does not silently stop another broker.** A broker on thinker11 that is not this profile -
  the systemd unit, or one left from `~/.local/bin` - would own 1883 and the record would name a
  version that was never running. The run refuses and names the process; `--stop-other-brokers`
  stops it, and the record gets a note saying so.
- **It does not file a failed set.** Non-zero exit leaves the logs in `results/raw/` and prints
  the tail of the record.
- **It does not call `run_scenario_set.sh` directly.** Do not reimplement the invocation: the
  record's version, Host line and restart command all come from the flags above, and a hand-built
  call has produced records that named the wrong version.

## When it fails

| Symptom | Meaning | What to do |
|---|---|---|
| `no executable ~/bench-*/restart.sh` | profile not built or wrong name | `ls -d ~/bench-*` on thinker11, pass the right one |
| `xmq_scn is already running` | another set is in progress | wait or `pgrep -af xmq_scn` |
| `another broker is running on ... would answer on 1883` | non-bench broker owns the port | rerun with `--stop-other-brokers` |
| `did not carry a pub/sub round-trip` | broker came up but not serving | read `~/bench-*/xmq_server.log` on thinker11 |
| `WARNING: N address(es) ... need 29` | source addresses not bound | `./make_ip_addresses.sh --iface enp86s0 --subnet 10.1.1 --first 150 --count 29` |
| scenario prints no `median` line | run aborted mid-way | look at `<scenario>.log` in the run directory |
| `file_results.py refused the record` | record has a failed scenario | do not force it; rerun the set |

Preflight failures are reported, not enforced: the client always fails
`ports per source address` for the 500K scenario on this hardware, and the server fails
`NOTRACK rules` when the rules were cleared. Both are known states, worth noting in the record
with `--note`, not reasons to refuse the run.

## Publishing

Per-version directories are `LoadTest/results/Bench/<version>/`, one per version, replaced whole
on re-test. Home-stand numbers and AWS numbers are different instruments and are never mixed in
one series.
