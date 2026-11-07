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

- [ ] **Paired stand run of the bench set, baseline against the fixed delivery sharding.**
  The shard fix was measured on Fan-Out alone (broker share 1.45 ms → 0.95 ms at two delivery
  threads, → 0.76 ms at four). The other four scenarios have not been re-run against it. No
  change is expected in them — their fan-outs are single recipients or shared subscriptions — but
  the run should say so rather than assume it.

- [ ] **The load client is the limiting stage on the stand; measure it, then use it.**
  The broker's own latency trace puts 33 µs of P2P's 231 µs and 0.76–0.95 ms of Fan-Out's
  2.24 ms inside the broker; the rest is publishing, the wire and the subscriber side of
  `xmq_scn` on thinker10, which saturates at 231% CPU with 1000 subscriber connections. Until the
  client is faster, no broker-side improvement can show the ten per cent the bench-set acceptance
  asked for. This is tomorrow's work; it also speeds up the comparison runs against FlashMQ.

- [ ] **Refuse to start when a configured listener is already in use.**
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
