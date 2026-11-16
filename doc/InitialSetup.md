# XMQ Initial Setup

This guide describes the first-time setup of an installed XMQ server using its
control API. It assumes XMQ is already built and installed. Initial Setup sets
the administrator and cluster passwords and configures Redis and the user
database (PostgreSQL, MySQL, or local SQLite).

> **Important:** `xmq_server --reset-configuration` resets the server
> configuration; it does not set these passwords or database URIs. After the
> reset, complete Initial Setup through `InitialSetupControl`.

## 1. Reset and start XMQ

**First stop every running `xmq_server` process on the target host.** Do not
reset configuration while an old broker process is still serving traffic. List
the processes:

```bash
pgrep -af xmq_server
```

Stop them through the service manager or supervisor that owns them. For a
common systemd installation (stopping a service needs root, so `sudo` is part
of the command):

```bash
sudo systemctl stop xmq_server
```

A `Unit xmq_server.service not loaded` or `not-found` answer is **not** an
error: it means this installation is not run by systemd under that name. Carry
on with the process cleanup below. A `systemctl` failure caused by missing
privileges, on the other hand, means the stop did not happen — retry it with
`sudo`.

The service name and executable path depend on how XMQ was installed. Check
them on the target host rather than assuming `xmq_server.service` exists. For
example, a user-local installation may use `~/.local/bin/xmq_server` and have
no systemd unit. If any `xmq_server` processes remain after stopping the
supervisor, terminate them gracefully and verify none remain. On Linux, for
example:

```bash
sudo pkill -TERM -x xmq_server
pgrep -x xmq_server || true
```

Use `pgrep -x xmq_server` for this check, not `pgrep -af xmq_server`: the
`-f` form also matches shells and wrappers whose command line merely mentions
the name (for example `bash -c 'cd ~/.local/bin && ./xmq_server ...'`), and
those are not broker processes. `pgrep -x` matches the process name exactly.
If it prints nothing, no broker is running.

Do not continue while an old broker is running. Run the reset as the account
that owns the installation and its configuration:

```bash
xmq_server --reset-configuration
```

The command replaces the configuration with a starting configuration, keeps
the old file beside it with an `.old` suffix, and continues into a normal
server start in the foreground. Accounts are not generally reset by this
command. On a fresh installation with no accounts, it creates an `admin`
account without a password.

When running over SSH, do not leave the reset command as an unmanaged
foreground process: the SSH command stays attached to the broker, and the
broker is killed when the connection ends. Start it through the installation's
normal persistent service/supervisor, or start it detached and then wait
until the interface answers. A concrete detached start over SSH:

```bash
nohup xmq_server --reset-configuration >/tmp/xmq_initial_setup.log 2>&1 </dev/null &
```

Then wait for the interface to respond before continuing:

```bash
for i in $(seq 1 15); do
  curl --silent --insecure --max-time 2 https://127.0.0.1:18883/ >/dev/null 2>&1 && break
  sleep 1
done
```

Read `/tmp/xmq_initial_setup.log` to confirm the reset happened and the
listeners came up. Confirm the process is still running with
`pgrep -ax xmq_server`.

Until Initial Setup gives `admin` a password, the control interface is
available only on the server itself:

```text
https://127.0.0.1:18883
```

The interface uses a self-signed certificate initially. An HTTP client must
trust that certificate or explicitly allow it for this first-run connection.
For example, `curl` supports `--insecure` (or `-k`); use this only for the
known local first-run endpoint.

## 2. Sign in as the initial administrator

Send a JSON `POST` to `/Login` with the empty initial password:

```http
POST https://127.0.0.1:18883/Login
Content-Type: application/json

{"username":"admin","password":""}
```

The successful response contains a `token` and normally
`"setup_required": true`. Keep the token for the setup request.

**When the empty password does not work.** `--reset-configuration` replaces the
configuration but not the accounts. The starting configuration points at the
local SQLite accounts file, so on a machine that has been set up before,
`admin` there may already have a password, and the empty-password login above
is refused with `"success": false` and a description. Decide which case applies
from the startup log:

- `No password is set for the 'admin' account` is present: the account is still
  passwordless, and the empty-password login is the right one.
- That line is absent: `admin` already has a password. Sign in with the current
  one instead of an empty string, or reset it with
  `printf '%s\n' '<password>' | xmq_server --set-password admin` and restart.

A login reply is JSON. Check `result.success`; when it is `false`, the
`result.description` field says why (wrong password, unknown account). Do not
retry the same request blindly, and do not treat a non-JSON reply as a
credential problem: that means the request did not reach the interface, or it
arrived over plain HTTP on an HTTPS port.

`InitialSetupControl` with `"action":"get"` returns the installed template
values to prefill the form; it is **not** a read-back of the active
configuration. Do not use that response to verify that a submitted Redis or
database URI was applied — it answers the same thing before and after setup.
The `"set"` reply is different: it echoes the setup that was applied (see
step 4), and that echo is the one worth checking.

Example using `curl` and `jq` (run on the XMQ host):

```bash
BASE_URL=https://127.0.0.1:18883
TOKEN=$(
  curl --silent --show-error --insecure \
    -H 'Content-Type: application/json' \
    -d '{"username":"admin","password":""}' \
    "$BASE_URL/Login" | jq -er '.token'
)
```

Do not print or log the token or passwords.

## 3. Submit the Initial Setup request

Send one JSON `POST` to `/InitialSetupControl` with a bearer token. The request
must include the administrator and cluster passwords, node identity, listener
ports, Redis connection fields, and a user database URI:

```http
POST https://127.0.0.1:18883/InitialSetupControl
Content-Type: application/json
Authorization: Bearer <token>

{
  "action": "set",
  "setup": {
    "admin_password": "<new-admin-password>",
    "cluster_password": "<cluster-password>",
    "node_name": "<node-name>",
    "node_host": "<advertised-host-or-address>",
    "mqtt_port": 1883,
    "mqtt_ssl_port": 8883,
    "web_service_port": 18883,
    "redis_host": "<redis-host>",
    "redis_port": 6379,
    "redis_database": 0,
    "redis_username": "",
    "redis_password": "",
    "database_uri": "postgresql://<user>:<password>@<database-host>:5432/<database-name>"
  }
}
```

A runnable equivalent, run on the XMQ host. Build the body with `jq` so the
JSON stays valid, submit it, and delete the file afterwards — it holds every
password in clear text:

```bash
BASE_URL=https://127.0.0.1:18883
TOKEN=$(curl --silent --show-error --insecure \
  -H 'Content-Type: application/json' \
  -d '{"username":"admin","password":""}' "$BASE_URL/Login" | jq -er '.token')

jq -n '{
  action: "set",
  setup: {
    admin_password: "<new-admin-password>",
    cluster_password: "<cluster-password>",
    node_name: "<node-name>",
    node_host: "<advertised-host-or-address>",
    mqtt_port: 1883,
    mqtt_ssl_port: 8883,
    web_service_port: 18883,
    redis_host: "<redis-host>",
    redis_port: 6379,
    redis_database: 0,
    redis_username: "",
    redis_password: "",
    database_uri: "<database-uri>"
  }
}' > /tmp/xmq_setup.json

curl --silent --show-error --insecure \
  -H 'Content-Type: application/json' -H "Authorization: Bearer $TOKEN" \
  --data @/tmp/xmq_setup.json "$BASE_URL/InitialSetupControl" \
  | jq '{success: .result.success, description: .result.description,
         service_restart_required}'

rm -f /tmp/xmq_setup.json
```

`jq -er '.token'` exits non-zero and prints nothing when the login was refused,
which stops the script at that point instead of sending a request with an empty
`Authorization` header.

The administrator and cluster passwords are required. The node name must be at
least two characters and contain only letters, digits, dots, dashes, and
underscores; it must start with a letter, digit, or underscore. `node_host`
must be a hostname or address without a port. The three ports must be distinct
and between 1024 and 65535.

### Password rules and shared accounts

Both passwords must be at least 8 characters long and contain a lowercase
letter, an uppercase letter, a digit, and a punctuation character. A weaker
password is refused with the list of what is missing, and nothing is applied.

When `database_uri` names a PostgreSQL or MySQL database that **already has
accounts**, the supplied passwords are not written to it: they are verified
against the existing `admin` and `cluster` accounts, so setup must be given the
passwords those accounts already have. A mismatch is refused with
`The admin password does not match the one in the shared database` or the same
sentence about the cluster account. This is what a repeated setup against the
same shared database looks like, and it is not a fault: use the passwords
already stored there. A local SQLite setup is the opposite — it replaces the
local accounts with the ones supplied.

### Redis

For a Redis connection, provide `redis_host`, `redis_port`, and a database
number from 0 through 16. Redis username and password are optional; if a
username is supplied, a password is required. XMQ checks the Redis connection
as part of applying the setup.

For the `thinker11` setup described in this runbook, use:

```text
redis://theater:6379/1
```

In the `InitialSetupControl` JSON, represent this URI as
`redis_host: "theater"`, `redis_port: 6379`, `redis_database: 1`, and empty
`redis_username` and `redis_password` fields.

To disable Redis, use an empty `redis_host`, with `redis_port` and
`redis_database` set to `0`:

```json
{
  "redis_host": "",
  "redis_port": 0,
  "redis_database": 0,
  "redis_username": "",
  "redis_password": ""
}
```

### User database

`database_uri` selects the user-account database:

- PostgreSQL: `postgresql://user:password@host:5432/database`
- MySQL: `mysql://user:password@host:3306/database`
- Local SQLite: an empty URI (`""`)

If the URI contains reserved characters in its username or password, URL-encode
those components. When using a shared PostgreSQL or MySQL accounts database,
configure the same database and Redis storage on every node, and use the same
administrator and cluster passwords on every node. Initial Setup does **not**
join a cluster.

For the `thinker11` setup described in this runbook, use this PostgreSQL URI
exactly as supplied:

```text
postgresql://xmq:secret@theater:5432/xmq
```

## 4. Verify completion

Check the API response for success: `result.success` must be `true`, and
`result.description` must be empty. Any non-empty description is the reason the
configuration was **not** applied — fix it and submit again, with a fresh
token.

A `set` reply also carries a `setup` object echoing what was applied — node
name, host, ports, Redis host, port and database number, and `database_uri`.
Read it back to confirm the request was understood, for example:

```json
{
  "result": {"success": true},
  "setup": {
    "node_name": "theater", "node_host": "theater",
    "mqtt_port": 1883, "mqtt_ssl_port": 8883, "web_service_port": 18883,
    "redis_host": "theater", "redis_port": 6379, "redis_database": 1,
    "database_uri": "postgresql://xmq:secret@theater:5432"
  },
  "running": true,
  "service_restart_required": false
}
```

The administrator, cluster and Redis passwords are cleared from that echo, but
**`database_uri` is returned complete, database password included**: redact it
before pasting the reply anywhere. `running` says whether the MQTT service came
back up.

`service_restart_required` is `false` when the interface kept its port. When it
is `true`, the interface could not move to the new port and the old one is
still in use; reconnect there.

On success XMQ writes the configuration and restarts its MQTT service. Then:

1. Sign in again as `admin` with the **new** password and confirm
   `"setup_required"` is absent or `false`:

   ```bash
   curl --silent --show-error --insecure -H 'Content-Type: application/json' \
     -d '{"username":"admin","password":"<new-admin-password>"}' \
     https://127.0.0.1:18883/Login \
     | jq '{success: .result.success, setup_required: (.setup_required // false)}'
   ```

2. Confirm the listeners are up: `pgrep -ax xmq_server` should show a single
   process, and `ss -ltn | grep -E ':(1883|8883|18883)\b'` should show the
   configured ports.

3. Confirm Redis and the user database are actually in use. The log carries
   lines like `Using Redis at <host>:<port>` and
   `user-database: authenticating against N account(s) in postgresql://<user>@<host>:<port>`:

   ```bash
   grep -E 'Using Redis|user-database: authenticating' \
     ~/.local/var/log/xmq/xmq_server.log | tail -3
   ```

   Check the path for your installation; a package install usually logs under
   `/var/log/xmq/`.

The setup API connects to Redis and to the SQL database before applying the
configuration, so a `success` reply already proves both were reachable at that
moment. Do not print `xmq_server.conf` without redacting: it stores the
database password.

### Deployment values for `thinker11`

For the `thinker11` deployment in this runbook, use node name and advertised
host `thinker11`, with MQTT, MQTT+SSL, and web interface ports `1883`, `8883`,
and `18883`, respectively. Use Redis URI `redis://theater:6379/1` and
PostgreSQL URI `postgresql://xmq:secret@theater:5432` — the database name is
omitted, and libpq then connects to a database named after the user, `xmq`.
`postgresql://xmq:secret@theater:5432/xmq` names that same database explicitly
and is equally valid; pick one spelling and use it unchanged on every node.

That node has no systemd unit and runs XMQ from `~/.local/bin/xmq_server`, so
the reset is started detached with `nohup`, as shown in step 1. A successful
submission there answers:

```json
{"success": true, "description": null, "service_restart_required": false}
```

and the log then carries:

```text
[extensions] user-database: authenticating against 3 account(s) in postgresql://xmq@theater:5432
(thinker11) <server_events> Using Redis at theater:6379.
```

## Common pitfalls

- **Calling the wrong endpoint:** use `POST /InitialSetupControl` with
  `{"action":"set","setup":{...}}`; do not send individual password or URI
  requests.
- **Missing bearer token:** obtain the token from `/Login` and send it as
  `Authorization: Bearer <token>`.
- **Connecting remotely before setup:** the interface is loopback-only until
  `admin` has a password. Run the first requests locally or use a secure local
  tunnel.
- **Using `--set-password` as a substitute:** this CLI option sets one account's
  password from standard input, but does not configure Redis or the SQL user
  database and does not apply the complete Initial Setup configuration.
- **Expecting setup to join a cluster:** it does not. Configure cluster
  membership separately after Initial Setup.
- **Treating `pgrep -af xmq_server` output as brokers:** it also lists shells
  whose command line mentions the name. Use `pgrep -x xmq_server` to decide
  whether a broker is really running.
- **Assuming `admin` has no password:** after `--reset-configuration` the
  accounts come from the accounts database the starting configuration names,
  not from the one used before the reset. Check the startup log for
  `No password is set for the 'admin' account` before choosing which password
  to log in with.
- **Verifying with `action: get`:** it returns the installed template, so it
  always looks the same regardless of what was applied. The `set` reply echoes
  the applied setup instead; use that, plus the log, the listeners, and a fresh
  login.
- **Treating a bearer token as harmless:** the token is a JWT whose payload
  carries the username and password (base64, not encrypted). A transcript or a
  `curl -v` dump that includes the token leaks the password. Strip the
  `Authorization` header, or the token, before sharing logs; treat such files
  as secrets (`chmod 600`).
- **Passing a database URI without Redis:** a shared accounts database is
  refused with `A shared accounts database needs shared storage: provide the
  Redis host.` Supply the Redis settings too, or leave the URI empty and use
  local SQLite.
- **Keeping the request body around:** the setup payload file contains the
  passwords in clear text. Delete it after use, and do not paste it into logs.
- **Leaving the broker under `nohup` for good:** a detached process runs the
  setup but does not survive a reboot or a session manager cleanup. Once setup
  is verified, hand the server to the installation's real service/supervisor
  (systemd unit, container entrypoint) and start it from there.
- **Reset on a PostgreSQL-backed node:** when the node's accounts already live
  in PostgreSQL or MySQL, `--reset-configuration` also retires that node's row
  in the database, because storage and cluster settings are stored there. If
  the database cannot be reached, nothing is changed and the command fails.
