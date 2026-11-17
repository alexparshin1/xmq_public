# Working notes for agents in this repository

## XMQ Initial Setup on a host

The procedure is `doc/InitialSetup.md`. The deployment values it needs - node
name and host, ports, Redis, database URI, and the `admin` and `cluster`
passwords - are not in the document: they are in `~/xmq-stand.env` on thinker10
and thinker11 (mode 600, not in version control). Load that file, do not invent
a password, and do not substitute defaults from the examples:

```bash
set -a; . "$HOME/xmq-stand.env"; set +a
```

On a shared accounts database the passwords must be the ones already stored
there; a chosen one is refused with `The admin password does not match the one
in the shared database`.

## Home load-test stand

The bench set runs with one command, from `LoadTest` on thinker10:

```bash
./stand_run.sh --version <XMQ version>            # full set, ~40 min; profile bench-<version>
./stand_run.sh ... --set Scenario-Set-Smoke.txt   # ~2 min wiring check
```

Read `LoadTest/STAND-RUNBOOK.md` before running anything on the stand. Do not call
`LoadTest/run_scenario_set.sh` or `run_load_test.sh` directly: host, ports, restart command,
source addresses, record path and version string all belong to `stand_run.sh`, and a hand-built
invocation has produced records that named a version that never ran. Do not invent flags for
`stand_run.sh`; its `--help` is the list.
