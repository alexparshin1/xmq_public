#!/bin/bash
#
# The cluster stand: several xmq_server nodes as plain processes, across several machines,
# sharing one Redis and one accounts database.
#
# Nodes are processes, not services, for two reasons. A scenario has to be able to stop and start
# one in the middle of a run (that is what the cluster is being tested for), and the installed
# service on a machine belongs to the release bench, which must not be disturbed by tests.
#
# A node is named as `node@host`, so one stand can span machines: the cluster's links then cross a
# real network instead of loopback, which is the cheaper and the less honest of the two. Everything
# a node needs lives on the machine it runs on, under the same path everywhere
# ($HOME/cluster/<node>), so the layout is the same whether a node is local or remote, and the
# running of one is a single ssh away.
#
# The cluster requires every node to name the storage the same way: a node joining is refused with
# "Cluster nodes must use the same database" when its `redis_uri` does not match the other's *as a
# string* (Cluster.cpp). A proxy per node would spell it differently on each, so the nodes share one
# URI here and one amount of storage. Taking the storage away from a single node, rather than from
# all of them, has to wait for that check to compare an identity written in the storage instead of
# the address it was reached by - and could then use a proxy again.
#
# One accounts database for the whole cluster, not one per node: the nodes of a cluster share the
# store, which is what makes them agree on who may connect rather than being configured one at a
# time. theater's PostgreSQL already holds it.
#
# Usage:
#   stand.sh up                 prepare (dirs, certificates, configuration), start every node
#   stand.sh down               stop every node
#   stand.sh start|stop <node>  one node (what a scenario timeline calls)
#   stand.sh status             processes, ports, and what the cluster has written in Redis
#   stand.sh wipe               empty this stand's Redis database, and nothing else
#   stand.sh tail <node>        the node's console log
#   stand.sh mark               remember where every node's logs end, for check-logs
#   stand.sh check-logs         what the nodes logged since the mark: a crash, or a node not running,
#                               fails it; what went cluster-offline is reported
#   stand.sh deploy <server> <host>...
#                               lay a server binary down on those machines, with the SPTK it was
#                               built against, and record which commit it was built from
#
# Overrides: STAND_DIR, SERVER, REDIS_HOST, REDIS_PORT, REDIS_DB, REDIS_URI, CLUSTER_PASSWORD,
# CLIENT_USER, CLIENT_PASSWORD, USER_DATABASE_URI, EXTENSION_LIBRARY, NODES, ALLOW_MIXED_BUILDS.
#
# `up` writes the settings it used to $STAND_DIR/stand.env on every machine of the stand, and every
# later command reads them from there - a timeline step that runs `stand.sh stop node3` on another
# machine means the same stand as the `up` did, without repeating the environment. A variable set
# in the environment still wins.

set -u

STAND_DIR=${STAND_DIR:-$HOME/cluster}
STAND_ENV="$STAND_DIR/stand.env"
if [ -f "$STAND_ENV" ]; then
    while IFS='=' read -r key value; do
        case $key in '' | \#*) continue ;; esac
        [ -z "${!key+x}" ] && export "$key=$value"
    done < "$STAND_ENV"
fi
STAND_SETTINGS="NODES REDIS_HOST REDIS_PORT REDIS_DB REDIS_URI SERVER SERVER_LIBRARY_PATH CLUSTER_PASSWORD
CLIENT_USER CLIENT_PASSWORD USER_DATABASE_URI EXTENSION_LIBRARY"
# A build of its own, never the bench's: the bench's tree and the server installed from it are what
# the release gates measure, and a test stand has no business replacing them. Laid down on every
# machine the stand uses, which is why the path is the same everywhere.
SERVER=${SERVER:-$HOME/cluster/xmq_server}
REDIS_HOST=${REDIS_HOST:-theater}
REDIS_PORT=${REDIS_PORT:-6379}
REDIS_DB=${REDIS_DB:-4}
# One spelling for every node, which is what the cluster asks of them; see the note at the top.
REDIS_URI=${REDIS_URI:-redis://$REDIS_HOST:$REDIS_PORT/$REDIS_DB}
CLUSTER_PASSWORD=${CLUSTER_PASSWORD:-Stand-Cluster1}
# ^ the server's password policy wants an uppercase letter and a digit, and refuses the account's
#   password silently otherwise - the links simply keep failing.
CLIENT_USER=${CLIENT_USER:-user}
CLIENT_PASSWORD=${CLIENT_PASSWORD:-User-Secret1}
# ^ the account the test clients connect with. It is made once in the configuration interface: the
#   import that seeds the accounts database updates the accounts already there and creates none, so
#   the stand can name this account but not invent it - and a server reads the accounts as it starts,
#   so an account made while the stand is up is there only after the next `up`. The cluster account
#   above is the stand's own business, and its password has to match what the nodes present when
#   they link.
USER_DATABASE_URI=${USER_DATABASE_URI:-postgresql://xmq:xmq_secret@theater/xmq}
# The authenticator extension, beside the server on every machine: a broker with no extension that
# authenticates admits nobody, and every cluster link arrives with an account and a password.
EXTENSION_LIBRARY=${EXTENSION_LIBRARY:-$HOME/cluster/lib/libxmq_user_database.so}
# Some machines carry SPTK of their own and some carry a private copy of it - a stand built against
# a machine whose installed SPTK is another version keeps the one it needs in ~/sptk. Naming both
# places costs nothing and saves discovering, on one machine only, that a node cannot find a
# library: on Ubuntu, where the installed SPTK is not the version the project wants, the binary
# names ~/.local in its runpath, and without this a node starts and dies with "cannot open shared
# object file".
SERVER_LIBRARY_PATH=${SERVER_LIBRARY_PATH:-$HOME/cluster/lib:$HOME/sptk/lib}
# Two nodes on each machine, so that the links between them are links between hosts, and the stand
# still leaves every machine able to carry its share of clients and traffic.
NODES=${NODES:-"node1@thinker10 node2@thinker10 node3@thinker11 node4@thinker11 node5@theater node6@theater"}

HERE=$(dirname "$(readlink -f "$0")")
LOCAL_HOST=$(hostname)
QUERY="python3 $HERE/redis_query.py $REDIS_HOST $REDIS_PORT $REDIS_DB"

# A node is written `name@host`; a plain name means this machine.
node_name() { echo "${1%%@*}"; }
node_host() { case $1 in *@*) echo "${1#*@}" ;; *) echo "$LOCAL_HOST" ;; esac; }
node_spec() {
    local wanted
    wanted=$(node_name "$1")
    for candidate in $NODES; do
        [ "$(node_name "$candidate")" = "$wanted" ] && { echo "$candidate"; return 0; }
    done
    echo "unknown node: $1" >&2
    return 1
}

# Everything a node owns lives under one directory on its own machine, and the path is the same on
# every machine: the configuration, the certificate, the logs, the pid file.
node_dir()  { echo "$STAND_DIR/$(node_name "$1")"; }
node_host_of() { node_host "$(node_spec "$1")"; }

# Ports are laid out by node number, and the numbers are unique across the stand, so two nodes on
# one machine never collide: clients above 2884, cluster TLS above 9883, the control web service
# above 28883. None of them touch the installed service, which keeps 1883/1884/8883.
#
# Both forms of a node are accepted everywhere - the plain name the test file writes, and the
# `name@host` the loops of this script carry - because a function that took only one of them was
# how the first status of a six-node stand answered "unknown node: node1@thinker10" six times.
node_index() {
    local wanted index=0
    wanted=$(node_name "$1")
    for candidate in $NODES; do
        index=$((index + 1))
        [ "$(node_name "$candidate")" = "$wanted" ] && { echo "$index"; return 0; }
    done
    echo "unknown node: $1" >&2
    return 1
}

client_port() { echo $((2883 + $(node_index "$1"))); }
tls_port()    { echo $((9882 + $(node_index "$1"))); }
web_port()    { echo $((28882 + $(node_index "$1"))); }

# Run a command on the machine a node lives on. A local node is run here and not through ssh to
# ourselves: that is both faster and one less thing to authenticate, and a failed ssh to the local
# machine is how a timeline step once "succeeded" without doing anything.
on_host() {
    local host=$1 command=$2
    if [ "$host" = "$LOCAL_HOST" ]; then
        bash -c "$command"
    else
        ssh -o BatchMode=yes "$host" "$command"
    fi
}

# Put a file on the machine a node lives on, creating the directory if it is the first thing there.
put_file() {
    local node=$1 path=$2 source=$3
    on_host "$(node_host_of "$node")" "mkdir -p \"$(dirname "$path")\" && cat > \"$path\"" < "$source"
}

wait_for_port() {
    local host=$1 port=$2 seconds=$3
    for _ in $(seq $((seconds * 4))); do
        (exec 3<>"/dev/tcp/$host/$port") 2>/dev/null && { exec 3>&-; return 0; }
        sleep 0.25
    done
    return 1
}

# One authority for the stand and one certificate per node, signed by it.
#
# The cluster verifies a peer against connections.ssl_keys.cafile when that is set, and only
# otherwise against the peers directory - which is the installed one, /etc/xmq/certs/peers, shared
# with the bench and not ours to write into. A CA of the stand's own keeps the whole arrangement
# inside $STAND_DIR, and one signature is what makes the nodes trust each other. The authority is
# made once, where the stand is run from; only the certificates travel to the nodes.
make_certificates() {
    local node=$1 staging="$STAND_DIR/staging/$(node_name "$1")"
    mkdir -p "$staging"
    if [ ! -f "$STAND_DIR/ca.crt" ]; then
        openssl req -x509 -nodes -newkey rsa:2048 -days 3650 \
            -keyout "$STAND_DIR/ca.key" -out "$STAND_DIR/ca.crt" -subj "/CN=xmq-stand-ca" >/dev/null 2>&1
    fi
    if [ ! -f "$staging/node.crt" ]; then
        openssl req -nodes -newkey rsa:2048 -keyout "$staging/node.key" -out "$staging/node.csr" \
            -subj "/CN=xmq-$(node_name "$1")" >/dev/null 2>&1
        openssl x509 -req -in "$staging/node.csr" -CA "$STAND_DIR/ca.crt" -CAkey "$STAND_DIR/ca.key" \
            -CAcreateserial -out "$staging/node.crt" -days 3650 >/dev/null 2>&1
    fi
    # The node's own key and certificate, and the authority that lets it verify its peers.
    put_file "$1" "$(node_dir "$1")/node.key" "$staging/node.key"
    put_file "$1" "$(node_dir "$1")/node.crt" "$staging/node.crt"
    put_file "$1" "$(node_dir "$1")/ca.crt"   "$STAND_DIR/ca.crt"
}

# The configuration is regenerated on every `up` rather than edited in place: it is a function of
# the ports and paths above, and a stale copy is how a node ends up in the wrong cluster.
write_configuration() {
    local node=$1 staging="$STAND_DIR/staging/$(node_name "$1")"
    local dir client tls web host
    dir=$(node_dir "$node"); client=$(client_port "$node"); tls=$(tls_port "$node")
    web=$(web_port "$node"); host=$(node_host_of "$node")
    mkdir -p "$staging"
    cat > "$staging/xmq_server.conf" <<JSON
{
  "connections": {
    "listener": [
      { "id": 1, "name": "Not Encrypted", "bind_ip": "0.0.0.0", "port": $client,
        "threads": 2, "protocol": "MQTT", "enable": true },
      { "id": 2, "name": "Encrypted", "bind_ip": "0.0.0.0", "port": $tls,
        "threads": 2, "protocol": "MQTT+SSL", "enable": true }
    ],
    "ssl_keys": {
      "cafile": "$dir/ca.crt",
      "keyfile": "$dir/node.key",
      "certfile": "$dir/node.crt",
      "verify_depth": 1
    }
  },
  "queue_limits": { "max_size": 50000, "max_inflight_messages": 32768 },
  "server_limits": { "send_threads": "auto", "receive_threads": "auto", "max_topic_alias": 128 },
  "authentication": {
    "allow_anonymous": false,
    "database_uri": "$USER_DATABASE_URI"
  },
  "persistence": {
    "redis_uri": "$REDIS_URI",
    "clean_start": false,
    "enabled": true,
    "max_redis_connections": 32,
    "max_queued_writes": 100
  },
  "logging": {
    "log_to": "$dir/xmq_server.log",
    "keep_logs": 3,
    "min_log_level": "INFO",
    "log_level": {
      "connect": "DEBUG",
      "disconnect": "DEBUG",
      "subscribe": "DEBUG",
      "unsubscribe": "DEBUG",
      "publish": "ERROR",
      "ack": "ERROR",
      "server_connections": "DEBUG",
      "server_events": "DEBUG",
      "cluster_connections": "INFO",
      "cluster_events": "INFO"
    }
  },
  "web_service": { "listener_port": $web, "encrypted": false },
  "bridges": [],
  "cluster": {
    "enabled": true,
    "password": "$CLUSTER_PASSWORD",
    "this_node": { "node_name": "$(node_name "$node")", "host_port": "$host:$tls", "encrypted": true },
    "nodes": []
  }
}
JSON
    put_file "$node" "$dir/xmq_server.conf" "$staging/xmq_server.conf"
}

# The authenticator. Extensions are read beside the server's own configuration, not from the
# installation prefix, which is why each node gets its own fragment in its own directory.
write_extensions() {
    local node=$1 dir staging="$STAND_DIR/staging/$(node_name "$1")"
    dir=$(node_dir "$node")
    mkdir -p "$staging"
    cat > "$staging/50-user-database.conf" <<JSON
{
  "extensions": [
    {
      "name": "user-database",
      "library": "$EXTENSION_LIBRARY",
      "enabled": true,
      "required": true,
      "settings": { "username": "", "password": "", "unknown_user": "deny" }
    }
  ]
}
JSON
    put_file "$node" "$dir/xmq_extensions.d/50-user-database.conf" "$staging/50-user-database.conf"
}

# The cluster account's password, given through the server's own command, on the machine the node
# runs on.
#
# The account appears in the shared database by itself, on the first start, with a password generated
# on the spot - 24 characters nobody can read back, since the database keeps only a verifier. What a
# node presents when it links to a peer is `cluster.password` from its own configuration, so the
# account's password has to be that, and only this command can make it so without the interface. The
# command reads the password from standard input, and the server's policy wants an uppercase letter
# and a digit in it, refusing the account's password otherwise - which leaves the links failing with
# nothing said about why.
#
# It runs before the nodes start: a server reads the accounts once, as it starts, and a client
# connecting later is checked against what it read.
set_cluster_password() {
    local node command answer
    for node in $NODES; do
        command="printf '%s\\n' '$CLUSTER_PASSWORD' | LD_LIBRARY_PATH='$SERVER_LIBRARY_PATH' $SERVER --configuration-file \"$(node_dir "$node")/xmq_server.conf\" --set-password cluster 2>&1"
        answer=$(on_host "$(node_host_of "$node")" "$command")
        case $answer in
            *"password set"*) ;;
            *) echo "$(node_name "$node"): could not set the cluster account's password: $answer" >&2 ;;
        esac
    done
}

# A node is started through a small script of its own, in a session of its own.
#
# The session is what makes a node outlive the run that started it: a local node starts in the
# stand's own process group, so an interrupted `up` - Ctrl-C in a terminal, or a tool call that is
# stopped - would take the nodes down with it. That is exactly what happened the first time this
# stand was brought up across machines: two nodes on the machine running the stand died when the
# run was interrupted, while the remote ones, started over ssh, stayed.
#
# The script is what makes the pid file name the server rather than a wrapper: it writes its own
# pid and then execs the server, so the number in the file stays the server's. It also carries the
# library path, which some machines need and others do not; see SERVER_LIBRARY_PATH above.
write_launcher() {
    local node=$1 dir staging="$STAND_DIR/staging/$(node_name "$1")"
    dir=$(node_dir "$node")
    mkdir -p "$staging"
    cat > "$staging/start.sh" <<LAUNCHER
#!/bin/bash
echo \$\$ > "$dir/xmq_server.pid"
exec env LD_LIBRARY_PATH="$SERVER_LIBRARY_PATH" "$SERVER" --configuration-file "$dir/xmq_server.conf"
LAUNCHER
    chmod +x "$staging/start.sh"
    put_file "$node" "$dir/start.sh" "$staging/start.sh"
    # Writing a file through a shell does not carry its mode, and a launcher that cannot be executed
    # is a node that does not start.
    on_host "$(node_host_of "$node")" "chmod +x \"$dir/start.sh\""
}

start_node() {
    local node=$1 dir
    dir=$(node_dir "$node")
    stop_node "$node" 2>/dev/null
    # All three streams go to the log, so the ssh that started the node has nothing left open and
    # returns at once.
    on_host "$(node_host_of "$node")" \
        "cd \"$dir\" && setsid --fork \"$dir/start.sh\" >> \"$dir/console.log\" 2>&1 < /dev/null"
    # A start that opened nothing is a failed start: a timeline step that reported success here
    # used to leave every check after it testing a cluster that was missing a node.
    if ! wait_for_port "$(node_host_of "$node")" "$(client_port "$node")" 20; then
        echo "$(node_name "$node") did not open $(client_port "$node")" >&2
        return 1
    fi
}

stop_node() {
    local node=$1 dir
    dir=$(node_dir "$node")
    on_host "$(node_host_of "$node")" "
        pid_file=\"$dir/xmq_server.pid\"
        # Nothing to stop is an answer of its own: a timeline step that stopped nothing must not
        # pass for one that took a node away.
        [ -f \"\$pid_file\" ] || { echo \"$(node_name "$node") is not running\" >&2; exit 3; }
        kill -0 \"\$(cat \"\$pid_file\")\" 2>/dev/null || { rm -f \"\$pid_file\"; echo \"$(node_name "$node") is not running\" >&2; exit 3; }
        kill \"\$(cat \"\$pid_file\")\" 2>/dev/null
        for _ in \$(seq 20); do
            kill -0 \"\$(cat \"\$pid_file\")\" 2>/dev/null || break
            sleep 0.25
        done
        kill -9 \"\$(cat \"\$pid_file\")\" 2>/dev/null
        rm -f \"\$pid_file\""
}

node_state() {
    local node=$1 dir pid
    dir=$(node_dir "$node")
    pid=$(on_host "$(node_host_of "$node")" "cat \"$dir/xmq_server.pid\" 2>/dev/null")
    if [ -n "$pid" ] && wait_for_port "$(node_host_of "$node")" "$(client_port "$node")" 1; then
        echo "running (pid $pid, $(node_host_of "$node"):$(client_port "$node"))"
    else
        echo "stopped"
    fi
}

# The settings this stand runs with, on every machine of it; see the note at the top.
write_stand_env() {
    local staging done_hosts=" " host
    staging=$(mktemp)
    for key in $STAND_SETTINGS; do
        printf '%s=%s\n' "$key" "${!key}" >> "$staging"
    done
    for node in $NODES; do
        host=$(node_host "$node")
        case $done_hosts in *" $host "*) continue ;; esac
        done_hosts="$done_hosts$host "
        put_file "$node" "$STAND_ENV" "$staging"
    done
    rm -f "$staging"
}

# The server each machine would run, as deploy recorded it: "commit=... sha256=... from=...".
server_stamp() {
    on_host "$1" "cat \"$SERVER.source\" 2>/dev/null || echo 'commit=unknown (not deployed by stand.sh)'"
}

# Every machine runs a build of the same commit, or the test says nothing about that commit: a node
# left with yesterday's server once passed for today's fix.
check_builds() {
    local done_hosts=" " host stamp commit commits=""
    for node in $NODES; do
        host=$(node_host "$node")
        case $done_hosts in *" $host "*) continue ;; esac
        done_hosts="$done_hosts$host "
        stamp=$(server_stamp "$host")
        commit=$(printf '%s' "$stamp" | sed -n 's/^commit=\([^ ]*\).*/\1/p')
        echo "  $host: $stamp"
        case " $commits " in *" $commit "*) ;; *) commits="$commits $commit" ;; esac
    done
    if [ "$(echo $commits | wc -w)" -gt 1 ] && [ "${ALLOW_MIXED_BUILDS:-0}" != 1 ]; then
        echo "the machines run servers built from different commits:$commits - deploy one build" \
             "everywhere, or set ALLOW_MIXED_BUILDS=1" >&2
        return 1
    fi
}

command_up() {
    echo "servers:"
    check_builds || exit 1
    write_stand_env
    # The authority first: a node's certificate is signed by it, so it has to exist by then.
    make_certificates "$(node_spec "$(node_name "${NODES%% *}")")"
    for node in $NODES; do make_certificates "$node"; done
    for node in $NODES; do write_configuration "$node"; done
    for node in $NODES; do write_extensions "$node"; done
    for node in $NODES; do write_launcher "$node"; done
    # Before the nodes start, not after: the accounts are read once, as a server starts.
    set_cluster_password
    # A run's logs start empty, so that what check-logs finds is this run's and not a crash from
    # the day before - which is what a log that only grows offers instead.
    for node in $NODES; do
        local dir
        dir=$(node_dir "$node")
        on_host "$(node_host_of "$node")" "cd \"$dir\" && for f in console.log xmq_server.log; do [ -f \$f ] && mv -f \$f \$f.prev; done; rm -f log.mark; true"
    done
    local failed=0
    for node in $NODES; do start_node "$node" || failed=1; done
    sleep 3
    command_status
    return $failed
}

command_down() {
    for node in $NODES; do stop_node "$node" 2>/dev/null; done
    return 0
}

# Where every node's logs end now; check-logs reads from there.
command_mark() {
    for node in $NODES; do
        on_host "$(node_host_of "$node")" "cd \"$(node_dir "$node")\" && for f in console.log xmq_server.log; do printf '%s %s\\n' \$f \$( [ -f \$f ] && wc -c < \$f || echo 0); done > log.mark"
    done
}

# What the nodes logged since the mark (or since `up`): a crash fails it, and so does a node that is
# not running when a test is over; going cluster-offline is reported, since a test that takes the
# storage away expects it.
command_check_logs() {
    local failed=0 node dir found state
    for node in $NODES; do
        dir=$(node_dir "$node")
        found=$(on_host "$(node_host_of "$node")" "cd \"$dir\" 2>/dev/null || exit 0
            for f in console.log xmq_server.log; do
                [ -f \$f ] || continue
                from=\$(awk -v f=\$f '\$1 == f { print \$2 }' log.mark 2>/dev/null)
                tail -c +\$(( \${from:-0} + 1 )) \$f | grep -E 'terminate called|Segmentation fault|Aborted|core dumped|AddressSanitizer|ThreadSanitizer|std::terminate|Cluster-offline|Cluster-online' | sed \"s|^|\$f: |\"
            done")
        if printf '%s\n' "$found" | grep -qE 'terminate|Segmentation|Aborted|core dumped|Sanitizer'; then
            echo "$(node_name "$node"): CRASHED"
            printf '%s\n' "$found" | grep -E 'terminate|Segmentation|Aborted|core dumped|Sanitizer' | head -5 | sed 's/^/    /'
            failed=1
        fi
        state=$(node_state "$node")
        if [ "$state" = stopped ]; then
            echo "$(node_name "$node"): not running"
            failed=1
        fi
        # Counted in the server's own log: the console carries the same lines, the node logging to both.
        if printf '%s\n' "$found" | grep -q '^xmq_server.log: .*Cluster-offline'; then
            echo "$(node_name "$node"): went cluster-offline $(printf '%s\n' "$found" | grep -c '^xmq_server.log: .*Cluster-offline') time(s), came back $(printf '%s\n' "$found" | grep -c '^xmq_server.log: .*Cluster-online')"
        fi
    done
    [ $failed = 0 ] && echo "node logs: no crash, every node running"
    return $failed
}

# Lay a server down on machines of the stand: the binary where SERVER says, the SPTK libraries it
# resolves from under $HOME into ~/sptk/lib (which every launcher puts on the library path), and a
# stamp of the commit it was built from, which `up` compares across machines. Run on the machine the
# build is on; the machines given have to be able to run it - the same distribution.
command_deploy() {
    local binary=${1:?server binary} host source_dir commit stamp libs
    shift
    [ $# -gt 0 ] || { echo "deploy: name the machines" >&2; exit 2; }
    binary=$(readlink -f "$binary")
    source_dir=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$(dirname "$binary")/CMakeCache.txt" 2>/dev/null)
    commit=unknown
    if [ -n "$source_dir" ]; then
        commit=$(git -C "$source_dir" rev-parse --short HEAD 2>/dev/null || echo unknown)
        # What the server is built from; a change elsewhere - a test, a script - is not the server's.
        git -C "$source_dir" diff --quiet HEAD -- server storage common base client service extension 2>/dev/null ||
            commit="$commit-dirty"
    fi
    stamp="commit=$commit sha256=$(sha256sum "$binary" | cut -c1-12) from=$(hostname):$binary"
    libs=$(ldd "$binary" | awk '{print $3}' | grep "^$HOME/" || true)
    for host in "$@"; do
        if [ "$host" = "$LOCAL_HOST" ]; then
            [ "$binary" = "$(readlink -f "$SERVER")" ] || cp -f "$binary" "$SERVER"
        else
            ssh -o BatchMode=yes "$host" "mkdir -p \"$(dirname "$SERVER")\" \"$STAND_DIR\" \$HOME/sptk/lib"
            scp -q "$binary" "$host:$SERVER"
            for lib in $libs; do scp -q "$lib" "$host:sptk/lib/"; done
            # The stand's own scripts go along: a timeline step runs them on that machine, and a
            # copy left from an older stand there answers for this one.
            scp -q "$HERE/stand.sh" "$HERE/redis_query.py" "$HERE/redis_proxy.py" "$host:$STAND_DIR/"
        fi
        on_host "$host" "echo '$stamp' > \"$SERVER.source\""
        echo "$host: $stamp"
    done
}

command_status() {
    echo "server under test: $SERVER (on each machine)"
    local done_hosts=" " host
    for node in $NODES; do
        host=$(node_host "$node")
        case $done_hosts in *" $host "*) continue ;; esac
        done_hosts="$done_hosts$host "
        echo "  $host: $(server_stamp "$host")"
    done
    echo "redis: $REDIS_URI"
    for node in $NODES; do
        printf '%-6s %s\n' "$(node_name "$node")" "$(node_state "$node")"
    done
    echo
    echo "cluster keys in Redis:"
    $QUERY KEYS 'cluster:*' 2>/dev/null | sed 's/^/  /'
    for key in cluster:coordinator cluster:term cluster:members; do
        local value
        value=$($QUERY GET "$key" 2>/dev/null)
        [ -n "$value" ] && echo "  $key = $value"
    done
    local owners
    owners=$($QUERY KEYS 'session_*_owner' 2>/dev/null | wc -l)
    echo "  session owners: $owners"
}

# Everything in the stand's own database, and only there: the farm works in database 1 and the
# desktops in 2, so a FLUSHALL here would be somebody else's data loss.
#
# Accounts are not kept here - they live in the user database the authenticator extension reads (SQLite
# beside a configuration by default, or a database of its own named by `database_uri`) - so the wipe
# touches nothing of theirs.
command_wipe() {
    $QUERY FLUSHDB
    echo "emptied $REDIS_HOST:$REDIS_PORT db $REDIS_DB"
}

case "${1:-help}" in
    up)      command_up ;;
    down)    command_down ;;
    start)   start_node "${2:?node}" ;;
    stop)    stop_node "${2:?node}" ;;
    status)  command_status ;;
    wipe)    command_wipe ;;
    mark)    command_mark ;;
    check-logs) command_check_logs ;;
    deploy)  shift; command_deploy "$@" ;;
    tail)    on_host "$(node_host_of "${2:?node}")" "tail -n 40 \"$(node_dir "${2:?node}")/console.log\"" ;;
    *)       sed -n '/^set -u$/q;2,$p' "$0" | sed 's/^# \{0,1\}//' ;;
esac
