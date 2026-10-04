#!/usr/bin/env bash
# Turns the broker's persistence on or off, for the one scenario in a set that needs it on.
#
# Why it stops the broker first: xmq_server rewrites its own configuration file shortly after it
# starts, so an edit made underneath a running broker is racing that write. Stopped, edited,
# started is the only order in which the setting is certainly the one that took effect - and the
# file is read back afterwards to say so.
#
# Meant for --setup-cmd of run_scenario_set.sh:
#   --setup-cmd './set_persistence.sh --host 10.0.0.1 --for "$XMQ_SCENARIO"'

set -u

host=""
config="/etc/xmq/xmq_server.conf"
service="xmq_server"
port="1883"
mode=""

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] (on | off | --for SCENARIO)

  on | off              What to set persistence to.
  --for SCENARIO        Decide from the scenario's file name: on for a *persistent* scenario,
                        off for every other. This is the convention the standing sets use, kept
                        in one place rather than repeated in each caller.

Options:
  --host HOST           Broker host, over ssh. Default: the local machine.
  --config PATH         Broker configuration file (default: $config).
  --service NAME        systemd unit (default: $service).
  --port N              Port to wait for after starting (default: $port).
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    on|off)     mode="$1"; shift ;;
    --for)      case "$2" in *persistent*) mode="on" ;; *) mode="off" ;; esac; shift 2 ;;
    --host)     host="$2"; shift 2 ;;
    --config)   config="$2"; shift 2 ;;
    --service)  service="$2"; shift 2 ;;
    --port)     port="$2"; shift 2 ;;
    -h|--help)  usage; exit 0 ;;
    *)          echo "Unknown argument: $1" >&2; usage; exit 1 ;;
  esac
done
[[ -z "$mode" ]] && { echo "Say on, off, or --for SCENARIO." >&2; usage; exit 1; }

run() { if [[ -n "$host" ]]; then ssh -o BatchMode=yes "$host" "$@"; else bash -c "$*"; fi; }

enabled=$([[ "$mode" == "on" ]] && echo true || echo false)

# clean_start goes with it: a persistence run measures storage from empty, and a database left
# holding the previous run's sessions is not that.
run "sudo -n systemctl stop $service" || exit 1
run "sudo -n jq '.persistence.enabled = $enabled | .persistence.clean_start = true' $config \
     > /tmp/xmq_conf.\$\$ && sudo -n cp /tmp/xmq_conf.\$\$ $config && rm -f /tmp/xmq_conf.\$\$" || {
  echo "could not edit $config" >&2; run "sudo -n systemctl start $service"; exit 1; }
run "sudo -n systemctl start $service" || exit 1

target="${host:-localhost}"
up=0
# A refused connection returns at once, so without the pause the 60 tries took well under a second
# and a broker restoring persistent state was declared down while it was still starting.
for _ in $(seq 1 60); do
  if timeout 2 bash -c ": >/dev/tcp/$target/$port" 2>/dev/null; then up=1; break; fi
  sleep 1
done
[[ "$up" -eq 0 ]] && { echo "$target:$port never accepted after the restart" >&2; exit 1; }

# Read back, because the whole point of stopping first was to be sure.
actual=$(run "sudo -n jq -r '.persistence.enabled' $config")
if [[ "$actual" != "$enabled" ]]; then
  echo "persistence is '$actual' in $config, asked for '$enabled' - the broker overwrote it" >&2
  exit 1
fi
echo "    persistence $mode (persistence.enabled=$actual, clean_start=true) on ${host:-this host}"
