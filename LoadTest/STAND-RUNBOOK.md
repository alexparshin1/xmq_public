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
`stand_run.sh` stops it when it finds it active - through `systemctl`, because the unit's
`Restart=always` would bring a killed process back - and notes that in the record.

## The broker must be configured

A broker that has no configuration listens on 1883 and refuses every client with
`Authentication failed`, so the set dies on its first scenario. Take a node from reset or from a
half-finished attempt to a finished setup in one command:

```bash
./initial_setup thinker11
```

It stops the broker, checks that 1883, 8883 and the web port are free, moves the local account
file aside, starts `xmq_server --reset-configuration` detached, signs in as the passwordless
`admin`, submits `/InitialSetupControl` with the values in `~/xmq-stand.env` on thinker11, and
then verifies the result: the echo matches the request, the new administrator password works,
`setup_required` is false, the three ports listen, and the log names the Redis server and the
user database. Exit status 0 means every one of those passed.

The values are never on the command line: the request body is built by `jq` from the environment
and written to a mode-600 temporary file, so no password appears in `ps`.

`--dry-run` prints the request with the passwords masked and changes nothing. `--no-reset`
submits against a broker already sitting in the pre-setup state. There is no separate reset
script: `initial_setup --reset-only` stops after the reset, and that node has no accounts, no
Redis and no user database, so every login is refused until Initial Setup runs.

## The bench profile

`~/bench-<something>/` on thinker11 is the whole of what the run measures, and it is not in version
control: create it there. It holds three things, and a profile missing any of them is a set that
fails forty minutes later rather than at the start.

**1. `xmq_server.conf`.** Copy the node's configuration, e.g.
`cp ~/.local/etc/xmq/xmq_server.conf ~/bench-1.1.2/`. On a node whose accounts live in PostgreSQL
the broker keeps most settings in the database and rewrites this file to the file-only keys on
startup, so a short file is not a damaged one; `xmq_server.conf.lastgood` beside it is the version
that was there before that rewrite.

**2. `restart.sh`.** Run before every scenario, over ssh, so it must start the broker and return
rather than hold the channel. Killing without starting is the failure to avoid: the port wait then
reports `thinker11:1883 never accepted` for every scenario and nothing is measured.

```bash
#!/bin/bash
# Standalone bench profile for XMQ 1.1.2: restart the broker before a scenario.
set -u
profile_dir="$HOME/bench-1.1.2"
bin="$HOME/.local/bin/xmq_server"
conf="$profile_dir/xmq_server.conf"

pkill -9 -x xmq_server 2>/dev/null || true
for _ in $(seq 1 20); do
    pgrep -x xmq_server >/dev/null 2>&1 || break
    sleep 0.5
done
pgrep -x xmq_server >/dev/null 2>&1 && exit 1

cd "$profile_dir" || exit 1
: > "$profile_dir/xmq_server.out"
( setsid nohup "$bin" -c "$conf" >>"$profile_dir/xmq_server.out" 2>&1 </dev/null & )
sleep 2
pgrep -x xmq_server >/dev/null 2>&1 || {
    tail -5 "$profile_dir/xmq_server.out" >&2; exit 1; }
exit 0
```

The `setsid nohup ... </dev/null` in a subshell is what lets the ssh command return; a plain
background job keeps the channel open and `stand_run.sh` hangs at the restart.

**3. `xmq_extensions.d`.** The broker reads `xmq_extensions.conf` and `xmq_extensions.d` *beside
its own configuration file*, not from `~/.local/etc/xmq`. The profile has its own configuration, so
without this it has no authenticator: with `allow_anonymous` false the broker listens, the port wait
passes, every client is refused, and the log says
`No extension authenticates and anonymous connections are not allowed`. Point the profile at the
installed fragments rather than copying them:

```bash
ln -sfn ~/.local/etc/xmq/xmq_extensions.d ~/bench-1.1.2/xmq_extensions.d
```

`stand_run.sh` refuses a profile that has none of this while anonymous connections are off.

## The one command

From `~/workspace/xmq_public/LoadTest` on thinker10:

```bash
./stand_run.sh --version 1.1.2
```

`--version` is the XMQ version the run measures, and it is the one input that changes between
runs: the bench profile on thinker11 is taken to be `bench-<version>` (here `bench-1.1.2`), it
goes into the record header, and it names both the log directory under `results/raw/` and the
results directory `--file` files under. Pass `--bench-dir` only when the profile is not named
after the version. The broker account defaults to `user` with password `Secret#123`; both are
overridden with `--user` and `--password`. Everything else - restart command, wait for 1883,
CPU/RSS sampling, client NIC, source subnet and address range, record path - is fixed in the
script.

Add `--file` to file the record into `LoadTest/results/Bench/<version>/` after a successful run.
Filing replaces a version's directory, so compare against the previous version's numbers first.

Check the plan without starting anything:

```bash
./stand_run.sh --version 1.1.2 --dry-run
```

Prove the wiring in about two minutes, before committing forty:

```bash
./stand_run.sh --version 1.1.2 --set Scenario-Set-Smoke.txt
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

## Running a set from an agent session

A forty-minute set cannot be held inside one tool call, and this host runs the model that would
be holding it. `agent_run.sh` starts the same `stand_run.sh` detached and returns at once:

```bash
./agent_run.sh --version 1.1.2 --file
```

It prints the log, a `.status` file that receives the exit code and duration when the set ends,
and the record path it expects. Everything else is passed to `stand_run.sh` unchanged, so the
record is still written by the script that owns it; this only decides when it runs.

The point is the turn ends there. `stand_run.sh` is not modified, and nothing stops an agent from
generating tokens during a set - that is what the run would then measure, so the log and the
record should be read afterwards, not watched.

## Writing a run, then watching it

`xmq_runner` is Linux, and that is a decision rather than a gap: it drives bash scripts that
restart a broker over ssh, and a second implementation of "what a run is" would be a second answer
to the question the record depends on. A Windows port is not part of this version; a Windows
machine reaches the stand through WSL or ssh.

A bench run needs a version, a set, a host and a handful of switches, and getting one of them wrong
is only visible after forty minutes. `xmq_runner --edit` is a form over exactly those settings: it
writes a run file, and nothing else.

```bash
cmake .                                  # from the repository root
cmake --build . --target xmq_runner
cmake --install . --component runner
xmq_runner --edit                        # create a run file
xmq_runner --edit xmq-1.1.2.run.sh
```

The default home install puts the binary in `~/.local/bin` and its scenario sets,
JSON scenarios and helper scripts in `~/.local/etc/xmq_runner`.
Run files live in `~/.local/var/lib/xmq/xmq_runner/` for the default home install, or
`/var/lib/xmq/xmq_runner/` for a system install. Raw logs and filed benchmarks
live in its `results/raw/` and `results/Bench/` subdirectories. On a system install,
the administrator must make the state directory writable by the account running benchmarks.
Existing results and run files are not migrated automatically.

Enter edits a value, Enter again keeps it, arrows move, F2 writes the file, Esc leaves without
writing. A value that cannot work - a version that is empty, a set that is not there - is refused
with the reason, and the form stays open.

The file it writes is a shell script, not a private format: the invocation `stand_run.sh` would get
from a person, one flag per line. It runs as it stands, and it can be edited by hand afterwards:

```bash
../runs/2026-11-18-1729-xmq1.1.2.run.sh           # runs stand_run.sh with those flags
../runs/2026-11-18-1729-xmq1.1.2.run.sh --dry-run # the same, for one that only prints
```

While it runs, a second window watches it - and, given the run file, also starts it, stops it and
files the result, so a set can be run from one window and watched from another:

```bash
./xmq_runner --view --run ../runs/2026-11-18-1729-xmq1.1.2.run.sh
```

The screen takes `<run-dir>` and the broker from the file's own `--out` and `--server`, so nothing
has to be typed twice and nothing can disagree with the file the run is executed from. Watching a
bare directory - `--view ../results/raw/<run>` - is read-only and contacts nothing outside this
machine.

Keys: `s` start, `x` stop, `f` file the result, `q` quit. Each of the three asks for a `y` first,
because none of them can be undone from the screen - a set is forty minutes, an interrupted set is
not filed, and filing replaces that version's whole directory under `results/Bench/`.

A start is detached in its own session, with its output in `<run-dir>/run.log`, so it outlives the
screen and a stop reaches `xmq_scn` and the scripts around it together, not just the first process.
The run's pid is kept in `<run-dir>/.stand.pid`: a second screen, or the same one reopened, attaches
to the run already going instead of starting another.

## Watching a set while it runs

`xmq_runner --view` reads what the run itself writes into its output directory and shows it live.
It is a watcher: it starts nothing, and it is not on the run's path.

```bash
xmq_runner --view <run-dir>           # full screen
xmq_runner --view <run-dir> --once    # one status, for a script or a log
```

The table shows one test at a time - a full set is five scenarios of several minutes each, and all
of them at once stops being readable by the second. It follows the test that is running; `left` and
`right` choose one by hand, and `up`, `down`, `PageUp`, `PageDown`, `Home` and `End` walk through
that test's intervals, with the row at the top saying how many are above what is on screen.

The rows come from the `INTERVAL` lines `xmq_scn` writes under `--output runner` (the default for a
set): rate, average and p99 latency, and the client process's CPU and memory. A run made before that
mode existed has none of those lines and shows as `not started`, so watch a set from 1.1.2 or later.

The broker's own CPU and memory come from its `$SYS` topics, read through `xmq_sub` on the broker
host named in the run file: it needs `xmq_sub` in `PATH` and a broker that publishes those topics
(1.1.2 or later). A broker line that reads `xmq_sub exited` means the subscriber died - the broker
restarted under it, or the binary is missing - and the figures stop rather than going stale.

## What it refuses to do

- **It does not invent a profile.** The profile name is derived from the version, `bench-<version>`,
  but the directory itself must exist and hold `restart.sh`, `xmq_server.conf` and an extension
  config; if any of those is missing on thinker11 the script stops before starting anything. Build
  and install the version on thinker11 first (`cmake . && make -j8 install` in `~/workspace/xmq`),
  then create or copy the bench directory.
- **It does not run two sets at once.** `xmq_scn` already running means exit 1.
- **It does not leave another broker on 1883, and does not hide that it stopped one.** A broker on
  thinker11 that is not this profile - the systemd unit, or one left from `~/.local/bin` - would own
  1883, and the record would name a version that was never running. It is stopped before the run,
  through `systemctl` when the unit owns it (`Restart=always` brings a killed process back), and the
  record gets a note saying so. `--keep-other-brokers` turns that into a refusal instead, naming the
  process.
- **It does not file a failed set.** Non-zero exit leaves the logs in `results/raw/` and prints
  the tail of the record.
- **It does not call `run_scenario_set.sh` directly.** Do not reimplement the invocation: the
  record's version, Host line and restart command all come from the flags above, and a hand-built
  call has produced records that named the wrong version.

## When it fails

| Symptom | Meaning | What to do |
|---|---|---|
| `no executable ~/bench-*/restart.sh` | profile not built or wrong name | `ls -d ~/bench-*` on thinker11, pass the right one |
| `... has no authenticator` | profile has no `xmq_extensions.d` beside its config | `ln -sfn ~/.local/etc/xmq/xmq_extensions.d ~/bench-*/` |
| `<host>:1883 never accepted` on every scenario | `restart.sh` killed the broker without starting it | see "The bench profile" above |
| `xmq_scn is already running` | another set is in progress | wait or `pgrep -af xmq_scn` |
| `another broker is running ... would answer on 1883` | `--keep-other-brokers` in use, non-bench broker owns the port | drop the flag to have it stopped, or stop it yourself |
| `did not carry a pub/sub round-trip` | broker came up but not serving | read `~/bench-*/xmq_server.log` on thinker11 |
| `WARNING: N address(es) ... need 29` | source addresses not bound | `./make_ip_addresses.sh --iface enp86s0 --subnet 10.1.1 --first 150 --count 29` |
| scenario prints no `median` line | run aborted mid-way | look at `<scenario>.log` in the run directory |
| `file_results.py refused the record` | XMQ record has a failed scenario; a campaign missing one is not filed | fix the failure and rerun the set; the record stays in `results/raw/` |

Preflight is reported, not enforced, and its output is graded:

| Level | Meaning |
|---|---|
| `FAIL` | a required limit is wrong; fix the host before trusting the numbers |
| `warn` | suspicious but not decisive |
| `NOTE` | a state this stand is known to run in; the run proceeds |

Two checks on this stand print `NOTE` rather than `FAIL`: the client's
`ports per source address` (55536, below the 60000 the set assumes) and the server's
`NOTRACK rules` when the rules were cleared. Both are expected here — with 29 bound source
addresses the port count is still ~1.6M connections of capacity, far more than the largest
scenario needs — so record them with `--note` and do not reconfigure the host mid-campaign.

## Publishing

Per-version directories are `LoadTest/results/Bench/<version>/`, one per version, replaced whole
on re-test. Home-stand numbers and AWS numbers are different instruments and are never mixed in
one series.
