#!/usr/bin/env bash
# Full client-side load test pipeline, safe to run right after login on a
# client box VM: checks the server is reachable, optionally applies client OS
# tuning, brings up the secondary bind IPs, then runs xmq_scn. Assumes
# xmq_server is already running on the server host (see 1M-Connection-Test-Plan.md).
#
# The server host and port can be optionally defined by the environment variables
# XMQ_SERVER_HOST and XMQ_SERVER_PORT that change the script defaults.
#
# The creation of the extra IP addresses can be optionally defined by the environment variables
# XMQ_CLIENT_SUBNET, XMQ_CLIENT_FIRST, and XMQ_CLIENT_COUNT.
#
# The example environment setup is init_environment_aws.sh.
# It can be customized per clients's server host so the scenario files don't require
# any changes.

set -euo pipefail

# Nothing on a desktop machine stops it going to sleep in the middle of a half-hour test, and
# when it does the run does not fail: the connections die, the broker falls idle, and the client
# reports "Not connected" some minutes later, which reads exactly like a broker fault. It cost a
# 30-minute run here on 2026-09-06 and is the likeliest explanation for two older records that
# say only "did not complete". Re-exec under an idle inhibitor, once.
if [[ -z "${XMQ_SLEEP_INHIBITED:-}" ]] && command -v systemd-inhibit > /dev/null 2>&1; then
  export XMQ_SLEEP_INHIBITED=1
  # Re-exec by absolute path through bash, not by "$0": systemd-inhibit execs its command
  # directly, so a script invoked by a bare name ("bash run_load_test.sh", which is how it is run
  # by hand) is looked up in PATH and not found - "Failed to execute". Called from
  # run_scenario_set.sh the path was always absolute, which is why this hid until an AWS run.
  self="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
  # Asked for first, and only used if the answer is yes. A blocking inhibitor needs an active
  # login session, so a run started detached - by a scheduler, or with setsid - is refused by
  # polkit; exec'ing into it regardless made that refusal the end of the run rather than the loss
  # of a precaution. The probe is one throwaway inhibitor, which either succeeds or does not.
  if systemd-inhibit --what=sleep:idle --who="probe" --why="probe" --mode=block \
                     true > /dev/null 2>&1; then
    exec systemd-inhibit --what=sleep:idle --who="MQTT load test" \
         --why="a run in progress must not be suspended" --mode=block bash "$self" "$@"
  fi
  # Detached, the inhibitor is still to be had from root - and is needed most there: a run left to
  # itself is the one nobody is at the desk to keep awake. KDE suspended thinker10 eighteen minutes
  # into such a run on 2026-10-05. Held for as long as this script runs, and released with it.
  if sudo -n systemd-inhibit --what=sleep:idle --who="probe" --why="probe" --mode=block true > /dev/null 2>&1; then
    sudo -n systemd-inhibit --what=sleep:idle --who="MQTT load test" \
         --why="a run in progress must not be suspended" --mode=block sleep infinity > /dev/null 2>&1 &
    sleep_inhibitor=$!
    trap 'kill "$sleep_inhibitor" 2> /dev/null' EXIT
  else
    echo "   warning: no sleep inhibitor could be taken, so nothing here stops this machine" \
         "suspending mid-run - run_scenario_set.sh checks afterwards whether it did" >&2
  fi
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XMQ_SCN="xmq_scn"

usage() {
  cat <<EOF
Usage: $(basename "$0") [options] <scenario file.json>

Client identity / scenario:
  --id-prefix PREFIX        xmq_scn --id-prefix, e.g. vm1- or vm2- (default: vm1-).
  --scenario FILE           Scenario JSON, relative to to this directory or absolute path.

Secondary-IP setup (passed through to make_ip_addresses.sh):
  --iface IFACE             Interface to bind to, default: autodetect.
  --subnet SUBNET           Subnet to bind to, default: 172.31.14 or ${XMQ_CLIENT_SUBNET}.
  --prefix-len LEN          Subnet prefix length, default: 24.
  --first OCTET             Subnet bind first IP address octet, default: 150 or ${XMQ_CLIENT_FIRST}.
  --count N                 The number of IP addresses to bind to, default: 29 or ${XMQ_CLIENT_COUNT}.
  --skip-ip-setup           Don't run make_ip_addresses.sh (e.g. already done this session).

OS tuning:
  --apply-sysctl            Apply client sysctl tuning (default: skipped, assumed already done).

Output:
  -R, --progress            Show progress bar(s).
  -v, --verbose             Print more details.

Scenario overrides (passed through to xmq_scn; omit to use the scenario file's own values):
  -h, --host N              Override server host (default: read from scenario file, fallback 1883 or ${XMQ_SERVER_HOST}).
  -p, --port N              Override server port (default: read from scenario file, fallback 1883 or ${XMQ_SERVER_PORT}).
  -u, --user <username>     Username for the server.
  -P, --password <password> Password for the server.
  -r, --connection-rate N   Client connection rate.
  -q, --qos N               Client connection QOS.
  -s, --payload-size N      Client PUBLISH message payload size.
  -k, --keep-alive SECONDS  Client keep alive, seconds.
  --duration SECONDS        Limit test duration to, seconds.
  --max-inflight N          Cap each client's un-acked QoS1/2 messages (throttles publishers against
                            their own ack latency instead of a broker that can't keep up)

  -h, --help                Show this help.
EOF
}

id_prefix=${XMQ_CLIENT_PREFIX:-vm1-}
scenario="100K-Connections-2500-rate.json"
iface=""
subnet=${XMQ_CLIENT_SUBNET:-172.31.14}
prefix_len=24
first=${XMQ_CLIENT_FIRST:-150}
count=${XMQ_CLIENT_COUNT:-29}
skip_ip_setup=0
apply_sysctl=0
server_host=${XMQ_SERVER_HOST:-}
server_port=${XMQ_SERVER_PORT:-1883}
connection_rate=""
qos="1"
payload_size=""
keep_alive=""
duration=""
verbose=""
username=""
password=""
progress="1"
max_inflight=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -i|--id-prefix) id_prefix="$2"; shift 2 ;;
    -s|--scenario) scenario="$2"; shift 2 ;;
    --iface) iface="$2"; shift 2 ;;
    --subnet) subnet="$2"; shift 2 ;;
    --prefix-len) prefix_len="$2"; shift 2 ;;
    --first) first="$2"; shift 2 ;;
    --count) count="$2"; shift 2 ;;
    --skip-ip-setup) skip_ip_setup=1; shift ;;
    --apply-sysctl) apply_sysctl=1; shift ;;
    -h|--host) server_host="$2"; shift 2 ;;
    -p|--port) server_port="$2"; shift 2 ;;
    -r|--connection-rate) connection_rate="$2"; shift 2 ;;
    -q|--qos) qos="$2"; shift 2 ;;
    -x|--payload-size) payload_size="$2"; shift 2 ;;
    -k|--keep-alive) keep_alive="$2"; shift 2 ;;
    -d|--duration) duration="$2"; shift 2 ;;
    -v|--verbose) verbose=1; shift ;;
    -u|--user) username="$2"; shift 2 ;;
    -P|--password) password="$2"; shift 2 ;;
    -R|--progress) progress=1; shift ;;
    --help) usage; exit 0 ;;
    *) if [[ "$1" =~ \.json$ ]]; then
         scenario="$1"; shift 1
       else
         echo "Unknown option: $1" >&2; usage; exit 1
       fi ;;
  esac
done

scenario_path="$scenario"

if [[ -z "$server_host" ]]; then
  echo "Could not read server hostname from $scenario_path" >&2
  exit 1
fi

# Checking server reachability:
if ! timeout 5 bash -c ": >/dev/tcp/${server_host}/${server_port}" 2>/dev/null; then
  echo "Cannot reach ${server_host}:${server_port} — is xmq_server running on the server host?" >&2
  exit 1
fi

if [[ "$apply_sysctl" -eq 1 ]]; then
  # Applying client OS tuning:
  sudo sysctl -w net.ipv4.ip_local_port_range="1024 65535"
  sudo sysctl -w net.ipv4.tcp_tw_reuse=1
  sudo sysctl -w net.ipv4.tcp_max_tw_buckets=2000000
  sudo sysctl -w fs.nr_open=2000000
fi
ulimit -n 2000000 || echo "   warning: could not raise ulimit -n to 2000000 (current: $(ulimit -n)); set LimitNOFILE in a systemd unit or /etc/security/limits.conf" >&2

if [[ "$skip_ip_setup" -eq 0 ]]; then
  [ "$verbose" != "" ] && echo "Setting up secondary IP addresses:"
  ip_args=(-s "$subnet" -p "$prefix_len" -f "$first" -c "$count")
  [[ -n "$iface" ]] && ip_args+=(-i "$iface")
  [[ "$verbose" -eq 1 ]] && ip_args+=(-v)
  "$SCRIPT_DIR/make_ip_addresses.sh" "${ip_args[@]}"
else
  # Skipped, so they had better be there already. Without them xmq_scn binds what is left - one
  # address, about 28,000 ports - and every run past that many connections fails with "Server not
  # available", which reads like the broker's fault. Two whole bench sets were lost that way.
  missing=0
  for ((n = first; n < first + count; ++n)); do
    ip -4 -o addr show | grep -q " ${subnet}.${n}/" || missing=$((missing + 1))
  done
  if [[ "$missing" -gt 0 ]]; then
    echo "$missing of the $count source addresses ${subnet}.${first}-$((first + count - 1)) are not configured;" \
         "drop --skip-ip-setup or run make_ip_addresses.sh first" >&2
    exit 1
  fi
fi

bind_mask="${subnet}.${first}/${prefix_len}"

xmq_scn_args=(--scenario "$scenario_path" --bind-to-interfaces "$bind_mask" --id-prefix "$id_prefix")
[[ -n "$server_host" ]] && xmq_scn_args+=(-h "$server_host")
[[ -n "$server_port" ]] && xmq_scn_args+=(-p "$server_port")
[[ -n "$username" ]] && xmq_scn_args+=(-u "$username")
[[ -n "$password" ]] && xmq_scn_args+=(-P "$password")
[[ -n "$connection_rate" ]] && xmq_scn_args+=(--connection-rate "$connection_rate")
[[ -n "$qos" ]] && xmq_scn_args+=(--qos "$qos")
[[ -n "$payload_size" ]] && xmq_scn_args+=(--payload-size "$payload_size")
[[ -n "$keep_alive" ]] && xmq_scn_args+=(--keep-alive "$keep_alive")
[[ -n "$duration" ]] && xmq_scn_args+=(--duration "$duration")
[[ -n "$progress" ]] && xmq_scn_args+=(--progress)
[[ -n "$max_inflight" ]] && xmq_scn_args+=(--max-inflight "$max_inflight")
[[ "$verbose" -eq 1 ]] && xmq_scn_args+=(--verbose)

# Anything in XMQ_SCN_EXTRA is appended to the client's own command line. It exists for the arms of
# a calibration - --use-cpu-affinity is the one that prompted it - where two runs must differ by a
# client flag and by nothing else, and passing it through the set runner as an option would mean
# teaching every layer about a flag that belongs to the client alone.
if [[ -n "${XMQ_SCN_EXTRA:-}" ]]; then
  # shellcheck disable=SC2206
  xmq_scn_args+=( ${XMQ_SCN_EXTRA} )
fi

echo "Executing:  xmq_scn ${xmq_scn_args[*]}"
xmq_scn "${xmq_scn_args[@]}"
