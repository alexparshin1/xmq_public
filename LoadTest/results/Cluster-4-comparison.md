# Four-node cluster: fan-in, fan-out and point-to-point

Home stand, 26 October 2026. Four broker nodes, two on each of `theater` and `thinker11`
(eight vCPU each); the load generator is `thinker10`, eight vCPU, bound to 51 source
addresses in `10.1.1.150/24`. XMQ 1.0.0 at `3f25071`, EMQX 5.8.9 open source. Every
scenario puts the publisher and subscriber groups on different nodes, so all traffic
crosses a cluster link.

Not a published performance result: each broker host runs two nodes on eight cores, so
both sides carry host contention.

| Shape | Target msg/s | EMQX msg/s | EMQX delivered | EMQX median | XMQ msg/s | XMQ delivered | XMQ median |
|---|---:|---:|---:|---:|---:|---:|---:|
| Fan-In | 50,000 | 15,403 | 9,241,861 (30.8%) | 244.4 s | 49,995 | 29,997,003 (100.0%) | 3.7 ms |
| Fan-Out | 250,000 | 126,527 | 37,958,318 (50.6%) | 53.1 s | 249,991 | 74,997,387 (100.0%) | 3.4 ms |
| P2P | 100,000 | 29,440 | 17,664,418 (29.4%) | 240.3 s | 88,990 | 53,394,342 (89.0%) | 32.6 s |

`delivered` is the number of deliveries the scenario completed in its fixed window
(fan-out counts one publication once per subscriber), against the number the target rate
would produce. All six runs exited 0, all clients connected, and every run moved traffic
between nodes.

## What the numbers do and do not say

- **EMQX's fan-out is capped by its own setting.** This stand sets `mqtt.max_inflight`
  to 128 per session. With 1,000 subscribers that is about 128k deliveries in flight,
  and the measured 126.5k/s matches that ceiling; it is not necessarily the broker's
  limit. XMQ's scenario allows 1,024 in flight per client and its session queue allows
  32,768. Raising EMQX's limit and repeating the run would be the like-for-like test.
- **Fan-in and point-to-point on EMQX were backlogged, not merely slower.** Median
  latencies of 244 s and 240 s mean the fixed window ended with queues still draining;
  the delivered count is what completed, not what was published. XMQ's fan-in finished
  all 30M deliveries at the target rate; its point-to-point run delivered 89% and its
  latency was rising at the end, so it too was beginning to fall behind.
- **Authentication differs.** XMQ required an account on every client connection
  (PostgreSQL back end, PBKDF2); EMQX ran with authentication disabled, as the stand
  was configured. That affects the connect phase, not publish latency. The EMQX account
  `user`/`User-Secret1` was created and verified separately with `xmq_pub`; anonymous
  was refused on an authentication-enabled listener. Enabling authentication on the
  main 1884/1885 listeners was not done: without a restart EMQX would have had to rebind
  both nodes of a host to the same port, and the run was already measured with the
  stand as configured.
- **Connections.** All 50,000 publishers (fan-in), 1,000 subscribers (fan-out) and
  50,000+50,000 clients (point-to-point) connected in every run.

Scenarios: `LoadTest/Cluster-4-*.json` (XMQ) and `LoadTest/emqx/Cluster-4-*.json`
(EMQX). Raw consoles sit beside each record; the run log is `Cluster-4-run.txt`.
