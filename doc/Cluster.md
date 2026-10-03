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
[-] any connections between cluster nodes are MQTT+SSL (encrypted).
[-] an external load balancer can connect a client to any node in the cluster.
[-] a node can be connected and disconnected to/from the cluster. When a node is disconnected (or crashed), one or more of the cluster
  nodes should load its sessions and be ready to migrate them to the clients re-connecting from disconnected node.
[x] retained messages, including replacements and clears, must be applied on all nodes regardless of whether they have subscribers.
[x] each cluster node must share its effective subscriptions with every other node. Subscriptions from multiple clients on the same
  node to the same topic filter must be represented as one internode subscription. Changes must propagate when clients subscribe,
  unsubscribe, or their sessions expire; subscriptions belonging to offline persistent sessions must remain active.
[-] a node receiving a publication must forward it only to nodes whose subscriptions match the topic, and at most once per destination
  node, even when multiple filters match. A joining node must synchronize subscriptions before it begins normal message routing.
