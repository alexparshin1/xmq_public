# The test tools as a product (1.1.0)

The tools XMQ is measured with - the load engine, the scenario files, the cluster orchestrator with
its timeline and its checks - are useful to anyone running an MQTT broker, and the market is weak
exactly where they are strong. The common tools (`emqtt_bench`, `mqtt-bench`, JMeter, Gatling,
`xk6-mqtt`, and a bench of its own in most vendors) are about one broker and one stream of figures.
None of them takes a node away in the middle of a run and then says what should have happened.

The requirements, as stated: installing it must not require XMQ; it must work against any MQTT
broker; and it must work against any MQTT cluster and bridge.

What makes those reachable, and why the work is smaller than it sounds:

- the client at the base of all of it is an ordinary MQTT client - 3.1, 3.1.1, 5, TLS - with no
  broker inside, the same one the bridges use. The load engine, the scenario engine and the
  orchestrator are built on it and nothing else;
- the timeline runs arbitrary commands (`ssh`, `docker`, `kubectl`, `systemctl`), so the nodes of any
  cluster are taken away and brought back the same way, whether or not the tool knows what a cluster
  is;
- the oracle is a command and the answer expected of it, not a piece of one broker's internals
  compiled in: `redis` for XMQ, `emqx ctl` and its REST API for EMQX, REST for HiveMQ, events for
  Kubernetes. The checks that speak MQTT alone need none of that and work everywhere.

Work, in the order it makes sense:

1. An inventory of what the tools depend on today, and what it takes to build and ship them outside
   the XMQ tree. This is the first honest step towards "installing it must not require XMQ".
2. Split the core from the plugins: broker-specific checks live in a plugin directory, the core stays
   neutral, and the scenario format is versioned from the first day it is public. The moment somebody
   else writes scenarios, that format is an API and it cannot move cheaply any more.
3. An honest comparison. The same machine, the same broker, the same scenario, published commands and
   versions - a figure without those is worth nothing, and a tool that measures brokers has to be
   seen to measure them fairly or it is marketing. `emqtt_bench` has been seen at around 10K
   connections per second against `xmq_con` at 18K on one machine; that is a claim to reproduce and
   document before it is repeated. Its result is known to depend on the number of Erlang schedulers
   and on its own options, so the first run may be a bad run rather than a ceiling.
4. A service and a user interface - through the broker's control service and the interface that
   already exists, not as a separate application with its own users, runs and charts.
5. A project of its own, if there is pull: external users, issues, pull requests.

Risks worth remembering: the file format becomes a public API the moment somebody else writes
scenarios; three operating systems have to be packaged, installed and supported; and the tool has to
be one others can run themselves, because a benchmark nobody can repeat is an advertisement.
