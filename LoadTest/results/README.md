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

**One directory per version.** A successful re-test replaces that version's directory whole -
automatically, without a flag. Before filing, compare the run against the previous version and
check for regression. If there is a noticeable regression, fix it and run the campaign again;
do not file the regressed run over the good results. Do not accumulate parallel runs of one
version: a directory that mixed two campaigns would keep an old table for a scenario the new run
did not have, and nothing downstream could tell. A campaign with a failed scenario also does not
replace good numbers; fix what failed and run the set again.

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

`file_results.py` replaces a version's directory when the record is successful. A record containing
a failed scenario is refused while results are there; `--force` files it anyway, for a failure
worth keeping.
