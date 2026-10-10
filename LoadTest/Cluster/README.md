# Testing a cluster

An ordinary load scenario runs against a cluster while a timeline takes nodes away and brings them
back, and checks after the load ask the cluster what state it is in.

## Where this lives

The stand, the tool and the example tests are on branch `2026-10-10-Cluster-test-system`, which carries
the cluster work as it stood when the branch was made; nothing here is in `0.9.20` yet. Two SPTK fixes
go with it, on branch `5.6.14`: a connect timeout taken from the URL by the PostgreSQL connector and by
`RedisConnect` - a check that points at something which cannot answer otherwise waits out the system's
own timeout instead of one it named itself.

## The pieces

| Piece | Where | What it is |
|---|---|---|
| stand | `LoadTest/Cluster/stand.sh` | builds a cluster of plain processes on one or several machines: nodes, certificates, one shared storage, one shared accounts database |
| tests | `LoadTest/Cluster/*.json` | a cluster test: nodes, the load to run, a timeline, checks |
| runner | `utilities/ClusterTestRunner.*`, `ClusterTestDefinition.*`, `ClusterTestCommandLine.*` | runs the load in a thread, walks the timeline, runs the checks, prints PASS/FAIL |
| executable | `xmq_scn_cluster` | the runner's entry point, installed beside `xmq_scn` |
| load | an ordinary scenario file | the same engine `xmq_scn` uses, so a scenario written for the bench runs as it is |

The runner does not change the scenario format: groups in the test file select the scenario's own
groups by name and may point them at a named node of the cluster.

## The stand

Nodes are processes, not services, so a test can stop one in the middle of a run. Each node keeps its
configuration, certificate and log in its own directory under `$STAND_DIR` (default `~/cluster`), and
is started through a launcher of its own, so an interrupted run does not take the nodes with it.

```sh
stand.sh up            # write configurations and certificates, start every node
stand.sh status        # nodes, ports, and the cluster keys in the shared storage
stand.sh stop node3    # stop one node
stand.sh start node3
stand.sh down          # stop every node
stand.sh wipe          # remove the directories; the next up starts fresh
```

Two things are shared and must read identically on every node: `persistence.redis_uri` (the cluster
compares the string, so proxies and different spellings of the same storage are out) and the accounts
database. Nodes are chosen by name and host:

```sh
NODES="node1@thinker10 node2@thinker10 node3@thinker11 node4@thinker11" stand.sh up
```

## The test file

```json
{
    "name": "the second node comes back",
    "comments": "short, for whoever runs it",
    "server": { "host": "...", "port": 2884, "username": "user", "password": "..." },
    "load": {
        "scenario": "cluster-p2p-small.json",
        "groups": [ { "name": "publishers", "node": "node3" },
                    { "name": "subscribers", "node": "node6", "servers": ["node1", "node3"] } ]
    },
    "nodes": [ { "name": "node3", "host": "thinker11", "port": 2884,
                 "stop": "~/cluster/stand.sh stop {node}",
                 "start": "~/cluster/stand.sh start {node}" } ],
    "timeline": [ { "at": 30, "action": "stop", "node": "node3" },
                  { "at": 60, "action": "start", "node": "node3" },
                  { "at": 100, "action": "check", "check": "delivery after the restart" } ],
    "checks": [ { "name": "delivery after the restart", "kind": "mqtt/deliver",
                  "publish-node": "node3", "subscribe-node": "node6",
                  "topic": "cluster/test/after-restart", "timeout": 15, "settle": 3 } ]
}
```

`{node}` in a command is replaced by the node's name. `at` is seconds from the start of the load.
A step that fails fails the test: a `stop` that did not stop anything used to leave every check
passing, which looked like a healthy cluster.

## The checks

| Kind | What it asks |
|---|---|
| `mqtt/connect` | a client connects to a named node, or is refused, as expected |
| `mqtt/deliver` | publish on one node, subscribe on another, expect delivery |
| `mqtt/retained` | the same with the retained flag |
| `redis` | a command against the shared storage, with an expected answer |

Checks speak MQTT 5 unless `-V` says otherwise, and each uses a client id of its own, so no check takes
over a session of the load. A check may also carry:

- `settle`: seconds between the subscribe and the publish, for what a cluster has to advertise first;
- `verify-command` with `verify-contains`: a command run between the two, whose output must contain a
  text - the way to ask a node itself whether it holds a subscription.

```sh
xmq_scn_cluster --scenario LoadTest/Cluster/the-second-node-comes-back.json
xmq_scn_cluster --scenario ... --dry-run     # print the plan, run nothing
```

## The method

1. A publish immediately after a SUBACK measures how fast the cluster tells the publishing node about
   the subscription, not how it routes: give the check a `settle`.
2. The bench has an environmental drift of a few percent between runs, so results are compared A/B/A:
   a change counts when the before-binary reproduces it.
3. A test that passes while the cluster was never touched is worse than a failing one, which is why a
   failed timeline step fails the run.
4. `XMQ_ClusterTests` in the unit test suite is the cluster's own coverage. The night build runs the
   suite with `--gtest_filter=-*Scenario*`, so changes to the scenario engine are covered by the
   scenario tests beside it, run by hand.

## Confirming a change

After the unit tests, on the default stand (six nodes, two on each of thinker10, thinker11 and theater):

```sh
# lay the build down on every machine, from the machine it was built on - one per distribution:
# thinker10 is Debian, thinker11 and theater are Ubuntu and take the same binary
~/cluster/stand.sh deploy ~/workspace/xmq_public/cmake-build-debug/xmq_server thinker10    # on thinker10
ssh thinker11 ~/cluster/stand.sh deploy ~/workspace/xmq_public/xmq_server thinker11 theater
~/cluster/stand.sh up                     # refuses machines with servers of different commits
xmq_scn_cluster --suite LoadTest/Cluster  # every test here; exit code 0 only if all passed
xmq_scn_cluster --suite LoadTest/Cluster --repeat 3   # for what shows in some runs only
```

`deploy` copies the server, the SPTK it was built against (into `~/sptk/lib`) and the stand's own
scripts, and records the commit the server was built from in `xmq_server.source`; `up` and `status`
show it per machine. `up` also writes the stand's settings to `$STAND_DIR/stand.env` on every
machine, so `stand.sh stop node3` means the same stand wherever it runs, and starts every node's logs
afresh, so that `check-logs` finds this run's crash and not yesterday's.

A test's `before` and `after` commands run around it - `after` whatever happened, which is where a
test undoes what it did to the network. The tests here end with `stand.sh check-logs`: a crash in a
node's log, or a node that is not running, fails the test; going cluster-offline is reported.

The timeline has two more steps: `run` (a shell command; the network cut in
`the-storage-goes-away-from-one-machine.json`), and a `check` with `every` and `until`, which runs the
check again and again over a span and passes only if every run did - what a cluster does in the middle
of something is a span.

## Fixed

A node that lost the shared storage under load went cluster-offline late, kept its clients, and came
back long after the storage did. Its coordinator's step was waiting on Redis - in a `recv()` with no
timeout (SPTK's synchronous `RedisConnect`), and in a `connect()` left to the system - and going
offline and online was reported from that step. Fixed by a read timeout in SPTK, timeouts of one step
on the coordinator's connection, and a watch thread that reports both from the lease's deadline.
`the-storage-goes-away-from-one-machine.json` covers it on the stand; `nodeWhoseStorageStopsAnsweringGoesOfflineAndReturns`
in the unit tests.
