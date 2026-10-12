# Testing a cluster

An ordinary load scenario runs against a cluster while a timeline takes nodes away and brings them
back, and checks after the load ask the cluster what state it is in.

## Where this lives

The test cluster, the tool and the example tests are on branch `2026-10-10-Cluster-test-system`, which carries
the cluster work as it stood when the branch was made; nothing here is in `0.9.20` yet. Two SPTK fixes
go with it, on branch `5.6.14`: a connect timeout taken from the URL by the PostgreSQL connector and by
`RedisConnect` - a check that points at something which cannot answer otherwise waits out the system's
own timeout instead of one it named itself.

## The pieces

| Piece | Where | What it is |
|---|---|---|
| test cluster | `LoadTest/Cluster/test_cluster.sh` | builds a cluster of plain processes on one or several machines: nodes, certificates, one shared storage, one shared accounts database |
| tests | `LoadTest/Cluster/*.json` | a cluster test: nodes, the load to run, a timeline, checks |
| runner | `utilities/ClusterTestRunner.*`, `ClusterTestDefinition.*`, `ClusterTestCommandLine.*` | runs the load in a thread, walks the timeline, runs the checks, prints PASS/FAIL |
| executable | `xmq_scn_cluster` | the runner's entry point, installed beside `xmq_scn` |
| load | an ordinary scenario file | the same engine `xmq_scn` uses, so a scenario written for the bench runs as it is |

The runner does not change the scenario format: groups in the test file select the scenario's own
groups by name and may point them at a named node of the cluster.

## The test cluster

Nodes are processes, not services, so a test can stop one in the middle of a run. Each node keeps its
configuration, certificate and log in its own directory under `$CLUSTER_DIR` (default `~/cluster.run`; `~/cluster` links to this directory of the test cluster worktree), and
is started through a launcher of its own, so an interrupted run does not take the nodes with it.

`test_cluster.sh` is bash, and the Windows package leaves it out. On Windows it runs under WSL
(`wsl --install`), which is the same environment as on Linux, or under Git Bash (Git for Windows,
with its `bin` on `PATH`), which is lighter but has no `pgrep`. Either way, `bash test_cluster.sh ...`.
`xmq_scn_cluster` itself and the tests need neither.

```sh
test_cluster.sh up            # write configurations and certificates, start every node
test_cluster.sh status        # nodes, ports, and the cluster keys in the shared storage
test_cluster.sh stop node3    # stop one node
test_cluster.sh start node3
test_cluster.sh down          # stop every node
test_cluster.sh wipe          # remove the directories; the next up starts fresh
```

Two things are shared and must read identically on every node: `persistence.redis_uri` (the cluster
compares the string, so proxies and different spellings of the same storage are out) and the accounts
database. Nodes are chosen by name and host:

```sh
NODES="node1@thinker10 node2@thinker10 node3@thinker11 node4@thinker11" test_cluster.sh up
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
                 "stop": "~/cluster/test_cluster.sh stop {node}",
                 "start": "~/cluster/test_cluster.sh start {node}" } ],
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

## A load balancer in front

`test_cluster.sh lb up` puts one load balancer in front of the whole test cluster: IPVS in the
kernel, run by keepalived, on the machine the script runs on, at `LB_VIP:LB_PORT` (this machine's
address, port 1883). It writes `/etc/keepalived/keepalived.conf` from the node list - one real server
per node, least-connections, NAT, so the nodes need nothing set up - and keepalived checks each node's
port and takes a node that does not answer out of the balancing until it answers again. `lb status`
shows the balancing (`ipvsadm -Ln --stats`), `lb down` stops it. It needs `keepalived` and `ipvsadm`
installed and sudo for them. Clients of the machine reach the cluster through `LB_VIP:LB_PORT`, by
address: the machine's own name may resolve to 127.0.1.1. A connection costs the kernel a few hundred
bytes - HAProxy's default buffers cost some 35 KB.

## Confirming a change

After the unit tests, on the default test cluster (six nodes, two on each of thinker10, thinker11 and theater):

```sh
# every machine builds the pushed branch and runs what it built
~/cluster/test_cluster.sh deploy 2026-10-03-Cluster-development
~/cluster/test_cluster.sh up                     # refuses machines with servers of different commits
xmq_scn_cluster --suite LoadTest/Cluster  # every test here; exit code 0 only if all passed
xmq_scn_cluster --suite LoadTest/Cluster --repeat 3   # for what shows in some runs only
```

`deploy <branch>`: every machine in `CLUSTER_DEPLOY_HOSTS` (thinker11, thinker10, theater) builds the
branch at once - over ssh, or directly on the machine `deploy` runs on - in `~/workspace/xmq_public_cluster`,
a worktree of `~/workspace/xmq_public` detached at `origin/<branch>`, with its own RelWithDebInfo
build in `build/`; the working trees there are left alone. The build runs at the lowest priority on
half the cores - theater runs the smart home - and is refused on a machine where the farm is
running. The first build takes several minutes, later ones only what changed. Builds of different
commits are refused; each machine builds against the SPTK installed in its `~/.local`. A build of
one's own, not pushed, goes the old way: `test_cluster.sh deploy <server> <host>...`, on the machine that
built it.

`deploy` copies the server, the SPTK it was built against (into `~/sptk/lib`) and the test cluster's own
scripts, and records the commit the server was built from in `xmq_server.source`; `up` and `status`
show it per machine. `up` also writes the test cluster's settings to `$CLUSTER_DIR/cluster.env` on every
machine, so `test_cluster.sh stop node3` means the same test cluster wherever it runs, and starts every node's logs
afresh, so that `check-logs` finds this run's crash and not yesterday's.

A test's `before` and `after` commands run around it - `after` whatever happened, which is where a
test undoes what it did to the network. The tests here end with `test_cluster.sh check-logs`: a crash in a
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
`the-storage-goes-away-from-one-machine.json` covers it on the test cluster; `nodeWhoseStorageStopsAnsweringGoesOfflineAndReturns`
in the unit tests.
