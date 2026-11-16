# Working notes for agents in this repository

## Home load-test stand

The bench set runs with one command, from `LoadTest` on thinker10:

```bash
./stand_run.sh --version <XMQ version> --bench-dir bench-<name>   # full set, ~40 min
./stand_run.sh ... --set Scenario-Set-Smoke.txt --stop-other-brokers   # ~2 min wiring check
```

Read `LoadTest/STAND-RUNBOOK.md` before running anything on the stand. Do not call
`LoadTest/run_scenario_set.sh` or `run_load_test.sh` directly: host, ports, restart command,
source addresses, record path and version string all belong to `stand_run.sh`, and a hand-built
invocation has produced records that named a version that never ran. Do not invent flags for
`stand_run.sh`; its `--help` is the list.
