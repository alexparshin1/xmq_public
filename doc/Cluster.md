# XMQ cluster should support:

`[x]` means done; `[ ]` means not done, with what is done and what is left said underneath.

- [x] the cluster is a mesh of at most 10 admitted nodes, including the coordinator. This is the current supported
  size limit and the target for cluster validation. Admission of an additional node must be rejected if it would exceed the limit; unreachable
  members still count until their removal is committed through an agreed membership change.

- [ ] from the outside, cluster behaves as a very large broker. If there is an external load balancer in the front,
  the illusion should be complete.
  Done: a client connects to any node and takes its session along, so which node answered it is invisible; see the
  load-balancer item below.
  Left: `$SYS` is each node's own, and whether it should be a cluster-wide view is not decided.

- [x] client id is unique within the cluster. For sequential connections using the same Client ID, the later connection takes over the
  session and the previous connection is closed. If connections with the same Client ID arrive concurrently at different nodes,
  either one connection survives or both may be disconnected. After conflict resolution, two active owners of the session must not
  remain, and its persistent state must not be corrupted. Clients may retry.

- [x] the cluster has one active coordinator, selected in cluster join order. Its authority is a lease held in the
  shared Redis storage, the arbiter every node already depends on. Coordinator succession must remain safe during network
  partitions; join order determines the candidate priority, not the authority to act as coordinator.

- [x] cluster sessions are persistent and must survive the reboot of any node. If a node goes down, the online node(s) should
  take over orphaned sessions. A coordinator node may distribute the orphaned sessions between online nodes.
  The coordinator hands a gone node's sessions round the running members, which load them and go on collecting for
  them; a client returning anywhere takes its session along, with what was queued.

- [x] every node must use the same logical Redis storage and cluster data namespace as the coordinator. Storage belongs
  to the cluster and remains unchanged on coordinator succession. Access to that storage is required for client service;
  a node must not fall back to an independent Redis instance.
  Done: a node whose `persistence.redis_uri` differs is refused and one without it does not start on a cluster; a node
  that cannot reach the storage goes cluster-offline. Left: the URIs are compared as strings, which is wrong in both
  directions - the same storage reached at another endpoint (a failover, a proxy, a DNS name) is refused as another
  storage, and two different storages behind the same address (`redis://127.0.0.1:6379` on every node) are accepted.
  The comparison is to be made on what the storage itself holds, its `storage:id`; see *What lives where*.

- [x] all cluster nodes share one accounts database at the same PostgreSQL or MySQL endpoint. A node whose
  `authentication.database_uri` differs is refused at attach, because its users would be unknown to the rest
  of the cluster. The `database_uri` is sent in the attach request and compared like `redis_uri`.

- [x] any MQTT client can connect to arbitrary node of the cluster. The cluster automatically migrates the session
  from its previous owner to the node to which the client has connected.

- [x] any connections between cluster nodes are MQTT+SSL (encrypted).

- [x] an external load balancer can connect a client to any node in the cluster. It is the requirement above seen
  through a load balancer: no session affinity is needed, because the session moves to the node the client connected
  to. Tests: the same ones; there is no test with an actual load balancer in front.

- [x] a node can be connected and disconnected to/from the cluster. When a node is disconnected (or crashed), one or more of the cluster
  nodes should load its sessions and be ready to migrate them to the clients re-connecting from disconnected node.

- [x] retained messages, including replacements and clears, must be applied on all nodes regardless of whether they have subscribers.

- [x] concurrent retained changes to the same topic must be ordered by monotonically increasing per-topic revisions
  committed atomically with the retained state in shared Redis. The greatest committed revision wins on every node;
  replacements and clears follow the same conflict-resolution rule.

- [x] each cluster node must share its effective subscriptions with every other node. Subscriptions from multiple clients on the same
  node to the same topic filter must be represented as one internode subscription. Changes must propagate when clients subscribe,
  unsubscribe, or their sessions expire; subscriptions belonging to offline persistent sessions must remain active.

- [x] for non-shared subscriptions, a node receiving a publication must forward it only to nodes whose subscriptions match
  the topic, and at most once per destination node, even when multiple filters match. This is an internode transport rule;
  it does not determine recipients of shared subscriptions. A joining node must synchronize subscriptions before routing messages.

- [x] shared subscriptions (`$share/{ShareName}/{filter}`) must select one recipient session per matching shared subscription
  across the entire cluster, not one recipient per node. Independent nodes must not assign the same publication to different
  members of that shared subscription.

- [x] recovery and reassignment of in-flight shared-subscription deliveries must follow MQTT 5 QoS rules, as specified
  below: after a node failure, session migration or coordinator handover, a QoS 1 delivery may be retransmitted or
  reassigned, and an incomplete QoS 2 delivery stays bound to its selected session.
  Done: a member's session that ends - a clean session, or the cluster link to a member's node - hands its
  unacknowledged QoS 1 shared deliveries to another member; QoS 2 stays with its session. A message published as a
  session moves reaches it once: the node it left sends what matches it on by name until the node it went to has
  subscribed, and that node keeps such messages until the session's CONNACK is out.
  A node that stops hands what its members have not acknowledged to members on other nodes first; a link copy
  carrying several shared subscriptions offers the message to each again. A node that crashes loses what its
  members held unacknowledged - nothing outside it knew of it.

- [ ] a node that loses the shared Redis storage for longer than its lease switches to cluster-offline state, disconnects
  all MQTT clients, and stops accepting new client connections and delivering messages. A change of coordinator is not
  seen by clients. Client service resumes once the node reaches the storage again.
  Done: the lease, cluster-offline (clients disconnected, CONNECT refused as "server unavailable"), a member that
  starts again rejoining by itself - `restartedNodeKeepsItsIdentityAndRejoins`, `enabledNodeJoinsByItself` - a node
  whose place went with the storage taking it back by itself (`aNodeWhoseMembershipWasClearedRejoinsByItself`), and
  cluster routing stopping while offline: the links are closed and not reopened until the node holds its lease
  again (`anOfflineNodeClosesItsClusterLinks`).

- [x] a node is its GUID, made on its first start and kept in `xmq_node.id` beside its configuration; its name is for
  people and unique in the cluster. A node may not take another node's name, a node already running elsewhere (a
  cloned machine) does not start again, and a node that is not a cluster node may not use a cluster's database.

- [x] a cluster is formed and rejoined through the shared storage. A node with `cluster.enabled` joins the cluster in
  its Redis database by itself, or forms it; a member that starts again rejoins whether that is set or not. Either
  finds the other members, and their TLS addresses, in the storage.


## Acceptance

What covers each requirement today. *unit* is the suite in `test/`, whose cluster tests are
`XMQ_ClusterTests`; *test cluster* is a cluster of nodes built by `LoadTest/Cluster/test_cluster.sh` and driven by
`xmq_scn_cluster` - `LoadTest/Cluster/README.md` says how; *bench* is the load bench against a single
broker, for what the cluster must not make worse.

| Requirement | Covered by |
|---|---|
| 10-node mesh, admission refused past the limit | unit: `eleventhNodeIsRefused`, `AddDuplicateNode` |
| client id unique, takeover | unit: `enforceUniqueClientId`, `connectedClientIsTakenOverAcrossNodes`, `sessionMovesToTheNodeItsClientConnectsTo`, `clusterAndOrdinarySessionsCannotTakeOverEachOther` |
| coordinator, leases, succession | unit: `firstNodeCoordinatesAndEveryNodeIsOnline`, `nextNodeInOrderSucceedsTheCoordinator`, `coordinatorChangeDoesNotDisturbClients`, `clientsNeverSeeTheClusterAssignment` |
| sessions survive a node going down | unit: `sessionOfANodeThatIsGoneIsTakenOver`; test cluster: `the-second-node-comes-back.json` |
| a node starts while others attach to it | unit: `coordinatorStartedTwiceAtOnceStartsOnce`; test cluster: `test_cluster.sh up`, every node at once |
| a client may connect to any node | test cluster: `six-nodes-on-three-machines.json`, publishing and subscribing from each machine |
| encrypted internode links | unit: `clusterCredentialsRequireTls`, `unencryptedClusterJoinIsRejected`, `unencryptedPeerRecordIsRejected`, `clusterLinkRequiresTrustedCertificate`, `clusterLinkRefusesUntrustedCertificate`, `peerTlsUsesLocalKeys` |
| one logical storage, no fallback | unit: `nodeWithoutStorageServesNoClients`, `standaloneNodeMayNotUseAClustersDatabase` |
| one accounts database for the cluster | code: a node whose `authentication.database_uri` differs is refused at attach; test cluster: every node points at the same PostgreSQL on farm-host |
| node identity, name, a clone | unit: `nodeMayNotTakeAnotherNodesName`, `nodeRunningElsewhereDoesNotStartAgain`, `restartedNodeKeepsItsIdentityAndRejoins`, `nodeThatLeavesFreesItsPlace` |
| forming and rejoining | unit: `enabledNodeJoinsByItself`, `attachAndDetachToCluster`; test cluster: `test_cluster.sh up`, a node stopped and started again |
| subscriptions shared between nodes | unit: `effectiveSubscriptionsAreSharedAndDeduplicated`, `subscriptionsReachEveryNode`, `joiningNodeLearnsExistingSubscriptions`, `subscriptionBurstReachesThePeerWhole`, `unsubscribeStopsForwarding`, `overlappingFiltersForwardOneCopy`, `publicationGoesOnlyToNodesWithSubscribers`, `expiredSessionWithdrawsItsSubscription` |
| shared subscriptions across the cluster | unit: `sharedSubscriptionDeliversOncePerCluster`, `independentSharedSubscriptionsEachGetEveryMessageOnce`, `oneShareNameWithTwoFiltersIsTwoSubscriptions`, `sharedSubscriptionIsNotServedAgainWhereAPlainSubscriberTookTheMessage` |
| retained, including a node that was away | unit: `XMQ_RetainedMessages.*`, `retainedMessageReplicatesWithoutSubscribers`, `concurrentRetainedChangesConverge`, `concurrentReplacementsAndClearingsConverge`, `retainedChangeCommittedButNotSentReachesEveryNode`, `lateRetainedChangeDoesNotBringAClearedMessageBack`, `retainedClearedWhileNodeWasDownStaysCleared`, `retainedReplacedWhileNodeWasDownIsTakenOnRejoin`, `joiningNodeReceivesRetainedMessagesWithoutSubscribers`, `joiningNodeDoesNotReplayRetainedToSubscribers` |
| cluster-offline, and returning to service | unit: `nodeWithoutStorageServesNoClients`, `nodeWhoseStorageStopsAnsweringGoesOfflineAndReturns`; test cluster: `the-storage-goes-away-from-one-machine.json` |
| in-flight shared-subscription deliveries across a failure | unit: `sharedQos1MessagesSurviveAMemberNodeStopping`, `sharedQos1MessagesANodeTookSurviveItStopping`, `sharedQos1MessagesForSeveralSubscriptionsSurviveAMemberNodeStopping`, `sharedQos1MessagesOfAnEndedSessionGoToAnotherMember` |
| a message published as a session moves | unit: `messagePublishedAsASessionMovesReachesIt`, `messagesPublishedWhileASessionMovesArriveOnce` |
| a load balancer in front | not covered |
| the cluster as one broker in `$SYS` | not decided |

"Not covered" means what it says: no test speaks to it yet.


## Retained conflict resolution

Shared Redis is the authoritative retained store. Each retained change, including a clear, atomically allocates the
next revision for its topic and commits that revision together with the resulting retained state. Revision allocation
and state replacement must be one atomic operation; allocating a revision and later performing an unconditional write
could allow an older update to overwrite a newer one.

Concurrent changes are ordered by their atomic commits in Redis. The change with the greatest committed revision wins,
regardless of the originating node, node uptime, wall-clock timestamps or internode arrival order. No ordering between
different topics is required. The coordinator does not independently arbitrate retained conflicts.

Internode retained updates carry the committed revision and resulting state. Each node applies an update only if its
revision is greater than the locally applied revision. Equal revisions are idempotent repeats of the same committed
change; lower revisions are ignored. Internode propagation and retries must never create a new retained mutation or
allocate another revision for an already committed change.

A clear removes the topic from the storage, in the same atomic step that numbers it; it leaves no
deletion marker. Topic revisions must not reset on clear, node restart or coordinator succession. A
node that receives a change to a topic it does not hold cannot tell it from a late change to a topic
cleared since, so it takes such a change only from the storage, never from the message: a delayed
publication cannot resurrect a cleared retained value.

Nodes must recover missed updates from authoritative storage, including after a writer commits a change but fails
before broadcasting it. Joining and returning nodes synchronize retained state and deletion revisions before serving
clients. Any local cache used to answer a retained lookup must be validated against authoritative state so that an older
cached revision is not replayed as the current retained value to a new subscriber. Propagation may be asynchronous,
but all replicas must converge to the greatest committed revision without rollback.

Validation must cover concurrent replacements, replacement racing with clear, two concurrent clears, out-of-order and
duplicate propagation, failure between commit and broadcast, and restart or rejoin with stale retained state.

## Cluster-wide shared subscriptions

The cluster acts as one MQTT Server for shared subscriptions. A shared subscription is identified by the exact pair
`(ShareName, filter)` across all nodes. The same ShareName with different filters denotes distinct shared subscriptions.
Every matching shared subscription is processed independently; non-shared subscriptions retain their own delivery rules.

For each publication and matching shared subscription with eligible members, the cluster must select exactly one
recipient session across all nodes. Broadcasting to member nodes and letting each node choose a local recipient is
forbidden. Internode aggregation must preserve shared-subscription identity and enough membership information to select
the recipient session; collapsing these subscriptions into an ordinary topic filter is insufficient.

The assignment mechanism must prevent competing assignments during concurrent routing, internode retries, session
migration and coordinator handover. A retry of the same routed publication must reuse its assignment unless an explicit,
MQTT-compliant reassignment is made. Offline nodes cannot initiate delivery. Shared-subscription membership and delivery
state required for recovery must be synchronized before a node begins routing.

Selecting one recipient does not promise exactly-once application processing at every QoS. Delivery must respect the
selected session's granted QoS and [MQTT 5 section 4.8.2](https://docs.oasis-open.org/mqtt/mqtt/v5.0/os/mqtt-v5.0-os.html):

- QoS 1 permits retransmission or reassignment after disconnection, with possible duplicate delivery.
- An incomplete QoS 2 delivery remains bound to the selected session across reconnection and node migration. If that
  session terminates, the message must not be reassigned to another member.
- A PUBACK or PUBREC with a Reason Code of 0x80 or greater requires discarding the message without trying another member.

Validation must cover members on different nodes, publications entering through different nodes, multiple independent
shared subscriptions, overlapping shared and non-shared filters, internode retries and recovery of in-flight deliveries.
With healthy connections, one matching shared subscription must produce delivery to one member in total across the cluster.

## Coordinator, leases and succession

The coordinator looks after membership and recovery, including the distribution of orphaned sessions. It is not on
the clients' path: no node needs it to serve clients, and a change of coordinator is invisible to them. The role is
assigned automatically, not attached to a configured node. Message routing goes over the mesh.

### The shared storage is the arbiter

Every node needs the shared Redis storage to serve clients anyway, and Redis has the one clock all nodes agree on.
So the cluster's state is kept there, as keys with a TTL, and no node compares clocks with another:

- `cluster:members` - the admitted nodes, scored by admission order: the succession order. At most 10. A member that
  is down still counts; only leaving the cluster removes it.
- `cluster:alive:<node>` - the node's client-service lease, renewed by the node itself.
- `cluster:term` - the coordinator generation; every new coordinator increments it.
- `cluster:coordinator` - `<term> <node>`, set only when absent and kept by renewal.

`cluster.lease_seconds` (10 by default) is the TTL; nodes renew five times per lease.

### Node identity and joining

A node is its GUID, made on its first start and kept in `xmq_node.id` beside its configuration - not in the
configuration, which is copied to set up the next node and would take the GUID with it. The storage keeps, per node,
its name and its TLS address (`cluster:node:<guid>`), and which node each name belongs to (`cluster:names`):

- A node whose name another node has is refused.
- The lease (`cluster:alive:<guid>`) holds the id of the node's current run. A node finding it held by another run
  waits one lease - its own previous run, if it crashed, serves nothing and lets go - and is then refused: another
  process runs as this node, a cloned machine say.
- A node that is neither a member nor `cluster.enabled` does not start on a database that holds a cluster: its
  sessions would be written over the cluster's.

A node with `cluster.enabled`, or one that is a member already, takes its place on start, before its listeners open:
it is admitted - or let back in - and gets its lease. Once its listeners are open it links to a running member, found
in the storage with its TLS address; that member answers with all the others, as for a join through it.

### Node lease and cluster-offline state

A node serves clients while it holds its lease, that is while it can reach Redis. It counts the lease from before it
asked Redis, so it stops serving no later than Redis lets the key expire. Until the key has expired no other node may
take over what the node serves, so a Redis outage shorter than a lease costs its clients nothing. Once it has expired,
the node is gone as far as the cluster is concerned: its sessions may be taken over elsewhere, and the node itself
has stopped serving them.

Losing the coordinator, or other nodes, does not take a node offline. A node that reaches Redis but not the other
nodes keeps serving its clients, while messages between it and the rest of the cluster stop: a routing failure, not
two owners of a session, which session ownership in Redis prevents.

In cluster-offline state a node:

- Disconnects all MQTT clients and refuses new ones as "server unavailable". Persistent sessions stay.
- Keeps its cluster links.
- Retries Redis, and serves clients again as soon as it renews its lease.

A node that leaves the cluster gives up the coordinator key if it holds it, frees its place among the members and
loses the subscriptions it received while it was one - the filters of the others' clients, held on it by their links -
so that it routes nothing for the cluster any more. The sessions it served are the cluster's, not its own: it gives
up their ownership keys, so that the cluster takes them over at once, disconnects those clients, and refuses a
CONNECT naming a client id that has a session in the cluster's storage - serving one would make a second owner of a
session the cluster holds, and its client would keep a session the cluster no longer serves. It serves clients of
its own again once it is started on storage of its own.

### Succession

When the coordinator key is gone, the first member in the order takes it at once and each one after it waits one
renewal longer, so the most senior node that is alive becomes coordinator, in a new term. Another node can take the
key only after it has expired. The coordinator is kept first in the order: a node found ahead of it - a former
coordinator, or one passed over while it was down or cut off - goes to the end, behind every node admitted
meanwhile, and cannot claim its old place back.

### Rejoining the cluster

Restoring a connection to the coordinator does not immediately restore client service. The node must:

1. Confirm access to the shared Redis storage.
2. Connect to the other members and synchronize the cluster state required for service, including subscriptions and
   session ownership.
3. Resolve stale ownership and any state conflicts before routing messages or accepting clients.
4. Obtain a new client-service lease and only then enter cluster-online state.

### A node that joins while starting must not end the process

Fixed: the thread is installed under the lock that asks whether it is there
(`coordinatorStartedTwiceAtOnceStartsOnce`). What follows is how it was found.

`Cluster::startCoordinator()` has four callers: two as the node starts, from the membership it reads in the storage,
and two from the link's own message handling - `onAttachNodeRequest()` and `onAttachNodeResponse()` - which run on that
session's receive thread. `Coordinator::start()` asks under its mutex whether the thread is already there, and returns
if it is, but assigns `m_thread` after the lock is let go (`Coordinator.cpp:189-235`). Two callers arriving together
therefore both pass the question, and the second assignment lands on a joinable `std::thread`: that is
`std::terminate`, and with it the process. The node that dies may be the one that was only told about a neighbour; the
one that asked sees no more than the link it tried failing.

The backtrace ends at that assignment rather than at an exception, which is what tells the two apart - an exception
leaving the handler would show the throw in between: `std::terminate()` called directly by
`xmq::cluster::Coordinator::start()`, called by `xmq::cluster::Cluster::onAttachNodeRequest()`, called by
`ClientSession::handlePublishMessage()`, from `ClientSession::receiveMessages()`.

What it takes is for the node's own startup and an incoming attach to reach that function together. Starting both nodes
of a test cluster at the same moment is exactly that: each asks the other, and each handles the asking while still starting
itself.

1. Two nodes on one host sharing one Redis, which is what the test cluster arranges (`LoadTest/Cluster/test_cluster.sh up`).
2. `test_cluster.sh down`, then `up`, so both start together. Watch for a node to go within a second or two of `Server
   started`: its console log ends with `terminate called without an active exception`, its ports stop listening, and
   the other logs `Node <name> couldn't connect to node <address>`.
3. The window is the few microseconds between the lock being released and the thread being assigned, so it does not
   come out every run: in one session it took three restarts of three, and attempts in a row afterwards did not reach
   it at all. Repeating `down; up` is what brings it out. The line stays in the node's console log, so the count of
   them grows across runs and is the more reliable check - the node can go a few seconds after the moment a fixed
   liveness check looks, and a process that is gone is otherwise easy to mistake for a test cluster that was never up.
4. Under a debugger, a node run as `gdb -batch -ex run -ex "thread apply all bt" --args xmq_server -c <node
   configuration>` while the other is restarted prints the backtrace above when it is the one that aborts. A debugger
   slows the startup enough to hide a race by itself, so the node that is not under it is the better one to watch.

The question and the assignment belong under the same lock: asking whether the thread is there is worth something only
if the thread is installed before the lock is let go. Nothing in the storage or in the other node is at fault here, and
an emptied storage does not prevent it.

## Session ownership

A session is served by one node at a time. `session_<clientId>_owner` holds that node's GUID; a node takes it when it
is free, its own already, or held by a node whose lease has expired - a node that is gone. Before a CONNECT is
completed, the node it arrived at takes the session:

1. It claims the owner key. If another running node holds it, that node is asked (`$CLUSTER/request/release_session`)
   to let the session go, and the claim is repeated until it succeeds - for up to three seconds, after which the
   CONNECT is refused as "server unavailable" and the client may try again.
2. The node letting go disconnects the client, if connected, and drops the session from memory without touching its
   record: its queued messages are written if they were still waiting to be and their records kept. Once Redis has
   confirmed all of it, it hands the owner key to the asking node. It forwards for a while after that: a publication
   matching the filters the session had, sent here by a node that still counts this one among the subscribers for
   them, is sent on to the node the owner key now names. It stops when that node advertises those filters itself,
   and in any case after one renewal. The forwarding is what keeps a move from losing a publication. The same
   publication may then reach the session twice - routed to its new node directly, and forwarded to it - so the
   node that took the session delivers one it has already delivered there only once, matching it by the origin it
   carries: the node that first took it from a publishing client, and that node's sequence number for it. That
   window lasts as long as the forwarding may, and it is what keeps a QoS 2 delivery exactly once across a move
   instead of relying on the latitude QoS 1 gives for duplicates.
3. The asking node loads the session from Redis - subscriptions and queued messages - as a node restoring its own
   sessions does, and advertises its subscriptions to the other nodes. The subscriptions are advertised before they
   are withdrawn, and withdrawn on the node letting go only after that, so most of what arrives in between is already
   routed to where the session now is and only the rest needs forwarding: whichever of the two a node saw first, the
   publication finds the session.

Two nodes claiming the same session at once end with one owner; the other CONNECT waits or is refused.

## Shared Redis storage

All cluster nodes, including the coordinator, use one logical Redis storage and the same cluster data namespace.
The authorized coordinator supplies the agreed storage configuration to joining and returning nodes. Each node
must verify access to that storage before entering cluster-online state; a successful coordinator connection or a
valid client-service lease alone is insufficient.

Storage identity belongs to the cluster, not to the current coordinator. A new coordinator inherits the existing
storage configuration and must not substitute its own Redis instance. A returning former coordinator adopts the
current cluster storage configuration before synchronizing state. Nodes must not use an independent Redis instance
as a fallback, since it would create divergent persistent session and message state.

The requirement concerns logical storage identity and the cluster namespace, rather than a fixed server address.
Redis failover may change the endpoint while retaining the same logical storage. Switching to a different data store
is a separate cluster storage migration, not a consequence of coordinator election.

A node that loses the Redis access required for correct session and message processing must enter cluster-offline
state even if the coordinator is reachable and its lease has not expired. It disconnects clients and stops client
service while retrying storage access. If the shared Redis storage is unavailable to every node, client service stops
throughout the cluster. Recovery requires restored access to the agreed storage, state synchronization and a valid
client-service lease before returning to cluster-online state.

## Cluster transport

Cluster peers connect to MQTT+SSL listeners. `cluster.this_node.host_port` must advertise a
reachable TLS endpoint (normally port 8883); the initial join also takes a peer's TLS endpoint.
Existing configurations that advertise the plain MQTT port must be updated before joining.
An explicit request for an unencrypted join, or a discovered peer record with `encrypted: false`,
is rejected. The reserved `cluster` account is admitted only over TLS; ordinary MQTT clients
can continue using the plain listener, including taking over their previous TLS connection
with the same Client ID. A cluster session and an ordinary session never take over each other:
an ordinary client that names a cluster link's Client ID is refused, and so is a cluster
connection that names an ordinary client's. The transport requirement applies to internode links.

Outgoing links use the local node's certificate and key (`connections.ssl_keys`, or the node's
own pair when none is named), never certificate paths advertised by another machine. The peer
is always verified: against `connections.ssl_keys.cafile` when it is set, otherwise against the
certificates in the `peers` directory next to the node's own certificate - the same trusted
peers bridges use. Each node normally has its own self-signed certificate, so every node must
hold the certificates of the others there. With nothing to verify against, no link is opened;
`verify_depth` 0 is read as 1, since a cluster link is never left unverified. Only the chain is
checked, not the host name.

## Subscriptions

Each node tells the others which filters its own clients use - one entry per filter however many
clients share it, kept while a persistent session is offline and dropped when it expires. A node
that connects to another is sent its whole set once; after that only changes travel
(`$CLUSTER/request/subscription_update`, one `+filter` or `-filter` per line). Changes are sent by
a thread of their own, a few milliseconds at a time, so a client's SUBSCRIBE never waits for the
network and a burst of subscriptions goes out as a few messages.

Routing is those subscriptions: a node subscribes on every other node to the filters its own
clients use, and nothing else, so a publication travels only to the nodes that have a subscriber
for it, and once to each - a node is one session on the others, and a session gets one copy
however many of its filters match. A publication forwarded by another node is never handed to a
cluster link, so nothing travels twice - except one sent on for a session that has just moved, to
the node its owner key names: that node delivers it to the session it holds and, arriving itself
over a link, forwards it no further. A `$share` group spread over nodes gets each message once
in the whole cluster: on each node the links to the others are members of the group, and a member
that may not take the message - a link, for a message that came over one - is passed over for the
next, never handed it and then skipped.

## Retained messages

Every node holds every retained message, whether or not anyone there subscribes. A change made on
a node - set, replaced or cleared - is committed in the shared storage, which allocates it the next
revision for its topic, and is then sent to all the other nodes in its own cluster message
(`$CLUSTER/request/retained`) carrying that revision together with the state the commit left. A node
takes it only if the revision is greater than the one it has applied - equal revisions are repeats of
the same commit - and ignores it otherwise. The storage clock does not order retained changes; the
revision does, and it is allocated by the same atomic commit that writes the state, so no node
compares clocks with another and no timing can reorder them. The commit is not waited for: the change
is acknowledged to the publisher as soon as the node has taken it, and the storage write follows
behind. Writes are issued in the order the changes were taken - one writer, in order - so that two
changes to the same topic are numbered in the order they were accepted. A node that dies before a
write lands has lost a retained change whose publisher was already acknowledged. A cleared topic
leaves nothing behind. Revisions are handed out per shard (`cluster:retained_rev:<k>`, the records in
`cluster:retained:<k>`, `k` the topic's hash modulo 16), so that changes to different topics do not
queue behind one another on a single key; the revisions of a shard leave no gaps. A node that sees a
shard's counter stay ahead of what it has applied - a change committed by a node that died before
sending it, or sent while the link was down - reads that shard again. A node that starts, or comes
back from cluster-offline, reads every shard before serving clients, and a change it is sent for a
topic it does not hold is checked against the storage before it is taken, so a node which was down
or cut off neither misses a change nor brings a cleared message back: a node's memory is never the
authority for a retained message. `$SYS` stays each node's own.
Publications forwarded between nodes carry no retained state, and carry the origin they were first
taken from a client at: the node, and its sequence number for that publication. That is what the
node holding a session that has just moved matches on to deliver a publication once; the links
subscribe without retained replay, so a joining node does not re-deliver retained messages to
subscribers.

## Cluster configuration and the interface

The cluster is declared by an administrator in the interface, not by editing a file on each node.

### What lives where

The configuration file keeps only what a node needs to find everything else: the SQL database URI and the GUI port.
The node's GUID stays where it is - `xmq_node.id` beside the configuration, not in it, for the reason given in
*Node identity and joining*.

The SQL database (PostgreSQL or MySQL - SQLite does not apply to a cluster) holds what is shared and what an
administrator edits. The configuration is one table, `xmq_node`, in which a column is a setting and a row is a node. Every node whose
configuration lives in this database has a row, member or not; a node the administrator declares with Add has one too,
keyed by a GUID the inviter generates until the Invite learns the node's own and takes it over. The bridge-era table
that held the name before is dropped when the schema is created - nothing reads it any more. The configuration is:
`node_id` (the GUID, primary key), `name`, `host_port`, `state`, `last_error`, and the settings that used to be in
the file - the Redis URI, the users database URI, the listeners and TLS key paths, `cluster_enabled`,
`cluster_lease_seconds`, `cluster_links`. A second table, `xmq_cluster_invitation`, holds at most one active row per
node: the node's GUID, a one-time token, the inviting node's address, `expires_at`.

Redis keeps the runtime (see *The shared storage is the arbiter*): `cluster:members`, the leases, the coordinator
keys, sessions, queues and retained messages. Those are written several times per lease and are not configuration.

The cluster has an identity of its own - `cluster:id`, a GUID created when the cluster is formed - so that a node can
tell "the same cluster" from "another cluster on the same storage" or "the same addresses, different cluster".

The storage has one too: `storage:id`, a GUID written into Redis when XMQ initializes it, the counterpart of MySQL's
`server_uuid`. Membership compares that and not the URI, because an address proves nothing in either direction: the
same Redis reached by a DNS name, a proxy or after a failover has one id under every address, and two Redis instances
behind the same address - each node's own `redis://127.0.0.1:6379` - have two. A node compares `storage:id` first
and is refused as being on another storage; then `cluster:id`, and is refused as belonging to another cluster.
Resolved addresses are compared only to say which address answered in the error message.

A node that cannot reach the SQL database at start does not start, and says why. Without the shared configuration it
cannot tell whether it is a cluster node at all, and serving on a guess risks writing over another node's sessions.

### States

| State | Meaning |
| --- | --- |
| Not Clustered | declared by an administrator with Add, no invitation sent yet |
| Standalone | the node's own row: it uses this database for its configuration and is not a member |
| Invited | an invitation is active and the node has not answered |
| Joining | accepted; the node is applying the configuration and restarting |
| Clustered | in `cluster:members` and holding its lease |
| Offline | in `cluster:members`, lease expired - down or cut off from Redis |
| Refused | the node declined the invitation |
| Expired | no answer within the invitation's lifetime (10 minutes) |
| Failed | the node accepted but did not join; the configuration was rolled back |

`Clustered` and `Offline` are computed from Redis and are never stored in SQL; the others are stored and are what the
administrator sees in the interface.

### Declaring and inviting

1. On the dashboard, the mode is chosen: **Standalone** or **Cluster**. Switching a standalone node to Cluster asks
   for the shared Redis URI and starts it as the first member and the coordinator. If the storage already holds a
   cluster, the node refuses with an error saying so and asks for an invitation: a cluster is joined by invitation,
   never by configuring a node to join it.
2. **Add node** on the Cluster Configuration screen adds a row: name and TLS address. The node is `Not Clustered`;
   nothing is contacted.
3. **Invite** on a selected row opens a dialog asking for the password of the `cluster` user; the invitation is
   published through the cluster's own MQTT client to the invited node's `cluster/invitation` topic, over TLS.
   It carries the Redis URI, the users database URI and the inviting node's address and names - the invited node
   is standalone and knows none of them. The `cluster` user's password is the same on every node of the cluster,
   and the inviting node checks it by connecting: an authentication refusal is reported before the invitation is
   sent.
4. The invited node, subscribed to `cluster/invitation` at the server level (it needs no client), shows the
   invitation on its own Cluster screen as a button. **Review** opens a confirmation that names every change:
   persistence off -> the shared Redis, the local users database -> the shared one, the cluster enabled, the node
   restarting and its clients disconnecting, and that local persistent sessions, queues and accounts are not carried
   over (moving accounts is a separate operation, by its own request and from a given database URI).
   **Accept** applies the configuration - the SQL database URI included, since that is in the bootstrap file - and
   ends the process with the restart exit code; **Decline** answers the inviting node, which records `Refused`.
5. Checks before the invitation is shown: the XMQ versions match, the Redis and the users database are reachable,
   the node is not a member of another cluster, and the node is not already a member of this one. A failed check
   shows the invitation with the reason; Accept is disabled while it stands. An invitation to a node that is
   already a member of this cluster, with the same storage and users database, is ignored and answered as such.
6. On Accept the node writes the new configuration and restarts. The first join of a node is by invitation only;
   afterwards its own `cluster_enabled` brings it back, because its GUID has been admitted already. Every start
   that joins checks the identity of the storage and of the users database just the same - `storage:id`,
   `cluster:id`, `database_uri` - and a node that fails a check does not join. The inviting node learns of the
   result from the state in SQL and from `cluster:members`; a node that does not join within the agreed time is
   `Failed`, and the node that tried keeps its previous configuration (the last-good copy) and serves on it.

### Interface

- **Dashboard:** the mode, `Standalone | Cluster`.
- **Cluster Configuration:** the declared nodes with their state, the coordinator, the number of open links and the
  last error; **Add node**, **Invite**, **Remove**. Remove deletes a node that is not a member. A member is detached
  first - its sessions are handed over - and then removed: its row is changed (`cluster_enabled` off) and the node
  is restarted. It keeps the same Redis, and writes its own sessions under a namespace of its GUID, so that a node
  that is no longer in the cluster names nothing the cluster uses.
- **Permissions:** add, invite, accept, decline and remove are for the administrative group of the node's own
  users.

## Release plan

- **0.9.20:** Introduce the cluster functionality.
- **1.0:** Release a complete, tested cluster meeting the requirements above. Coverage of all
  code in `server/Cluster` must be at least 85% by completion of cluster development.
- **1.0.1:** Develop the optional commercial cluster management and monitoring module.

### Cluster configuration interface

In this order - each step leaves the system working:

- [ ] Move the shared settings to SQL: `xmq_node` (a column is a setting, a row is a node, the GUID is the
  key) and `xmq_cluster_invitation`; a bootstrap loader that reads the SQL URI from the file and everything else from
  the database. Keep `node_id` in `xmq_node.id`. A node that cannot reach SQL refuses to start and says why.
  Done: the tables, `ClusterStore`, and the loader - a node's row is created from its configuration on the first
  start and read in place of it afterwards for the storage, the accounts database, the cluster switches and the
  node's name and address; an unreachable database stops the start with the reason. The table is `xmq_node`,
  not `xmq_node`: the older name belongs to the bridge-era node table still present in shared databases.
  Left: the rest of the configuration - listeners, logging, limits, bridges - still comes from the file.
- [ ] Sessions a node writes when it is not a cluster member go under a namespace of its GUID, so that a node
  removed from the cluster names nothing the cluster uses while it serves on the same Redis.
- [ ] Add `cluster:id`, created with the cluster, and `storage:id`, written into Redis when XMQ initializes it.
  Compare a joining node's storage and cluster by those ids instead of by the URI, and keep the resolved address only
  for the error message.
- [ ] Implement the state machine behind the screen: declared, invited, joining, refused, expired, failed; the
  computed `Clustered`/`Offline` from Redis. One active invitation per node, ten-minute lifetime.
- [x] Invitation transport: the server-level subscription to `cluster/invitation` (the server itself, not a client),
  authentication as the `cluster` user over TLS (the password checked by connecting before sending), the Redis and
  users database URIs in the invitation, and the refusal reply. `anInvitationFromTheClusterUserIsHeld` and its
  siblings; the inviter's side is `invitingANodeDeliversTheInvitation`, including the `cluster/hello` by which it
  learns the invited node's GUID.
- [x] Accept on the invited node, at the server level: the offered configuration is written into the node's own
  file, the process exits with the restart code 78, and the row goes Invited to Joining at the next start.
  `acceptingAnInvitationAppliesTheConfigurationAndAsksToRestart`; Decline answers the inviter on cluster/refusal and
  the inviter marks the row Refused - `decliningAnInvitationAnswersTheInviter`. The rollback to the last-good
  configuration with `Failed` when the join does not happen is in place too, wired into the start that follows the
  accept. Left: the confirmation the screen shows before Accept - the Review dialog.
- [x] The server side of the interface: `ClusterControl` with get, enable, add, remove, invite, accept and decline.
  `get` answers the whole picture - the mode, the declared nodes with states computed from the storage over the
  stored ones, the coordinator, the open links and the invitation the node holds with what accepting it would
  change. `add` declares a row with a GUID the inviter generates; `invite` takes the cluster account's password
  from its dialog and publishes the invitation, the invited node's own GUID taking the declared row's place;
  `remove` takes a declared node away and refuses a member, which is detached from its own screen first; `enable`
  makes this node the first member of a cluster on the storage it names, refusing a storage that already holds a
  cluster - a cluster is joined by invitation - and asking for the restart. What the screen still owes: the
  `disable` direction, which waits for the per-GUID session namespace below - a node that leaves would otherwise
  serve on the storage its cluster uses - and a member's Remove, which waits for the same.
- [ ] The React side: the dashboard mode, the Cluster Configuration screen over `ClusterControl`, the Add and
  Invite dialogs, and Review/Accept/Decline on the invited node.
- [ ] Tests: the state machine and the checks as unit tests, invitation/accept/decline/expiry/rollback as cluster
  tests, and a `test_cluster.sh` scenario that invites a node and watches it join.

### Recovery without an operator

The holy grail of an MQTT cluster, and the reason XMQ exists: a real installation of another broker in 2014-2016
lost one or two nodes a day and never came back, with two weeks of the vendor's own engineer. Throughput is not the
test; a node that dies, is cut off, or loses its lease coming back by itself is.

Likely after the 1.0.0 announcement rather than in it - but not optional:

- [x] A node that lost its lease while cut off from the shared storage rejoins the cluster by itself once the storage
  returns, instead of staying cluster-offline until somebody restarts it: `step()` joins again when the membership
  has no place for it, and `JoinScript` still refuses when another run holds the node's lease, so a clone cannot
  come up beside its original. `aNodeWhoseMembershipWasClearedRejoinsByItself`.
- [x] An offline node stops cluster routing: its links are closed when it goes offline and `keepLinks` leaves them
  closed until it holds its lease again, so it neither forwards to the nodes it can reach nor takes their traffic.
  `anOfflineNodeClosesItsClusterLinks`.
- [ ] A chaos run on the test cluster: `kill -9` random nodes and cut one node off from Redis, over an hour, and the
  cluster returns to full service with no operator action. This is the acceptance test for the two items above, and
  it is what a night on the farm cannot show by itself.

### 1.0.1 tasks

- [ ] Provide live control of node and cluster settings, with validation and per-node application
  status. Identify settings that require a controlled restart.
- [ ] Monitor node availability, cluster links, session ownership and synchronization status,
  plus CPU, RAM, connections, traffic, queues, latency and persistence health.
- [ ] Use the existing API to start and stop nodes, including controlled withdrawal and session
  handover before stopping a node. These operations change the node's operating state while
  the systemd service and management API remain running; they do not stop or start the service.
- [ ] Provide administrative permissions, an audit trail, historical metrics and alerts.
- [ ] Keep the module optional: its absence or failure must not prevent cluster routing,
  coordinator election, session migration or automatic recovery.

## Cloning a node

Create a second node from an existing configuration without starting it:

```sh
xmq_server --clone-node xmq1 \
    -c xmq2 \
    --node-name thinker10-2 --node-host thinker10 \
    --mqtt-port 2883 --mqtt-ssl-port 9883 --web-port 28883
```

The destination directory must not exist. The clone receives a new UUID in
`xmq_node.id` and new `certs/node.crt` and `certs/node.key`, issued for `--node-host`.
The source configuration, UUID and keys are unchanged. All three ports are required,
must differ, and must be between 1024 and 65535. Configurations with additional
enabled listener protocols must be configured separately before cloning.

Shared SQL accounts and Redis settings are retained. Local SQLite accounts are
snapshotted into the clone's own database, including committed WAL data; legacy
accounts files are copied when there is no database. Extension configuration and
the setup template are copied, but peer certificates and runtime state are not.
The clone has its own log and starts with `clean_start` disabled.

Start the new node explicitly, then use the usual cluster admission flow:

```sh
xmq_server -c xmq2
```

The clone is standalone until admitted; copying a configuration does not copy
cluster membership. Its first start registers the new identity in the shared SQL
database when one is configured.

### Joining and leaving from the command line

With the cloned node running, request admission through its own control API:

```sh
xmq_server -c xmq2 --join-cluster
```

The command reads the administrator password from standard input (without echo
on Unix terminals). For automation, pass the password through a pipe; it is never
a command-line argument. Use `--admin-user NAME` for another administrator account.
The node must share the PostgreSQL or MySQL accounts database with a live cluster
member. The existing admission checks select that cluster; this command does not
create one or switch the node's database.

```sh
xmq_server -c xmq2 --leave-cluster
```

Both operations contact only the local running node, check its UUID against the
selected configuration's `xmq_node.id`, and wait up to 30 seconds for the resulting
state. HTTPS trusts the web certificate specified in that configuration (or its
own `certs/node.crt`). They do not launch another server. Joining an already live
member and leaving an already standalone node are successful no-ops.

The complete workflow is: clone, start, verify readiness, join. Leaving returns
the running node to standalone mode without deleting its configuration or identity.

A bare configuration name resolves relative to the installed configuration root:
`-c xmq2` means `~/.local/etc/xmq2/xmq_server.conf` for a home installation, or
`/etc/xmq2/xmq_server.conf` for a system installation. The same shorthand works for
`--clone-node xmq1`. Full paths, `./node` paths and filenames such as `custom.conf`
retain their normal meaning; the working directory does not affect bare names.
