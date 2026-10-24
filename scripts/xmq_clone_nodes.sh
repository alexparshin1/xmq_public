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
installed_root=$("$server" --configuration-root)
root=$installed_root
source=$root/xmq/xmq_server.conf
if [ "$#" -eq 2 ]; then
    case "$2" in
        */*)
            source_directory=$(CDPATH= cd -P "$(dirname "$2")" && pwd)
            source=$source_directory/$(basename "$2")
            root=$(dirname "$source_directory") ;;
        *) source=$root/$2/xmq_server.conf ;;
    esac
fi
# Only moving the live source needs downtime. Other instances may keep running.
if [ ! -f "$root/xmq1/xmq_server.conf" ]; then
    command -v pgrep >/dev/null || { echo 'pgrep is required.' >&2; exit 1; }
    if running=$(pgrep -x xmq_server); then :; else
        status=$?
        [ "$status" -eq 1 ] || { echo 'Cannot check running XMQ servers.' >&2; exit 1; }
        running=
    fi
    for pid in $running; do
        if arguments=$(ps -p "$pid" -o args= 2>/dev/null); then :; else
            if kill -0 "$pid" 2>/dev/null; then
                echo "Cannot inspect XMQ PID $pid; cannot safely move $source." >&2
                exit 1
            fi
            continue
        fi
        uses_source=0
        case " $arguments " in
            *" -c $source "*|*" -c=$source "*|*" --configuration-file $source "*|*" --configuration-file=$source "*)
                uses_source=1 ;;
        esac
        # ps prints relative arguments as supplied. Resolve them against the process's
        # working directory, not the directory from which this script was invoked.
        process_directory=$(readlink "/proc/$pid/cwd" 2>/dev/null) || process_directory=
        if [ -z "$process_directory" ] && command -v procstat >/dev/null; then
            process_directory=$(procstat -f "$pid" 2>/dev/null | awk '$3 == "cwd" { for (i=10; i<=NF; i++) printf "%s%s", (i>10?" ":""), $i; print "" }')
        fi
        if [ -n "$process_directory" ]; then
            common=$process_directory
            up=
            while [ "$common" != / ]; do
                case "$source" in "$common"/*) break ;; esac
                common=${common%/*}
                common=${common:-/}
                up=../$up
            done
            if [ "$common" = / ]; then relative=$up${source#/}; else relative=$up${source#"$common"/}; fi
            case " $arguments " in
                *" -c $relative "*|*" -c ./$relative "*|*" --configuration-file $relative "*|*" --configuration-file ./$relative "*|*" --configuration-file=$relative "*|*" --configuration-file=./$relative "*|*" -c=$relative "*|*" -c=./$relative "*) uses_source=1 ;;
            esac
        fi
        if [ "$source" = "$installed_root/xmq/xmq_server.conf" ]; then
            case " $arguments " in
                *" -c xmq "*|*" -c=xmq "*|*" --configuration-file xmq "*|*" --configuration-file=xmq "*) uses_source=1 ;;
                *" -c "*|*" -c="*|*" --configuration-file "*|*" --configuration-file="*) : ;;
                *) uses_source=1 ;;
            esac
        fi
        if [ "$uses_source" -eq 1 ]; then
            echo "Stop xmq_server PID $pid using $source before moving it to xmq1. Other instances may keep running." >&2
            exit 1
        fi
    done
fi
if [ "$#" -eq 2 ]; then
    exec "$server" --clone-nodes "$count" -c "$2"
else
    exec "$server" --clone-nodes "$count"
fi
