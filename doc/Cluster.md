# XMQ cluster should support:
- [ ] the cluster is a mesh of at most 10 admitted nodes, including the coordinator. This is the current supported
  size limit and the target for cluster validation. Admission of an additional node must be rejected if it would exceed the limit; unreachable
  members still count until their removal is committed through an agreed membership change.

- [ ] from the outside, cluster behaves as a very large broker. If there is an external load balancer in the front,
  the illusion should be complete.

- [ ] client id is unique within the cluster. For sequential connections using the same Client ID, the later connection takes over the
  session and the previous connection is closed. If connections with the same Client ID arrive concurrently at different nodes,
  either one connection survives or both may be disconnected. After conflict resolution, two active owners of the session must not
  remain, and its persistent state must not be corrupted. Clients may retry.

- [ ] the cluster has one active coordinator, selected in cluster join order. Its authority is granted by a lease
  confirmed by a majority of the agreed voting membership. Coordinator succession must remain safe during network partitions;
  join order determines the candidate priority, not the authority to act as coordinator.

- [ ] cluster sessions are persistent and must survive the reboot of any node. If a node goes down, the online node(s) should
  take over orphaned sessions. A coordinator node may distribute the orphaned sessions between online nodes.

- [ ] every node must use the same logical Redis storage and cluster data namespace as the coordinator. Storage belongs
  to the cluster and remains unchanged on coordinator succession. Access to that storage is required for client service;
  a node must not fall back to an independent Redis instance.

- [ ] any MQTT client can connect to arbitrary node of the cluster. The cluster automatically migrates the session
  from its previous owner to the node to which the client has connected.

- [x] any connections between cluster nodes are MQTT+SSL (encrypted).

- [ ] an external load balancer can connect a client to any node in the cluster.

- [ ] a node can be connected and disconnected to/from the cluster. When a node is disconnected (or crashed), one or more of the cluster
  nodes should load its sessions and be ready to migrate them to the clients re-connecting from disconnected node.

- [x] retained messages, including replacements and clears, must be applied on all nodes regardless of whether they have subscribers.

- [ ] concurrent retained changes to the same topic must be ordered by monotonically increasing per-topic revisions
  committed atomically with the retained state in shared Redis. The greatest committed revision wins on every node;
  replacements and clears follow the same conflict-resolution rule.

- [x] each cluster node must share its effective subscriptions with every other node. Subscriptions from multiple clients on the same
  node to the same topic filter must be represented as one internode subscription. Changes must propagate when clients subscribe,
  unsubscribe, or their sessions expire; subscriptions belonging to offline persistent sessions must remain active.

- [ ] for non-shared subscriptions, a node receiving a publication must forward it only to nodes whose subscriptions match
  the topic, and at most once per destination node, even when multiple filters match. This is an internode transport rule;
  it does not determine recipients of shared subscriptions. A joining node must synchronize subscriptions before routing messages.

- [ ] shared subscriptions (`$share/{ShareName}/{filter}`) must select one recipient session per matching shared subscription
  across the entire cluster, not one recipient per node. Independent nodes must not assign the same publication to different
  members of that shared subscription. Recovery and reassignment must follow MQTT 5 QoS rules, as specified below.

- [ ] a node without a valid coordinator-issued lease switches to cluster-offline state, disconnects all MQTT clients,
  and stops accepting new client connections and delivering messages. It continues coordinator discovery and reconnection;
  client service resumes only after cluster connections and state have been restored and a new lease has been granted.


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

A clear stores a versioned deletion marker (tombstone), rather than simply removing all evidence of the topic's state.
The marker is internal metadata and is never delivered as a retained message. Topic revisions must not reset on clear,
node restart or coordinator succession. Deletion metadata must remain available until older updates can no longer be
replayed, so delayed publications cannot resurrect cleared retained values.

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

The coordinator maintains the agreed cluster membership and succession order, authorizes nodes to serve clients,
and coordinates recovery, including the distribution of orphaned sessions. The coordinator role is assigned automatically;
it is not permanently attached to a configured node. Normal message routing continues over the cluster mesh.

### Succession order and coordinator authority

Nodes enter the succession queue in the order in which their admission to the cluster is committed. The first eligible
node in that order is the preferred coordinator candidate. If it cannot obtain authority, the next eligible candidate
may be considered. Queue position alone never authorizes a candidate to coordinate the cluster.

The coordinator holds a time-limited lease confirmed by a majority of the agreed voting membership. It must renew
that lease regularly. Loss of a connection is not proof that the coordinator has failed: during a network partition,
both sides may consider the other side disconnected. A coordinator that cannot renew its lease must stop acting as
coordinator when the lease expires. A replacement requires majority confirmation and must not become active while
the previous coordinator's lease can still be valid.

Each transfer of coordinator authority establishes a new, monotonically increasing generation (term). Nodes reject
coordinator commands and lease renewals from older terms; a higher term alone does not prove that its sender has
majority-backed authority. Votes, terms and committed membership must survive node restarts. The lease protocol must
account for clock drift and delayed messages so that coordinator authority cannot overlap across terms.

Voting membership and succession order are agreed cluster state. Nodes must not calculate a majority from only the
peers they can currently reach or independently remove unreachable voters. Membership changes must preserve quorum
safety across the old and new configurations. A partition without a majority cannot appoint a coordinator or renew
authority to serve clients. If no partition has a majority, client service stops after the outstanding leases expire.

Once a replacement coordinator has been established, the former coordinator cannot reclaim its old position or
authority. When it rejoins, it accepts the current term and is admitted as an ordinary node at the end of the succession
queue through an agreed membership change.

### Node leases and cluster-offline state

Each serving node, including the coordinator itself, needs a valid lease authorizing client service. Nodes renew their
leases through the active coordinator. A single failed connection or heartbeat does not immediately revoke an unexpired
lease, but failure to renew before expiry requires the node to enter cluster-offline state.

Node leases must not outlive the coordinator authority under which they were issued. Coordinator handover must allow
all previously issued node leases to expire or be safely revoked before the new term begins client service. Losing the
coordinator lease therefore cannot leave the old partition serving clients under outstanding node leases.

In cluster-offline state, a node:

- Disconnects all existing MQTT clients and rejects new client connections.
- Stops client message delivery and normal cluster message routing.
- Preserves persistent session state for subsequent synchronization and recovery.
- Continues coordinator discovery, lease/succession control traffic and attempts to reconnect to the coordinator.
  Client-service shutdown must not prevent quorum participation or coordinator election.
- Retries access to the shared Redis storage if storage connectivity has been lost.

### Rejoining the cluster

Restoring a connection to the coordinator does not immediately restore client service. The node must:

1. Obtain the current coordinator term, agreed membership, succession order and shared Redis configuration from the
   authorized coordinator.
2. Confirm access to the configured logical Redis storage and cluster data namespace.
3. Connect to all active nodes listed by the coordinator and synchronize the cluster state required for service,
   including subscriptions and session ownership.
4. Resolve stale ownership and any state conflicts before routing messages or accepting clients.
5. Obtain a new client-service lease and only then enter cluster-online state.

The transition requires the connections and synchronization needed for correct cluster operations; coordinator
reachability alone is insufficient. A node that cannot complete recovery remains cluster-offline.

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
network and a burst of subscriptions goes out as a few messages. Routing still relies on each
node's `#` subscription to the others; using these sets for it, and naming `$share` filters
without their group, are the next requirement's work.

## Retained messages

Every node holds every retained message, whether or not anyone there subscribes. A change made on
a node - set, replaced or cleared - is sent to all the other nodes in its own cluster message
(`$CLUSTER/request/retained`), stamped with the cluster time (the storage clock), and a node takes
it only if it is newer than what it holds; changes made in the same millisecond are ordered by
content, so every node picks the same one. A cleared topic leaves a tombstone for 24 hours. A node
that connects to another sends it all its records, tombstones included, so a node that was down or
cut off neither misses a change nor brings a cleared message back. `$SYS` stays each node's own.
Publications forwarded between nodes carry no retained state, and the links subscribe without
retained replay, so a joining node does not re-deliver retained messages to subscribers.

## Release plan

- **0.9.20:** Introduce the cluster functionality.
- **1.0:** Release a complete, tested cluster meeting the requirements above. Coverage of all
  code in `server/Cluster` must be at least 85% by completion of cluster development.
- **1.0.1:** Develop the optional commercial cluster management and monitoring module.

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
