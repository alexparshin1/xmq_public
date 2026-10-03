# XMQ cluster should support:
[-] the general approach here: a cluster is a mesh of nodes, without a fixed ten-node limit.
[-] from the outside, cluster behaves as a very large broker. If there is an external load balancer in the front,
  the illusion should be complete.
[-] client id is unique within the cluster. For sequential connections using the same Client ID, the later connection takes over the
  session and the previous connection is closed. If connections with the same Client ID arrive concurrently at different nodes,
  either one connection survives or both may be disconnected. After conflict resolution, two active owners of the session must not
  remain, and its persistent state must not be corrupted. Clients may retry.
[-] there is no assigned cluster coordinator node. If a cluster coordinator node is needed, for instance - to take
  over the dead node, it is automatically elected from the online node. I.e. it's the node with higher uptime.
[-] cluster sessions are persistent and must survive the reboot of any node. If a node goes down, the online node(s) should
  take over orphaned sessions. A coordinator node may distribute the orphaned sessions between online nodes.
[-] any MQTT client can connect to arbitrary node of the cluster. The cluster automatically migrates the session
  from its previous owner to the node to which the client has connected.
[x] any connections between cluster nodes are MQTT+SSL (encrypted).
[-] an external load balancer can connect a client to any node in the cluster.
[-] a node can be connected and disconnected to/from the cluster. When a node is disconnected (or crashed), one or more of the cluster
  nodes should load its sessions and be ready to migrate them to the clients re-connecting from disconnected node.
[x] retained messages, including replacements and clears, must be applied on all nodes regardless of whether they have subscribers.
[x] each cluster node must share its effective subscriptions with every other node. Subscriptions from multiple clients on the same
  node to the same topic filter must be represented as one internode subscription. Changes must propagate when clients subscribe,
  unsubscribe, or their sessions expire; subscriptions belonging to offline persistent sessions must remain active.
[-] a node receiving a publication must forward it only to nodes whose subscriptions match the topic, and at most once per destination
  node, even when multiple filters match. A joining node must synchronize subscriptions before it begins normal message routing.

## Cluster transport

Cluster peers connect to MQTT+SSL listeners. `cluster.this_node.host_port` must advertise a
reachable TLS endpoint (normally port 8883); the initial join also takes a peer's TLS endpoint.
Existing configurations that advertise the plain MQTT port must be updated before joining.
An explicit request for an unencrypted join, or a discovered peer record with `encrypted: false`,
is rejected. The reserved `cluster` account is admitted only over TLS; ordinary MQTT clients
can continue using the plain listener, including taking over their previous TLS connection
with the same Client ID. The transport requirement applies to internode links.

Outgoing links use the local node's `connections.ssl_keys`, including its certificate and trust
store, rather than certificate paths advertised by another machine. Certificate verification
follows `verify_depth`: zero encrypts the connection without verifying the peer certificate;
a positive value enables verification against the configured CA. Encryption is mandatory.

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
