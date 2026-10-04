# Load-test results

```
results/
  AWS/<version>/<test>.txt     the AWS set, one file per test
  Bench/<version>/<test>.txt   the home bench set (thinker10 client, thinker11 broker)
  raw/                         per-run logs; not kept in git
```

`<version>` is the XMQ version (`0.9.19`), or `<Broker>-<version>` for another broker
(`FlashMQ-1.27.1`). `<test>` is the scenario name. Each file holds the run's header - broker,
version, host, client, date, notes - and that test's table, or the line saying it failed.

**Every XMQ version has both an AWS and a Bench set before it is released.** `release.sh` on the
build farm refuses a version without them.

A change is checked for regressions against the previous version's `Bench/<version>/` files.
A later run of the same version replaces its files.

## Filing a run

```
./run_scenario_set.sh Scenario-Set-Bench.txt ... --record results/raw/<run>/record.txt --version 0.9.20
./file_results.py --env Bench results/raw/<run>/record.txt
```

`file_results.py` refuses to replace files already filed unless given `--force`.
