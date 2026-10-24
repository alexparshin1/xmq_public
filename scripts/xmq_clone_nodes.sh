#!/bin/sh
# Compatible with Bash and the native Linux/FreeBSD /bin/sh.
set -eu
usage() { echo "Usage: ${0##*/} N [source/xmq_server.conf]" >&2; exit 2; }
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || usage
case "$1" in ''|*[!0-9]*) usage ;; esac
[ "${#1}" -le 5 ] || usage
count=$1
while [ "${count#0}" != "$count" ]; do count=${count#0}; done
count=${count:-0}
[ "$count" -ge 1 ] && [ "$count" -le 65535 ] || usage
script_directory=$(CDPATH= cd -P "$(dirname "$0")" && pwd)
server=${XMQ_SERVER:-"$script_directory/xmq_server"}
if [ ! -x "$server" ]; then
    [ -z "${XMQ_SERVER:-}" ] || { echo "XMQ_SERVER is not executable: $server" >&2; exit 1; }
    server=$(command -v xmq_server) || { echo 'xmq_server was not found.' >&2; exit 1; }
fi
root=$("$server" --configuration-root)
if [ "$#" -eq 2 ]; then
    case "$2" in */*) root=$(dirname "$(dirname "$2")") ;; esac
fi
# Only the first rename needs downtime. Adding clones reads xmq1 without changing it.
if [ ! -f "$root/xmq1/xmq_server.conf" ]; then
command -v pgrep >/dev/null || { echo 'pgrep is required.' >&2; exit 1; }
if pgrep -x xmq_server >/dev/null; then
    echo 'Stop all xmq_server processes before moving and cloning configurations.' >&2
    exit 1
else
    status=$?
    [ "$status" -eq 1 ] || { echo 'Cannot check running XMQ servers.' >&2; exit 1; }
fi
fi
if [ "$#" -eq 2 ]; then
    exec "$server" --clone-nodes "$count" -c "$2"
else
    exec "$server" --clone-nodes "$count"
fi
