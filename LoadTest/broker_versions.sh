#!/usr/bin/env bash
# Print the version of each MQTT broker installed on this host, as "Version:" lines
# ready to paste into a results file under results/.
#
# Results files are assembled by hand from xmq_scn output plus a few meta lines, and the
# broker version belongs with them: without it a published latency figure cannot be
# reproduced later, and a re-run months on may silently compare against a different build
# (the broker install scripts here do not pin versions).
#
# Usage:
#   ./broker_versions.sh                 # brokers on this host
#   ./broker_versions.sh -H <host>       # brokers on a remote host, over ssh
#   ./broker_versions.sh -s XMQ          # just one broker
#
# The output keys match what the web pages parse, so a line can be pasted directly
# beneath the matching "Server:" line:
#
#   Server:   EMQX
#   Version:  5.8.9

set -uo pipefail

remote=""
only=""

usage() {
  cat <<EOF
Usage: $(basename "$0") [-H host] [-s server]

  -H, --host HOST     Query a remote host over ssh instead of this machine.
  -s, --server NAME   Only report this broker (XMQ, EMQX, Mosquitto, NanoMQ).
  -h, --help          Show this help.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -H|--host) remote="$2"; shift 2 ;;
    -s|--server) only="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage; exit 1 ;;
  esac
done

# Runs a command locally, or on the remote host when -H was given.
run() {
  if [[ -n "$remote" ]]; then
    ssh -o ConnectTimeout=10 -o BatchMode=yes "$remote" "$1" 2>/dev/null
  else
    bash -c "$1" 2>/dev/null
  fi
}

want() {
  [[ -z "$only" ]] || [[ "${only,,}" == "${1,,}" ]]
}

emit() {
  printf 'Server:   %s\nVersion:  %s\n\n' "$1" "$2"
}

if want XMQ; then
  v=$(run 'xmq_server --version 2>/dev/null')
  # Builds before --version existed print the version only in the --help banner, and abort
  # on an unrecognised option rather than reporting one - so fall back to parsing that.
  [[ -n "$v" ]] || v=$(run 'xmq_server --help 2>/dev/null | head -1 | sed -n "s/^XMQ Server \(.*\)$/\1/p"')
  [[ -n "$v" ]] && emit XMQ "$v"
fi

if want EMQX; then
  v=$(run 'dpkg-query -W -f="\${Version}" emqx 2>/dev/null')
  [[ -z "$v" ]] && v=$(run 'emqx_ctl status 2>/dev/null | sed -n "s/.*v\([0-9][0-9.]*\).*/\1/p" | head -1')
  [[ -n "$v" ]] && emit EMQX "$v"
fi

if want Mosquitto; then
  v=$(run 'dpkg-query -W -f="\${Version}" mosquitto 2>/dev/null')
  [[ -z "$v" ]] && v=$(run 'mosquitto -h 2>&1 | sed -n "s/^mosquitto version \([0-9.]*\).*/\1/p" | head -1')
  [[ -n "$v" ]] && emit Mosquitto "$v"
fi

if want NanoMQ; then
  v=$(run 'dpkg-query -W -f="\${Version}" nanomq 2>/dev/null')
  [[ -z "$v" ]] && v=$(run 'nanomq --version 2>&1 | sed -n "s/.*\([0-9]\+\.[0-9]\+\.[0-9]\+\).*/\1/p" | head -1')
  [[ -n "$v" ]] && emit NanoMQ "$v"
fi

# A broker simply not being installed is not an error, so don't inherit the exit status
# of the last "is it present" test.
exit 0
