# Load-test results

```
results/
  AWS/<version>/<test>.txt     the AWS set, one file per test
  Bench/<version>/<test>.txt   the home bench set (client-host client, server-host broker)
  raw/                         per-run logs; not kept in git
```

`<version>` is the XMQ version (`0.9.19`), or `<Broker>-<version>` for another broker
(`FlashMQ-1.27.1`). `<test>` is the scenario name. Each file holds the run's header - broker,
version, host, client, date, notes - and that test's table, or the line saying it failed.

**One directory per version.** Re-testing a version replaces its directory whole; do not accumulate
parallel runs of one version. A directory that would mix two campaigns is worse than a missing one:
a scenario the new run did not have would keep its old table, and nothing downstream can tell.

**AWS results are required when a release changes the major or minor version, or when it moves the
measured path.** A patch release that leaves the path alone is published without them; `release.sh`
on the build farm decides by version number and takes `--hotpath` for a patch that does move it.
The bench results are our own baseline on our own hardware, never part of a release.

A change is checked for regressions against the previous version's `Bench/<version>/` files.

## Filing a run

```
./run_scenario_set.sh Scenario-Set-Bench.txt ... --record results/raw/<run>/record.txt --version 0.9.20
./file_results.py --env Bench results/raw/<run>/record.txt
```

`file_results.py` refuses a version whose directory already holds results unless given `--force`,
which removes that directory before writing the new run.
