# XMQ 1.1.1 — tasks

Work planned for 1.1.1, in the order it should be done. An item leaves this file when it is
released, not when it is coded: the changelog records what shipped, this file records what is
still owed.

## Planned

- [ ] **Regression test for the fan-out shard mapping.**
  `SubscriptionManager::shardOf()` now mixes the session address before folding it, because
  sessions are allocated at a stride that made the plain address fold to one shard for every
  subscriber. The test should allocate a thousand objects of one size class, take their addresses
  as the mapping's input, and assert that every shard gets members: the old mapping put all of
  them in one, and that is exactly what a future simplification would reintroduce.

- [x] **Fix the two failing jest suites.**
  Done in 1.1.1: the screens were right and the expectations were stale. `gui-react` asks for the
  Redis database explicitly, in a field of its own, and names it back in what it sends and in
  what it says it checked, so `redis://cache:...@theater:6379/0` and
  `Redis at theater:6380/0 is reachable.` stay. The suite is green: 116 passed.

- [ ] **Paired stand run of the bench set, baseline against the fixed delivery sharding.**
  The shard fix was measured on Fan-Out alone (broker share 1.45 ms → 0.95 ms at two delivery
  threads, → 0.76 ms at four). The other four scenarios have not been re-run against it. No
  change is expected in them — their fan-outs are single recipients or shared subscriptions — but
  the run should say so rather than assume it.

- [ ] **The load client, 2026-11-08: it is not the bottleneck the bench implied, and its measured
      costs are now written down.**  Profiling `xmq_scn` under Fan-Out (1000 subscribers, 227k deliveries/s) revised the
  2026-11-07 conclusion. The client is not CPU saturated - eight session threads at 20–30%,
  the machine 45% idle - and its latency does not fall when threads are added: session threads
  8/16/32 measured 1886/2024/2247 µs, socket-event pollers 1/2/4 measured within noise of each
  other. Fan-Out latency is linear in the publication's copy count (~1.5 µs per copy) and
  independent of the offered rate, so it is the serialized cost of delivering one publication,
  shared between the broker's send stage and the client's per-packet work, not a client thread
  waiting on anything.
  - The etalon run of this investigation: `/tmp/opencode/` sweeps of 2026-11-08 (threads, pollers,
    subscriber counts, rates, ack batches). Per-copy end-to-end cost ~1.9 µs at a thousand
    subscribers; broker-stamped share ~0.95 µs of it.
  - What did move: batching acknowledgements, one write per 16 acks instead of one per
    acknowledgement, gave Fan-Out −4.5% (1887 → 1802 µs) and P2P −10% (240 → 216 µs), −3.9% at
    four. That is the size the ack path is: one write syscall per delivered message, and on the
    stand's P2P the kernels stop carrying one hundred thousand one-segment writes a second the
    tool itself made. It is in `xmq_scn` behind `XMQ_CLIENT_ACK_BATCH`, default 1 (unchanged
    behaviour), force-flushed on disconnect.

- [ ] **Measure the client's latency accounting with an array, not a tree.** Coded 2026-11-08;
  leaves when 1.1.1 is released.
  `addToHistogram` inserted into a `std::map` twice per delivered message (interval and total),
  at 2.48% of all samples on the machine, ~35% of the client's userspace CPU, on the very path
  whose latency it reports. The two hot histograms now count into dense vectors whose element
  per bucket index is exactly the key the map folded, with the ranges seen stored sparse (the
  interval and combined copies) as before. Same medians where measured; the cost fell to 0.16%
  of all samples.

- [ ] **The send queue is not the P2P slowdown: measured, 2026-11-08.**
  Branch `2026-11-08-send-optimisation` built the alternative - a publication that matched one or
  two sessions writes its sockets on the delivering thread instead of waking a send worker for
  each (`XMQ_INLINE_SEND_MAX`, `MessageDispatch::m_inlineSend`, one detach-and-write mutex in
  `SocketWriteBuffer::send()`). Stand P2P, 100k deliveries/s, paired against 1.1.1:
  - inline send on: **429-625 us** against 205-226 - the write moved onto the thread that reads;
  - inline send off with the send worker rewritten to one `send()` per session: **542-579 us** -
    the batch shape the worker had (detach a round, then write it) is worth more than the hop it
    looks like it costs;
  - inline send off and the worker's 1.1.1 shape restored: **204/213 us** against 200/218 - the
    rest of the branch (the dispatch flag, the send mutex, the instrument) is neutral.
  Conclusion: the 8-10% P2P gap to FlashMQ is not in the send queue's hand-off. The remaining
  candidates are per-message userspace costs on the delivery path (topic lookup, queue locks,
  `shared_ptr` traffic) and the broker's receive path, all of which the profile shows as a long
  tail of 0.1-0.4% symbols rather than one hotspot.
  One of them was tried on the same day: the destination topic a PUBLISH names was resolved
  through `TopicManager` (shared lock plus hash) on every message, 0.26% of every cycle, and now
  a per-session `PublishMessage::TopicCache` answers the repeat - a publisher sends to its own
  topic. `getTopic` left the profile (latency 203/205 against 203/211, i.e. unchanged: it was
  0.24 us of a 200 us median), which sizes the rest: one such item at a time cannot reach the
  eight to ten per cent, because the broker's userspace is 6.6 us per delivery and the kernel's
  per-packet work is 84% of the samples on the broker's host. The gap to FlashMQ has to come
  from fewer packets or fewer wake-ups per delivery, not from cheaper userspace per message.
  The branch is kept for the instrument: with `XMQ_SEND_DIAG` set, the broker prints the queued
  wait per second, and the inline knob remains for a rig where a direct write is the faster
  shape.

- [x] **Refuse to start when a configured listener is already in use.**
  Done in 1.1.1: `common/ListenerPort.cpp` probes the configured address and port with a bind
  before any socket of this broker exists; `Server::createMqttListeners()`,
  `ServerController::startService()` and `ControlServiceListener::factory()` call it, and
  `XMQ_ControlServiceTests.startServiceRefusesListenerHeldByAnotherBroker` covers a second broker
  with the same configuration and a different node name. The same probe is used before the
  Prometheus endpoint is replaced.
  A broker must not come up on a listener that another process on the same host already serves.
  Today SPTK opens the listening sockets with `SO_REUSEPORT`, so a second broker binds the same
  address and port successfully, and the kernel then spreads incoming connections across both
  processes: publishers and subscribers of one run land on different brokers, messages are lost,
  and latency figures become meaningless while every log looks healthy.
  - Check every enabled listener in the configuration before creating it — MQTT, MQTT+SSL and the
    web/control listener, each on its configured bind address and port, not only the default 1883.
  - Do the check before `addListener()`, so no socket of this broker is open yet and a refusal
    leaves nothing serving.
  - Fail with a startup error naming the listener, its bind address and port, and a non-zero exit
    code. Do not fall back to sharing the port or to a different port.
  - Keep the check portable: the Windows build has no `SO_REUSEPORT`, but it also must not start
    on a busy listener.
  - Test: start a broker on a port, start a second broker with the same configuration, expect the
    second to exit with the error naming the port. Cover a second broker that differs only in its
    node name, since that is the case that used to go unnoticed.
