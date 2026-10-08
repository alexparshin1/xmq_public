# Changelog

What changed in each release, for someone deciding whether and how to upgrade. A new section is written whenever the
version number changes.

This file starts at 0.9.14. Earlier releases are not described here; their history is in the commit log.

Announcements published on the website live in `doc/news/` and are a different thing: they are written for readers who
are not upgrading anything, and most releases do not get one.

## 0.9.20 — unreleased

Requires SPTK 5.6.14.

### Changed

- **A cluster has a coordinator, and a node serves clients while it reaches Redis.** The shared
  Redis storage is the arbiter: the first node of a cluster becomes coordinator, and when it goes
  the next one in join order takes over in a new term, unseen by clients. Each node renews its own
  client-service lease there, `cluster.lease_seconds` (10 by default). A node that cannot reach
  Redis for longer than that disconnects its clients and refuses new ones as "server unavailable"
  until it can. A cluster admits at most 10 nodes.
- **Cluster nodes have a GUID and join through the shared storage.** A node makes its GUID on its
  first start and keeps it in `xmq_node.id` beside its configuration. With `cluster.enabled`, it
  joins the cluster in its Redis database by itself - finding the other nodes there - or forms one;
  a member that starts again rejoins the same way. A node may not take another node's name, a node
  running elsewhere already does not start again, and a node that is not a cluster node refuses to
  start on a database a cluster uses.
- **A session lives on one cluster node, and moves to the node its client connects to.** Redis
  records which node serves each session. A client connecting to another node takes its persistent
  session along - subscriptions and queued messages - and the node it leaves disconnects it and
  lets the session go first. A session whose node is gone is taken over the same way.
- **Cluster links require MQTT+SSL.** The reserved `cluster` account is refused on plain MQTT
  connections. A node advertises its TLS endpoint, and outgoing links use the local node's keys
  and certificate verification settings. Existing experimental cluster configurations must set
  `cluster.this_node.host_port` to the reachable TLS listener before joining.
- **Persistent clients connect without any thread waiting for Redis.** The broker looked a
  connecting client's stored session up with a synchronous request, one at a time per connection,
  and with Redis syncing every write to disk (`appendfsync always`) each answer waited for a flush.
  On a disk slow to flush the authentication queue overflowed and connections were refused - 32 000
  of 40 000 on a USB hard disk. The lookup is now asynchronous and the CONNECT is finished when the
  answer comes: no refusals on the same disk.
- **A message's persistence record waits 10 ms before it is written**, and a record whose message
  has been acknowledged by then is never written at all. It was 1 ms, and a record queued just
  before a flush was written at once, so under a burst nearly every record reached Redis; on a slow
  disk that filled `max_queued_writes` and the broker never caught up. A waiting record counts
  against `max_queued_writes`, so what a crash can lose is unchanged. With Redis syncing every
  write, 40 000 persistent messages a second now run at 159 µs on a USB hard disk, where 0.9.19
  managed 1 500 a second.

- **`xmq_server --set-password` asks for the password at a terminal**, and does not echo it,
  as `passwd` does. From a pipe it reads standard input as before.

### Removed

- **The Docker image's `XMQ_ADMIN_PASSWORD`.** It set the administrator's password on every
  start, so a password changed in the configuration interface came back after a restart, and
  it sat in plain sight in `docker inspect`. The password is set the way it is on any other
  installation, once, and kept in the `/etc/xmq` volume:
  `docker exec -it <container> xmq_server --set-password admin`, then `docker restart`.
  **A container started with `XMQ_ADMIN_PASSWORD` and no volume** has no administrator
  password after the upgrade.

### Fixed

- **On Windows, a client that failed the TLS handshake brought the broker down.** The accepted
  socket was closed twice, the second time with a C runtime call meant for files, which ends the
  process. A port scanner on the TLS port was enough. On Linux and FreeBSD the second close was
  harmless unless another connection had been given the same descriptor in between - then that
  connection was closed instead. Every accepted socket is now closed exactly once.
- **MQTT client shutdown retains callbacks still executing.** Concurrent receive and
  disconnect could clear a callback while it was running, or destroy the client before
  its receiver returned. This could crash during reconnects and bridge rebuilds.
- **RPM installation alongside SPTK.** Bundled SPTK libraries no longer create duplicate
  `/usr/lib/.build-id` links that conflict with the standalone SPTK packages.

## 0.9.19 — 2026-09-30

Requires SPTK 5.6.13.

### Changed

- **`SIGHUP` reloads instead of stopping the broker.** It shared a handler with `SIGTERM`/`SIGINT`,
  so a `SIGHUP` - the conventional "reload configuration, reopen the log" signal for a daemon - shut
  the broker down silently instead. It now re-reads extension configuration and rotates the log,
  the same operation the daily rotation performs, on demand; the process stays up, and the handler
  stays installed, so a second `SIGHUP` does another reload rather than falling through to the
  signal's default action.
- **Persistent messages are delivered on the receive thread, like all others.** They used to go through
  the delivery pool, which acknowledged the publisher before the message's record reached Redis and
  then let the pool's queue grow without limit while Redis fell behind - so a crash could lose more
  than `persistence.max_queued_writes` promised. Now the window holds: a full window pauses the
  publisher instead of queueing past it: that publisher's connection is not read until Redis catches
  up, TCP slows it down, and every other connection is read as usual. A broker with a bridge running or other cluster nodes still
  delivers through the pool, as do messages arriving from a bridge or another node: delivering into a
  bridge can wait on a remote broker, which a receive thread must not. `server_limits.delivery_threads`
  sizes the pool for those cases only.
- **Persistent Point-To-Point runs at 60 000 messages a second with sub-millisecond latency.** On the
  test bench, with Redis writing its append-only file once a second, the median is 199 µs and no
  minute averages above 1.2 ms - twice the 30 000 a second where 0.9.17 stopped. With Redis syncing every write to disk (`appendfsync always`) it holds 40 000 a
  second at 237 µs on an NVMe disk; on a disk slow to flush, persistent clients connect too slowly -
  issue #1, for 0.9.20. Three changes make up the difference:
  - SPTK 5.6.13 keeps many batches of Redis commands in flight on a connection instead of waiting for
    each batch's replies before sending the next;
  - the broker shares `persistence.max_redis_connections` Redis connections between its threads
    instead of opening one per thread - the setting was read and then ignored. It defaults to 2,
    which measured best; an existing configuration probably says 32, and is better at 2;
  - a message's record is written a millisecond after the delivery, and not at all when the
    subscriber has acknowledged it by then - which with connected subscribers is nearly always -
    so most messages cost Redis nothing, rather than a write and a delete each.
- **`persistence.max_queued_writes` defaults to 1000.** The configuration a new installation is created
  with said 0 - every message waiting for its own Redis write, which caps a broker at a few tens of
  thousands of persistent messages a second and, with delivery now on the receive thread, holds that
  thread for each write. 1000 is under 20 ms of traffic at 60 000 messages a second. **An existing
  configuration keeps the value it has**: the file is created once, on first start, and upgrades do
  not touch it. Set it to 1000 by hand to get the numbers above.
- **What a crash can lose depends on Redis as well as the broker.** `persistence.max_queued_writes`
  bounds the messages the broker has acknowledged but Redis has not yet confirmed. Redis's own setting
  decides what it loses when the machine does: with `appendfsync always` nothing, with `everysec` up
  to a second of writes, and with snapshots only (Redis's default) everything since the last snapshot.
  Run Redis with `appendonly yes` for the window to mean what it says.

### Fixed

- **A client resuming its session could crash the broker.** A client reconnecting with a persistent
  session (clean session / clean start off) whose subscriptions matched a retained message published
  with QoS 1 or 2 aborted the broker: the retained messages were sent while the session's lock was
  held, and delivering them took the same lock again. No restart was needed - an ordinary reconnect
  did it.
- **Retained messages are sent only in answer to SUBSCRIBE.** A resumed session was sent every retained
  message its restored subscriptions matched, as if it had subscribed again. MQTT sends retained
  messages when a subscription is made and at no other time.
- **Retain handling 1 ("send only if the subscription is new") now asks exactly that.** It checked
  whether a retained message had been sent on the subscription before, so a subscription made before
  anything was retained got the retained message again on every repeated SUBSCRIBE.

## 0.9.18 — 2026-09-28

Requires SPTK 5.6.12.

Point-To-Point, mostly: a broker that no longer pins itself to half the machine, a delivery pool sized
for the work instead of a constant from before anything was measured, and a message path that stopped
allocating to decide who receives a message. The broker also says, for the first time, how much work
is waiting inside it.

### Changed

- **CPU affinity is off by default.** The broker used to restrict itself to one hardware thread per
  physical core, leaving the hyperthread siblings unused. On the test bench that cap was the whole
  of a latency cliff: Point-To-Point held 0.8 ms at 100 000 messages a second and 158 ms at
  125 000, with five of eight CPUs idle - two thirds of the *permitted* CPUs were busy, which is what
  the old reading missed. Without it the same runs go 125 000 a second normally and 150 000 well,
  within 3% of FlashMQ on the same client and scenario. `--use-cpu-affinity` turns pinning back on.
  **`--no-cpu-affinity` is gone and is now rejected as an unknown option**, so a start-up script
  still passing it will stop the broker from starting.
- **The delivery pool is a setting, `server_limits.delivery_threads`, and defaults to two workers.**
  It was a fixed 16 - two per core on an eight-thread machine, so a hand-off usually had to wake a
  sleeping worker rather than find an idle one. `auto`, `0` or an absent setting means two; an
  explicit number is honoured. One worker gives the lowest Point-To-Point latency but costs the other
  scenarios; two to four is the range to choose from. It is on the Server Limits screen, and the
  start-up log says what it resolved to.
- **A delivery worker drains what is queued in one go**, up to 32 tasks per acquisition of the queue's
  lock. It never waits to fill the batch, so sparse traffic keeps the latency of a single hand-off.
- **A message is delivered on the thread that received it** when the broker runs alone: no persistent
  store, no bridge and no cluster node. It used to be handed to a delivery worker, and that hand-off was
  most of what the broker spent on a Point-To-Point message. When any of the three is present the
  delivery pool is used as before, because writing to a store or to a remote broker can block, and a
  receive thread that blocks stops reading every session it serves. A publisher's PUBACK now follows
  the delivery instead of preceding it.
- **`receive_threads` is four, and `auto` means at least four.** The receive threads now also match
  and deliver, so three left them the narrowest stage. The shipped `xmq_server.conf` says 4; a
  configuration file that already names a number keeps it.
- **Counting traffic no longer takes a lock on the message path.**
- **Six allocations per message fewer, 12.24 down to 6.24** on a Point-To-Point QoS 1 message. The
  subscription search and the per-subscription visit took their callback through
  `const std::function&`, which builds a heap copy of any lambda capturing more than sixteen bytes on
  every call; they take a two-pointer `FunctionRef` now. The list of sessions a message goes to was a
  `std::map`, a node per recipient - a vector now, reused by each delivery thread from message to
  message, merged only when a session matched twice. A message delivery and its control block are one
  allocation, and packets no longer allocate a byte when they are moved. Measured on the bench at
  150 000 messages a second, five runs a side: broker CPU 5.87 to 5.81 cores, lower in every one of
  25 pairings; latency 416 to 367 &micro;s, likely but not proven - the bench moves by more than that
  between runs of one binary.
- **The latency breakdown no longer prints nonsense when the broker was not started with
  `XMQ_LATENCY_TRACE`.** The broker stamps the moment the reactor woke a session only with that
  variable set in its own environment; without it the two phases that depend on the stamp came out as
  numbers of a few trillion microseconds, because a missing stamp was summed as an absolute time of
  zero. They now print `n/a` and say why, and every other phase is measured as before.
- **The control service runs one worker and one accept thread**, where it ran four of each. Its
  requests are an administrator's - short reads, the occasional write, a dashboard polling on a timer
  - and nothing it serves streams, so no request can hold a worker open.

### Added

- **The broker reports how much work is waiting in each of its three queues**: the session receive
  and send queues, summed over their pools, and the delivery queue. Sampled once a second by the
  existing metrics scan, never on the message path, because reading a queue's size takes the lock its
  push and pop take. They are on `$SYS/broker/queues/{receive,send,delivery}/length`, in
  `GetStatistics` as `queue_receive`, `queue_send` and `queue_delivery`, and in a Queues panel on the
  dashboard. The counters said what the broker had done; these say whether it is keeping up.
- **`xmq_scn` says so when its source addresses are on a tunnel.** A load client bound to a VPN
  interface measures the tunnel, not the broker, and the runs it produces look like a slow server
  rather than a misdirected client. It now names the interface it will send from and warns when
  that interface is a tunnel (`tun`, `tap`, `wg`, `nordlynx`, `ppp`, `utun`).
- **The log says which signal stopped the broker**: `Received SIGTERM (15), stopping the server.`,
  the line before `Server stopped.` A clean stop with no reason beside it reads as the broker
  deciding to quit - on an AWS campaign three restarts by `needrestart`, during unattended upgrades,
  looked exactly like that until the system journal was read.

### Build

- **Unit tests are kept out of the production install.**
- **React dependencies install one at a time**, so parallel builds stop racing each other in
  `node_modules`.

## 0.9.17 — 2026-09-18

Requires SPTK 5.6.11.

MQTT 5 properties, mostly: what the broker tells a client about itself, what it keeps of what a
client tells it, and what it costs to carry. The thread counts stop being a number somebody has to
know, and the load tools learn to send properties so the two can be measured against each other.

### Added

- **`send_threads` and `receive_threads` accept `auto`.** The broker counts the physical cores and
  picks from them, so a configuration no longer carries a number chosen for somebody else's
  machine. `0` means the same thing, and an explicit number is still honoured. The template shipped
  8 and 8; on a 16-vCPU AWS instance the measured optimum is 3, and on the same instance a Fan-Out
  run at 8 send threads and one at 3 differ by 3175 against 1909&micro;s. The startup log says what
  it resolved to and what it counted.
- **`xmq_pub` and `xmq_scn` send message properties**, with `-D connect <name> <value>` and
  `-D publish <name> <value>` naming the packet each belongs to. A user property is a pair, written
  `-D publish user-property name=value`.
- **A scenario declares its own properties**, in `connect_properties` and `publish_properties` on a
  client group. Both are lists, because MQTT 5 lets a user property repeat and keeps its order. The
  command line wins where the two name the same property.
- **The scenario-set runner records what the broker cost**, not only what the client saw:
  `--stats-host` names the machine it runs on and `--stats-process` the process to watch, so the
  same runner measures a rival broker the same way. Mean and peak CPU and peak RSS go into each
  scenario's block.
- **The broker warns at start-up about a network card that will cap it.** When an interface its
  listeners use has a single receive queue, no receive packet steering, and the machine has more than
  two CPUs, the log says so and links the user manual's new section on enabling RPS. Such a card runs
  the receive path of every client on one CPU; on the test bench it dropped 255 891 frames a second
  at 100 000 messages a second, and clients were closed for keep-alive timeouts although they were
  sending. With RPS the drops went to zero. Linux only; the manual covers FreeBSD and Windows.
- **The broker and the load tools can choose their CPUs when asked.** On
  a machine with cores of different speeds, or with hyper-threading, the startup line now says what
  was picked and why - `4 of 8 permitted hardware threads (CPUs 0,1,2,3): the 4 fastest cores
  (5100 MHz against 3700)`. `xmq_scn` does the same, because a load client that wanders across
  efficiency cores measures its own scheduling as latency. CPU affinity is off by default;
  `--use-cpu-affinity` turns it on in both programs, and the option exists only where the system can
  honour it. It was worth 4-7% on the hybrid bench client, while on a c5n.4xlarge, whose sixteen
  vCPUs are identical, it was worth 0.8%.

### Changed

- **Message properties are held as MQTT 5 wire bytes and an index**, in place of two hash maps and
  a map of user properties. Reading a message copies the block and walks it once, decoding nothing
  until it is asked for; writing copies it back out. A set that fits inside the message - which is
  nearly all of them - costs no allocation at all, where six properties used to cost seven
  allocations to build and seven frees to destroy.

  Measured on the bench, 500 000 MQTT 5 connections: 45.1 allocations per connection against 38.1,
  and 7422 bytes against 7222. On a 50 000 message/s Point-To-Point run carrying three properties
  per message: 25.3% fewer allocations, 22.5% fewer bytes, and 2.5% less broker CPU. Latency is
  unchanged, and on a delivery-heavy Fan-Out run so is CPU.
- **The package carries its SPTK privately and loads exactly that release.** The bundled SPTK
  libraries used to go into the prefix's shared `lib/`. There they replaced the SPTK of every other
  program on the machine, and they collided with SPTK's own packages, so the two could not be
  installed together. They now live in `lib/xmq-sptk`, which no search path names except XMQ's own.
  SPTK's soname carries its whole version (`libsputil5.so.5.6.11`), and every installed program finds
  its libraries through an RPATH that `LD_LIBRARY_PATH` cannot override. The utilities, which had no
  SPTK path at all, get the same RPATH. The build accepts only the SPTK release it names, so a
  broker cannot start against a different one. Each package is inspected for both at build time.
- **A session's queue of waiting messages is built when the first message waits.** Every session
  carried one from the moment it connected, and an empty `std::deque` is not free: it allocates its
  map and a first block as it is constructed. Most sessions never fill their in-flight window at
  all. Measured on 500 000 connections: exactly two allocations and 648 bytes per connection fewer,
  and 657 bytes less live heap per connection under massif. Broker CPU is unchanged.
- **A connected session holds 900 bytes less.** Each session's outgoing buffer used to start at
  1024 bytes, whether or not the client was ever sent anything past its CONNACK. It now starts at
  128 bytes. A buffer that a busy flow grows keeps its size: the send thread swaps buffers with
  the session rather than freeing them. Measured on the bench with 500 000 idle MQTT 5
  connections: peak RSS fell from 2386 to 1958 MB (17.9%). A 100 000 message/s Point-To-Point
  run used 15% less memory at the same broker CPU, and no scenario got slower.
- **`xmq_scn` registers its sockets edge-triggered**, as the broker already did, and no longer
  re-arms each socket with an `epoll_ctl` after every read. The load generator costs 14% less CPU,
  all of it kernel time. Windows stays one-shot.
- **`xmq_scn` sends a keep-alive ping only when it has sent nothing else**, as MQTT intends. A
  session publishing once a second no longer pings on schedule as well: 50 000 such sessions sent
  202 455 pings in two minutes before, and none now. The keep-alive timer also stopped taking the
  session's lock, which had queued it behind the busiest session in the process.
- **A session waits on one timer event instead of four.** Waiting for a CONNECT that never came,
  the keep-alive, the session's own expiry and the expiry of the messages it holds were four events
  per session; they are now one, armed for whichever deadline comes first. A session also knows what
  it is waiting for - `Accepted`, `Authenticating`, `Active`, `Detached`, `Gone` - so the deadline
  follows from the state instead of being searched for. Message expiry deliberately sits outside
  that: when a queued message dies has nothing to do with what the session is doing.
- **A connected session costs about half of what it did.** The CONNECT packet's parameters are kept
  as three atomics and a pointer rather than the whole parsed object; the names a session shares
  with others - its account, its node - are interned and held once for all of them; a session points
  at its protocol instead of carrying three pointers copied out of it; and a mutex that guarded two
  bytes and a string that could be rebuilt on demand are gone. `sizeof(ClientSession)` went from
  1208 to 952 bytes, and a connection measured on the bench at 500 000 sessions from 4956 to about
  2665. At a million connections on AWS the broker's peak resident memory is 2521 Mb, against 4790
  recorded for 0.9.16.

### Removed

- **`min_keep_alive_seconds`.** It let a broker override what a client asked for, which MQTT gives
  it no standing to do: the only sanctioned way to impose a floor is the Server Keep Alive property
  of MQTT 5, and a v3 client cannot be told at all. A configuration that still carries the setting
  loads and runs; the key is ignored.

### Fixed

- **The CONNACK reported the client's own limits back to it as the server's.** The broker walked
  every integer property of a CONNECT and copied each into the CONNACK on the way past, so a client
  asking for a Receive Maximum of 10 was told the server accepts 10 - and throttled itself to ten
  messages in flight where 32768 were available. The same for Maximum Packet Size and Topic Alias
  Maximum. All three describe the server, and are now written in one place from its settings.
- **A repeated user property is no longer dropped.** MQTT 5 allows a name to appear more than once
  and its order is significant; the map the broker kept them in silently discarded all but the
  first.
- **`-D` never worked.** It collected the same word three times, so `-D connect receive-maximum 10`
  became the property `connect:connect=connect`, and then rejected the three arguments it had not
  consumed.
- **Properties asked for on MQTT 3 refuse the run** rather than being dropped in silence. A
  measurement that quietly sends none of what it was told to send is worse than no measurement.
- **Text set on a property declared to hold a number is refused.** It used to be accepted and
  written where a number belonged.
- **The rpm packages no longer declare the configuration templates and the systemd units as
  configuration files.** CPack marked everything installed to an absolute path `%config`, so rpm
  took ownership of files the broker and the operator write, where the .deb and the FreeBSD
  package claimed none. `xmq_extensions.conf.template` is also installed readable by root and its
  group only, like the server template beside it; every 0.9.16 package made it readable by all.
  Each package is now inspected as it is built, against the previous release's file list, and a
  package that would do either is not published.

### Measured

The set on AWS, one c5n.4xlarge for the broker and one for the load client, against the same set
measured on the same pair for 0.9.16. Interval medians, first and last row dropped:

| Scenario | 0.9.16 | 0.9.17 | |
|---|---|---|---|
| Fan-Out 250K, 5 minutes | 1909&micro;s | 1738&micro;s | -9.0% |
| Fan-In 50K | 217&micro;s | 212&micro;s | -2.3% |
| Point-To-Point 50K | 213&micro;s | 209&micro;s | -1.9% |
| Point-To-Point 30K, persistent | 188&micro;s | 186&micro;s | -1.1% |
| 500K connections | 311&micro;s | 283&micro;s | -9.0% |
| 1M connections | 350&micro;s | 307&micro;s | -12.3% |

**The client is not the same client.** 0.9.16 was measured with the 0.9.16 `xmq_scn`, and this
release changed that client too - edge-triggered sockets, a keep-alive ping only when something else
has not been sent, and the core pinning above. There is no way to divide the gain between the two
ends from these numbers, and no run separates them: what the table says is that the pair of them,
which is what a user has, does this work faster than the pair before it. The Fan-In row compares an
interval median against an average, because the 0.9.16 record for that scenario predates the median
being recorded; its intervals ran 215-218&micro;s, so the two are close enough to put side by side
and not close enough to argue about.

## 0.9.16 — 2026-09-10

Requires SPTK 5.6.10.

Two things: who is allowed to connect, and what the broker does when a great many of them arrive at
once. Accounts move out of a file and into a database, authentication becomes an extension like any
other, and the tables the broker keeps one entry per connection in stop rebuilding themselves as
they fill.

### Added

- **Accounts and groups live in a SQL database.** SQLite by default, beside the configuration;
  PostgreSQL or MySQL when the broker is pointed at one. Groups and membership come with them, and
  the Users screen of the configuration interface edits both. An existing `xmq_users.conf` is
  imported on the first start after the upgrade, so nothing has to be retyped.
- **Extensions, through a documented C interface.** An extension can authenticate a client,
  authorise a subscription or a publication, or observe what the broker does, and it declares which
  of those it means to do rather than being asked to guess. The header a third party builds against
  is the one XMQ's own `user-database` extension is built against; the Extensions screen lists what
  is loaded, what it declared and what it last refused. ABI 1.8 also hands an extension the address
  of the account database, so the interface and the authenticator cannot be pointed at two
  different stores.
- **`--set-password` sets an account's password from the command line**, reading it from standard
  input so it never appears in the process list or the shell history. A server whose administrator
  has no password yet serves its configuration interface on the loopback address only, which is
  right on a machine with a console and unusable without one: on a headless host it meant an SSH
  tunnel, and in a container it meant nothing worked, because a container's loopback is not the
  host's and a published port reaches no listener. The password that opens the interface could
  only be set through the interface.
- **The container image sets that password from `XMQ_ADMIN_PASSWORD`**, and refuses to start when
  it cannot, rather than bringing up a broker nobody can administer. With the variable unset the
  behaviour is unchanged: MQTT works and the interface answers inside the container only.
- **The scenario engine runs over TLS**, with `--encrypted` on `xmq_scn`, so the encrypted listener
  can be put under the same load as the plain one.

### Changed

- **Authentication goes through the extension interface.** The broker no longer has an
  authenticator of its own: `user-database` ships with it, enabled and required. A broker whose
  required authenticator fails to start serves its configuration interface and no MQTT at all,
  rather than admitting everybody.
- **The per-connection tables are sized once, from the file-descriptor limit.** There were three of
  them - two in the broker's session manager and one in the reactor's registration - and each
  rebuilt itself as it filled, on the client side as well as the server's. On a 16-vCPU AWS
  instance a million sessions now connect at a median of 346&micro;s against 612&micro;s, with every
  interval between 323 and 362&micro;s and none of the three periodic stalls left; half a million
  connect at 305&micro;s against 378&micro;s. A million sessions occupy 4.8 Gb.
- **The authentication queue is sized from the thread count** instead of a constant. At 8192 it
  turned away 6562 sessions of a 20000-session burst. Session writes on the connect path no longer
  block the authentication thread either, and the Redis storage no longer connects while holding
  its lookup mutex.
- **The read path no longer asks `FIONREAD` before every read.** At 250k messages a second that was
  112659 ioctls a second - more than the reads they sized, because a pass that finds nothing pays
  too. Both the broker and `xmq_scn` were changed.
- **A package carries templates, not live configuration files.** The broker writes
  `xmq_server.conf` and an extension's fragment from the templates beside them when it finds none,
  so installing a new version cannot write over settings that have been answered - including
  whether an extension is switched on. Removing a package leaves them, which also means a complete
  removal is now a manual one.
- **Release builds keep frame pointers**, so `perf` can unwind one without a debug build.

### Security

- **Passwords are stored as one-way verifiers.** The broker can check a password; nothing, the
  broker included, can read one back. The cluster secret moved to where a secret that is presented
  rather than verified belongs.
- **The SQLite account store is not readable by every account on the machine.** SQLite creates a
  file with whatever the umask allows, and this one holds a verifier per account; it is now 0640,
  like the configuration beside it.

### Fixed

- **A TLS session could stop delivering under load.** `SSL_read` returns one TLS record, and a
  short read was taken for a drained socket - so a session sent several messages at once stopped
  after the first. Under a 500-message test, 4 arrived.
- **An abruptly closed TLS connection broke the next one.** OpenSSL's error queue is per thread and
  was never cleared, so one connection's failure was reported against the next session the same
  broker thread served.
- **A reconnect's own teardown could erase the session it had just made**, and the shutdown path
  could destroy a session's queues while its threads still read them - silent on libstdc++, an
  illegal instruction on libc++.
- **The FreeBSD package named its extension at the build machine's path.** CPack does not stage
  through `DESTDIR`, so the configure-time prefix written into the installed configuration was a
  directory inside the build tree. The installed broker started, served its interface, and refused
  MQTT.
- **The FreeBSD service refused to start without a configuration file**, which is the state every
  fresh installation is in now that the package ships templates only.
- **An extension DLL is installed where the configuration says it is.** On Windows a DLL is a
  runtime artifact, so it had been landing in `bin` while the configuration named
  `lib/xmq-extensions`; the broker could not load its own authenticator and served no MQTT.
- **Saving an account or a membership tells the extensions to forget what they cached**, so a
  changed password takes effect at once instead of within a minute.

## 0.9.15 — 2026-08-30

Requires SPTK 5.6.9.

An installation-and-platforms release. Nothing changes in how the broker moves messages; almost everything here is about
where XMQ runs, who it runs as, and what it looks like on a machine nobody has configured yet.

### Added

- **FreeBSD is a supported platform.** XMQ builds with the base system's Clang, passes its test suite, and ships as a
  native `pkg(8)` package with an `rc.d` service. The package is self-contained — six files, 8 MB, no ports tree and no
  compiler needed on the target machine.
- **Log rotation, with retention.** The log is rotated daily and a week of archives is kept; `logs_to_keep` in the
  configuration sets the number, and zero or less keeps everything. Until now the log grew for as long as the broker ran,
  on every platform. Windows had no answer to this at all, since the usual external rotation tools are not there.

### Changed

- **The Windows installer no longer needs a network connection.** It is a bootstrapper carrying the Visual C++
  redistributable, rather than an MSI that downloaded one at install time.
- **The broker no longer puts itself into the background.** systemd and `rc.d` supervise it directly, which is what both
  expect. `--console` remains only on Windows, where it selects between service and console; `--terminal` is gone.
- **Each published package has a `.sha256` beside it**, generated when the package is copied to the download area.

### Security

- **Nothing ships with a password any more.** The administrator password is asked for at first start, and until it is
  set the web interface answers only on the loopback address, so an unconfigured broker cannot be adopted over the
  network. The cluster account's password is generated during installation instead of being a constant anyone could
  read in the documentation.
- **The service runs as its own unprivileged account.** The package creates `xmq` and gives it the configuration,
  certificate and log directories, mode 0750. Before this the broker ran as root, which a process listening on a
  network has no reason to do.

### Fixed

- **A subscription could be lost on reconnect.** When a client reconnected while its previous session was still being
  torn down, a write that failed on the old connection closed whichever connection the session was holding at that
  moment — which could be the new one, taking the subscription just made with it. The close now names the connection it
  belongs to.
- **A graceful disconnect could still publish the Last Will.** The disconnect was recorded after the socket closed, so
  the thread handling the close could get there first and treat it as a broken connection.
- **One silent connection could stop a listener accepting.** The TLS handshake ran on the accept thread, so a client
  that connected and then said nothing held the whole interface until it timed out.
- **Setup no longer offers a Redis nobody asked for**, and refuses a configuration naming a Redis that is not reachable
  rather than accepting it and failing at the next start.

## 0.9.14 — 2026-08-20

Requires SPTK 5.6.8.

### License

- **XMQ is now under the [Mozilla Public License 2.0](https://mozilla.org/MPL/2.0/)**, replacing GPL-2.0. Under the GPL,
  a product linking XMQ had to be released under the GPL too, which for a component embedded in appliances and
  industrial systems meant it was not used. The MPL's copyleft is per file: changes to XMQ's own sources stay open, a
  larger work combining them may carry its own terms.
- **SPTK 5.6.8 adds a linking exception** to its LGPL, so linking it — statically included — no longer brings the
  relinking obligation of LGPL section 6. SPTK itself remains LGPL. Its license text, which had never been distributed
  with the library although the LGPL requires it, is now in
  `COPYING.LGPL`.

Nothing about the running broker changes. If the GPL was why you could not use XMQ, that reason is gone.

### Fixed

- **Retained messages survive a restart.** They are held per topic and persisted, rather than kept on a subscription
  object. This also fixes a wildcard subscription receiving only some of the retained messages it matched, and `$SYS`
  retained values are correctly excluded from persistence.
- **`xmq_sub` can subscribe to wildcards.** It rejected `#` and `+` while validating its own arguments, before any
  packet was sent, so the shipped subscriber could not do the one thing a subscription is for.
- **The interface is built from the versions it was tested against.** `package-lock.json` was never copied into the
  build directory, so every build resolved dependencies afresh and could fail on a dependency nobody had edited. As a
  side effect the interface was rebuilt on every build of the project; it is now rebuilt only when it changes.

### Changed

- **Point-to-point latency roughly halved.** On 16 vCPU the broker's own share of the path fell from 64 to 30
  microseconds and the end-to-end figure from 296 to 227, using 11% less CPU. The batching round is now taken by the
  sending workers alone; the receiving side, which has no use for one, was waiting behind it.
- **`send_threads` and `receive_threads` configuration settings default to 3**, replacing 8. On 16 vCPU the old default
  was the worst measured point, 56% behind. The optimum tracks the inbound packet rate rather than the core count, so a
  value suiting one workload can be wrong for another — the setup documentation carries the measurements.
- **io_uring has been removed.** A complete socket pool built on it matched epoll to within oneper cent, and every
  tuning option either lost or had an epoll equivalent that performed identically. The event mechanism is 6% of the
  broker's system calls while thread handoffs are 67%, so there was nothing there to win. epoll remains, and liburing is
  no longer a build dependency.

### Added

- **A per-hop latency breakdown.** Set `XMQ_LATENCY_TRACE` and run an MQTT 5 scenario to see where time goes inside the
  broker: reactor to read, decode, handler, delivery, socket write. Every phase in the broker's own total is stamped by
  the broker, so it stays meaningful when the client's clock disagrees.
- **An official Docker image**, keeping configuration and stored state across container restarts.
- **An extension interface** — a C ABI for loading shared libraries that observe broker events, decide whether a client
  may connect, or decide what a connected client may publish and subscribe to. **Not stable, not announced, and subject
  to change without notice**; it is listed here for completeness, not as something to build against yet. See
  `extension/README.md`.
- **Topic permissions in the extension ABI (1.2).** An extension places a client in a group when it connects, and every
  permission after that is a property of the group and the topic — never of the individual client. The broker caches
  each answer under that pair, so clients in one group share it: measured at about 8 ns per message on a warm cache,
  scaling with cores rather than contending. Rules per client would make the cache key the client and cost a call into
  the extension on every message. A broker with no such extension loaded is unaffected — the check is a null pointer
  test.
- **`XMQ_ADD_EXTENSION`**, installed to `share/xmq/cmake`, builds an extension against the ABI headers and nothing else
  of the broker, whether the sources sit in this tree or in a repository of their own.
