#!/bin/sh
# Idempotent detached start of numbered configurations; compatible with Bash and /bin/sh.
set -eu
usage() { echo "Usage: ${0##*/} [N]" >&2; exit 2; }
[ "$#" -le 1 ] || usage
if [ "$#" -eq 1 ]; then
    case "$1" in ''|*[!0-9]*) usage ;; esac
    [ "${#1}" -le 5 ] || usage
    count=$1
    while [ "${count#0}" != "$count" ]; do count=${count#0}; done
    count=${count:-0}
    [ "$count" -ge 1 ] && [ "$count" -le 65535 ] || usage
fi
command -v pgrep >/dev/null || { echo 'pgrep is required.' >&2; exit 1; }
script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
server=${XMQ_SERVER:-"$script_directory/xmq_server"}
if [ ! -x "$server" ]; then
    [ -z "${XMQ_SERVER:-}" ] || { echo "XMQ_SERVER is not executable: $server" >&2; exit 1; }
    server=$(command -v xmq_server) || { echo 'xmq_server was not found.' >&2; exit 1; }
fi
case "$server" in /*) ;; *) server="$(pwd)/$server" ;; esac
root=$("$server" --configuration-root)
indices=
if [ "$#" -eq 1 ]; then
    index=1
    while [ "$index" -le "$count" ]; do
        indices="$indices $index"
        index=$((index + 1))
    done
else
    for directory in "$root"/xmq[0-9]*; do
        [ -d "$directory" ] || continue
        index=${directory##*/xmq}
        case "$index" in ''|0*|*[!0-9]*) continue ;; esac
        [ "${#index}" -le 5 ] && [ "$index" -le 65535 ] || continue
        indices="$indices $index"
    done
    [ -n "$indices" ] || { echo "No numbered configuration directories found under $root." >&2; exit 1; }
    # indices contains validated integers only, never paths or other shell input.
    indices=$(printf '%s\n' $indices | sort -n)
fi
for index in $indices; do
    directory=$root/xmq$index
    [ -f "$directory/xmq_server.conf" ] || { echo "Missing configuration: $directory/xmq_server.conf" >&2; exit 1; }
    [ -w "$directory" ] || { echo "Configuration directory is not writable: $directory" >&2; exit 1; }
done
lock=$root/.xmq-start-instances.lock
mkdir "$lock" 2>/dev/null || { echo "Another launch is active, or a stale lock needs inspection: $lock" >&2; exit 1; }
trap 'rmdir "$lock"' 0
trap 'exit 130' INT
trap 'exit 143' TERM
if running=$(pgrep -x xmq_server); then :; else
    status=$?
    [ "$status" -eq 1 ] || { echo 'Cannot check running XMQ servers.' >&2; exit 1; }
    running=
fi
started=
for index in $indices; do
    directory=$root/xmq$index
    configuration=$directory/xmq_server.conf
    found=
    for pid in $running; do
        arguments=$(ps -p "$pid" -o args= 2>/dev/null) || continue
        case " $arguments " in
            *" -c $configuration "*|*" --configuration-file $configuration "*|*" --configuration-file=$configuration "*|\
            *" -c xmq$index "*|*" --configuration-file xmq$index "*|*" --configuration-file=xmq$index "*)
                found=$pid; break ;;
        esac
    done
    if [ -n "$found" ]; then
        printf 'xmq%s: already running, PID %s\n' "$index" "$found"
        continue
    fi
    ( cd "$directory"; exec nohup "$server" -c "$configuration" ) \
        </dev/null >"$directory/xmq_start.log" 2>&1 &
    pid=$!
    printf '%s\n' "$pid" >"$directory/xmq_server.pid"
    printf 'xmq%s: started PID %s, log %s\n' "$index" "$pid" "$directory/xmq_start.log"
    started="$started $index"
done
[ -n "$started" ] || { echo 'All selected instances are already running.'; exit 0; }
# Launch all missing nodes first, then observe them together.
remaining=30
while [ "$remaining" -gt 0 ]; do
    pending=0
    for index in $started; do
        directory=$root/xmq$index
        pid=$(cat "$directory/xmq_server.pid")
        if ! kill -0 "$pid" 2>/dev/null; then
            echo "xmq$index exited; inspect $directory/xmq_start.log. Other instances remain running." >&2
            exit 1
        fi
        if grep -Eq 'MQTT server did not start|lease.*refus|Segmentation fault|core dumped' "$directory/xmq_start.log"; then
            echo "xmq$index failed startup; inspect $directory/xmq_start.log. Other instances remain running." >&2
            exit 1
        fi
        if ! grep -Fq 'Server started.' "$directory/xmq_start.log"; then pending=1; fi
    done
    [ "$pending" -ne 0 ] || { echo 'All newly launched instances reported Server started.'; exit 0; }
    sleep 1
    remaining=$((remaining - 1))
done
echo 'Startup was not confirmed within 30 seconds. Instances remain running; inspect their xmq_start.log files.' >&2
exit 1
