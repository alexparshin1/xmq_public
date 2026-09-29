# XMQ in Docker

Runs the released XMQ MQTT server in a container. The image installs the published
`.deb`, so it contains the same binaries as a package install — nothing is rebuilt.

## Quick start

```bash
./build.sh
docker run --rm -p 1883:1883 -p 18883:18883 alexeyparshin/xmq
```

`build.sh` takes the XMQ version from `../VERSION.txt` and the SPTK version from
`../../sptk5/code/VERSION.txt` (see `versions.sh`), so no version number is written down
in this directory.

The broker is then on `localhost:1883` and accepts clients with no credentials, so
there is nothing to configure before the first publish:

```bash
mosquitto_pub -h localhost -t test/hello -m 'first message'
```

The image carries XMQ's own clients too, so nothing has to be installed on the host:
`docker exec <container> xmq_pub -h localhost -t test/hello -m 'first message'`.

The configuration interface is on `https://localhost:18883` (self-signed certificate,
so the browser will warn) once `XMQ_ADMIN_PASSWORD` is set: sign in as **admin** with that
password. Without it the interface answers only inside the container, so a published port
reaches nothing - the broker does not open an interface nobody has set a password for.

Anonymous access is on because the image is mostly used to try the broker out, and a
first attempt that fails on credentials nobody has been told about wastes everybody's
time. It is the wrong setting the moment the container is reachable from a network you
do not control — turn it off there, and create accounts in the web interface:

```bash
docker run --rm -p 1883:1883 -e XMQ_ALLOW_ANONYMOUS=false alexeyparshin/xmq
```

## With persistence

Queued messages, retained messages and sessions live in Redis. `docker compose up`
starts both and wires them together:

```bash
docker compose up
```

Without Redis the broker works but forgets everything when it restarts.

## Settings

The entrypoint applies these to `/etc/xmq/xmq_server.conf` before the server starts.
Anything not listed here is edited in the file itself, or through the web interface.

| Variable | Default | Meaning |
|---|---|---|
| `XMQ_ADMIN_PASSWORD` | unset | Password for the `admin` account: upper and lower case, a digit and a punctuation character, or the container refuses to start. Until it is set the configuration interface answers inside the container only, so a published port reaches nothing |
| `XMQ_ALLOW_ANONYMOUS` | `true` | Accept clients with no credentials |
| `XMQ_PERSISTENCE` | `false` | Store sessions and messages in Redis |
| `XMQ_REDIS_URI` | `redis://redis_server:6379` | Where that Redis is |
| `XMQ_CLEAN_START` | `false` | `true` discards stored state on every start |
| `XMQ_LOG_LEVEL` | `INFO` | `DEBUG`, `INFO`, `WARNING`, `ERROR` |
| `XMQ_CERT_CN` | `xmq` | Common name for the generated certificate |
| `XMQ_LOG_FILE` | `discard` | `keep` also writes the log to a file, as a package install does |

Ports: **1883** MQTT, **8883** MQTT over TLS, **18883** the web interface.

## Things worth knowing

**Connections are not in the log by default.** The `connect` and `disconnect`
categories under `logging.log_level` have to be set to `DEBUG` before client
activity shows up; `XMQ_LOG_LEVEL` alone does not do it.

**Mount `/etc/xmq` if the container is more than a quick try.** The configuration, the
accounts and the certificate live there. Without a volume, every restart is a fresh
install: the admin password goes back to `admin` and the certificate changes.

```bash
docker run -d -p 1883:1883 -v xmq-config:/etc/xmq alexeyparshin/xmq
```

**Raise the descriptor limit for real load.** Docker gives a container 1024 open
files by default, which caps the broker at about a thousand connections — nowhere
near what it can hold. `docker-compose.yml` sets this already; for `docker run`:

```bash
docker run --ulimit nofile=1048576:1048576 -p 1883:1883 alexeyparshin/xmq
```

**The log goes to stdout only.** The broker writes to stdout *and* to
`/var/log/xmq/`, but in a container that file duplicates what `docker logs` already
shows and nothing rotates it, so it is sent to `/dev/null`. Set `XMQ_LOG_FILE=keep`
to get the file back, and mount `/var/log/xmq` so it does not fill the container
layer.

**Use your own certificate for anything real.** The generated one is self-signed and
regenerated whenever `/etc/xmq/certs` is empty. Mount a directory with `node.crt` and
`node.key` over it instead.

**amd64 only.** The published packages are x86-64, so the image will not run on
Raspberry Pi or Apple Silicon without emulation. An arm64 package would be worth
having — much of the MQTT audience is on ARM.

## Publishing to Docker Hub

The account is `alexeyparshin`; create a repository named `xmq` under it, then:

```bash
./build.sh
./publish.sh
```

`publish.sh` runs `check-image.sh` on the image, logs in, and pushes it as
`<version>` and `latest`.

Both tags matter: `:latest` is what people type without thinking, and the version tag
is what anyone pinning a deployment will use. Push both on every release, and never
move a version tag to different content — that breaks deployments that pinned it.

The Docker Hub page shows the repository's own description, which is edited on the
site and is not taken from this file — paste the quick start there, and link to
https://xmq.sptk.net.
