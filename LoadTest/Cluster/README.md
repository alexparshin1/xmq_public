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
test_cluster.sh crash node3   # kill -9: no handover, no lease released
test_cluster.sh down          # stop every node
test_cluster.sh wipe          # remove the directories; the next up starts fresh
```

Two things are shared and must read identically on every node: `persistence.redis_uri` (the cluster
compares the string, so proxies and different spellings of the same storage are out) and the accounts
database. Nodes are chosen by name and host:

```sh
NODES="node1@client-host node2@client-host node3@server-host node4@server-host" test_cluster.sh up
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
    "nodes": [ { "name": "node3", "host": "server-host", "port": 2884,
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

## A chaos run

`chaos_run.sh` is the survivability run: under the constant `chaos-p2p.json` load it kills each node
outright - `kill -9`, so nothing hands over and no lease is released - and brings it back, one node at
a time, over the whole run. The survivors must keep serving clients and carrying messages while a node
is gone, and the victim must carry traffic again afterwards. Every node is taken in turn, so the
coordinator and the nodes the load's clients are on are among the victims.

```sh
# three or more nodes; the defaults come from cluster.env
./LoadTest/Cluster/chaos_run.sh --nodes "node1@client-host node2@client-host node3@server-host" \
                               --duration 600 --cycle 90 --down 20
./LoadTest/Cluster/chaos_run.sh ... --dry-run      # print the generated timeline
```

The timeline is fixed, not random: the same nodes are killed at the same seconds on every run, so a
failure can be reproduced and looked at again. A cycle takes one node away for `--down` seconds and
gives it 45 seconds to serve again before the next cycle; the run needs at least three nodes, because
with two a crash leaves too little behind to prove that anything but the survivor still works.

`chaos_run.sh` writes the test it runs to `$CLUSTER_DIR/chaos-run.generated.json` and a copy of the load
with `duration_sec` set to `--duration` to `$CLUSTER_DIR/chaos-load.generated.json`; both are the record
of what was run. The load reconnects clients whose node goes away, and skips their turns while they are
reconnecting instead of ending the run, which is what makes it a measure of the cluster rather than of
the load's patience. `--keep-cluster` and the other `xmq_scn_cluster` options pass through.

The chaos load uses clean sessions, so it does not carry session state across a crash. That is a
deliberate part of what it measures - serving and delivery while nodes die - but it means a passing
chaos run says nothing about migration.

## A stateful migration run

`failover-session-migration.py` is the companion check: a persistent session must survive its owner
being killed, with the messages published during the outage. It connects a subscriber with
`clean_session=false` and a fixed client id to the target node, kills the node with the same
`kill -9` adapter the chaos run uses, publishes unique QoS 1 messages while the node is down, and
reconnects the same client id to a survivor. It also runs a clean observer on a third node, so a
publisher that stalled is distinguishable from a session that did not migrate.

```sh
XMQ_CLIENT_USER=user XMQ_CLIENT_PASSWORD=User-Secret1 \
python3 LoadTest/Cluster/failover-session-migration.py \
    --target-host node-c --target-port 1884 --target-node node-c \
    --target-adapter '~/cluster/test_cluster.sh' \
    --publisher-host node-a --survivor-host node-d --observer-host node-b \
    --redis-host <redis> --redis-db 4 \
    --message-count 10000 --publish-rate 100 --output-dir ~/migration-run
```

Without `--target-adapter` the script kills the `xmq_server` process for a numbered local instance
(`~/.local/etc/xmq<N>/xmq_server.conf`). QoS 1 is at-least-once, so the result allows duplicate
deliveries and fails on loss; it reports both counts.

## Measuring how long migration takes

`session_migration_time.py` answers the timing questions that the pass/fail test above does not:
how long N persistent sessions take to move off a node that has failed, and what a graceful
`Leave cluster` does to them. It creates N persistent sessions on one node, then either kills that
node or asks it to leave, and watches the shared Redis:

- `session_<client id>_owner` is the node serving a session, and `cluster:alive:<node id>` is its
  lease. A session is migrated when its owner is a live node other than the target.
- In `crash` mode it reports the time from the kill to lease expiry, and from the kill to the last
  session owned by a survivor, with first/p50/p95/last per-session times. The lease is what keeps
  another node from taking the sessions before it expires, so it dominates the result.
- In `leave` mode it reports the duration of the control operation and how many sessions the
  cluster adopts by itself. A graceful leave removes the node, its lease and its ownership markers;
  the surviving cluster does not walk the departed node's sessions and adopt them, so they remain
  unowned until their clients reconnect. The script then reconnects a sample to a survivor and
  times the claim.

```sh
XMQ_ADMIN_PASSWORD=... python3 LoadTest/Cluster/session_migration_time.py \
    --mode both --sessions 1000 --password User-Secret1 \
    --target node3 --target-port 2886 --survivor node1 --survivor-port 2884
```

The local test cluster is the default: `node3` on port 2886 is killed or leaves, `node1` on 2884
receives the reconnects, and `redis://theater:6379/4` is the shared store. The script restarts the
killed node and rejoins the node that left, and removes the sessions it made. It needs the admin
password for `--leave-cluster` and does not use it in crash mode. The output is `summary.txt` and
`summary.json` under `~/session-migration-results/<timestamp>/`.

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

After the unit tests, on the default test cluster (six nodes, two on each of client-host, server-host and farm-host):

```sh
# every machine builds the pushed branch and runs what it built
~/cluster/test_cluster.sh deploy 2026-10-03-Cluster-development
~/cluster/test_cluster.sh up                     # refuses machines with servers of different commits
xmq_scn_cluster --suite LoadTest/Cluster  # every test here; exit code 0 only if all passed
xmq_scn_cluster --suite LoadTest/Cluster --repeat 3   # for what shows in some runs only
```

`deploy <branch>`: every machine in `CLUSTER_DEPLOY_HOSTS` (server-host, client-host, farm-host) builds the
branch at once - over ssh, or directly on the machine `deploy` runs on - in `~/workspace/xmq_public`,
brought to `origin/<branch>` and built in its `build/` (RelWithDebInfo): one tree and one build per
machine, so a node never runs a binary from a build other than the one just made. A tree with
changes not committed is refused rather than checked out over. The build runs at the lowest priority on
half the cores - farm-host runs the smart home - and is refused on a machine where the farm is
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
