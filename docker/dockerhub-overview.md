# XMQ

A fast, free MQTT server for Linux and Windows, written in C++.

    docker run -d --name xmq -p 1883:1883 -p 18883:18883 -v xmq-config:/etc/xmq alexeyparshin/xmq
    docker exec -it xmq xmq_server --set-password admin
    docker restart xmq

MQTT is on `localhost:1883` at once and takes clients with no credentials. The
configuration interface, `https://localhost:18883`, opens once `admin` has a password:
set it once as above - the volume keeps it - and sign in as **admin**. Set `XMQ_ALLOW_ANONYMOUS=false` before the container is reachable from anywhere you do
not control.

## Settings

| Variable | Default | Meaning |
|---|---|---|
| `XMQ_ALLOW_ANONYMOUS` | `true` | Accept clients with no credentials |
| `XMQ_PERSISTENCE` | `false` | Store state in Redis |
| `XMQ_REDIS_URI` | `redis://redis_server:6379` | Where that Redis is |
| `XMQ_CLEAN_START` | `false` | `true` discards stored state on every start |
| `XMQ_LOG_LEVEL` | `INFO` | `DEBUG`, `INFO`, `WARNING`, `ERROR` |
| `XMQ_CERT_CN` | `xmq` | Common name for the generated certificate |
| `XMQ_LOG_FILE` | `discard` | `keep` also writes the log to a file, as a package install does |

Ports: **1883** MQTT, **8883** MQTT over TLS, **18883** web interface.

Mount `/etc/xmq` to keep the configuration, accounts and certificate across
restarts. Raise the descriptor limit for real load:
`--ulimit nofile=1048576:1048576` — the Docker default of 1024 caps the broker at
about a thousand connections.

## With persistence

```yaml
services:
  xmq:
    image: alexeyparshin/xmq
    ports: ["1883:1883", "18883:18883"]
    environment:
      XMQ_PERSISTENCE: "true"
    volumes: ["xmq-config:/etc/xmq"]
    ulimits:
      nofile: {soft: 1048576, hard: 1048576}
    depends_on: [redis_server]
  redis_server:
    image: redis:8-alpine
    command: ["redis-server", "--appendonly", "yes"]
volumes:
  xmq-config:
```

## Logs and tools

The log goes to stdout, where `docker logs` shows it; nothing rotates a file inside a
container, so no file is written unless `XMQ_LOG_FILE=keep` asks for one. Client
connections appear only when the `connect` and `disconnect` log categories are set to
`DEBUG` in the configuration interface.

The load generators `xmq_pub`, `xmq_sub`, `xmq_con` and the scenario runner `xmq_scn`
are in the image, so a broker can be driven from a second container with nothing else
installed. The accounts file of the package is not: it carries known passwords, and the
server writes a fresh one on first start instead.

## Performance

Median latency with 50 000 publishers at 50k msg/s
(AWS c5n.4xlarge, MQTT 5, QoS 1):

| Broker | Median latency | Peak RAM |
|---|---:|---:|
| XMQ | 217 µs | 242 MB |
| Mosquitto | 236 ms | 31.4 GB |
| EMQX | 103 s | 9.78 GB |

Full results, methodology and raw output: <https://xmq.sptk.net>.

Documentation and downloads: <https://xmq.sptk.net>.
