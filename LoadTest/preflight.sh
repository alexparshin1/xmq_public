#!/usr/bin/env bash
# Verify a load-test host is tuned before a run. Every check here corresponds to a limit
# that has silently corrupted a run at least once - see setup/README.md for the symptoms.
#
#   ./preflight.sh                 # check this host (client role)
#   ./preflight.sh -H <host>       # check a remote host over ssh
#   ./preflight.sh -H <host> -r server -p 1884
#
# Exits non-zero if any REQUIRED check fails, so it can gate a run.
set -uo pipefail

remote=""; role="client"; port=""
usage() { echo "Usage: $(basename "$0") [-H host] [-r client|server] [-p port]"; exit 0; }
while [[ $# -gt 0 ]]; do
  case "$1" in
    -H|--host) remote="$2"; shift 2 ;;
    -r|--role) role="$2"; shift 2 ;;
    -p|--port) port="$2"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

run() {
  if [[ -n "$remote" ]]; then ssh -o ConnectTimeout=10 -o BatchMode=yes "$remote" "$1" 2>/dev/null
  else bash -c "$1" 2>/dev/null; fi
}

fails=0; warns=0
label="${remote:-$(hostname)} [$role]"
echo "=== preflight: $label ==="

# $1 description, $2 actual, $3 expected-minimum-or-exact, $4 mode: min|exact|note
check() {
  local desc=$1 got=$2 want=$3 mode=${4:-min}
  local ok=1
  case "$mode" in
    min)   [[ -n "$got" ]] && [[ "$got" =~ ^[0-9]+$ ]] && (( got >= want )) || ok=0 ;;
    exact) [[ "$got" == "$want" ]] || ok=0 ;;
  esac
  if (( ok )); then printf "  ok    %-34s %s\n" "$desc" "$got"
  else printf "  FAIL  %-34s %s (want %s %s)\n" "$desc" "${got:-<empty>}" "$mode" "$want"; ((fails++)); fi
}
note() { printf "  warn  %-34s %s\n" "$1" "$2"; ((warns++)); }
info() { printf "  --    %-34s %s\n" "$1" "$2"; }

# sysctl lives in /usr/sbin and is NOT on the non-root PATH on these hosts; a bare
# "sysctl" returns empty, which reads exactly like "not configured".
SC=/usr/sbin/sysctl

check "fs.nr_open"                 "$(run "$SC -n fs.nr_open")"                      2000000
check "fs.file-max"                "$(run "$SC -n fs.file-max")"                     2000000
check "net.core.somaxconn"         "$(run "$SC -n net.core.somaxconn")"              32768
check "net.ipv4.tcp_max_syn_backlog" "$(run "$SC -n net.ipv4.tcp_max_syn_backlog")"  16384
check "tcp_tw_reuse (1, not 2)"    "$(run "$SC -n net.ipv4.tcp_tw_reuse")"           1     exact

# conntrack only matters where the module is loaded at all (Docker/libvirt pull it in).
if [[ -n "$(run 'lsmod | grep -w nf_conntrack')" ]]; then
  info "nf_conntrack" "loaded - exemption required"
  check "nf_conntrack_max"         "$(run "$SC -n net.netfilter.nf_conntrack_max")"  1000000
  nt=$(run "sudo nft list table ip raw 2>/dev/null | grep -c notrack")
  check "NOTRACK rules"            "${nt:-0}"                                        4
else
  info "nf_conntrack" "not loaded - no exemption needed"
fi

# Descriptor limits. The per-user profile trap is the one that cost a day: "ulimit -n"
# sets the HARD limit too, so a low value there cannot be raised again by the run.
hard=$(run "bash -lc 'ulimit -Hn'")
if [[ -z "$remote" ]] && [[ "$hard" =~ ^[0-9]+$ ]] && (( hard < 2000000 )); then
  # Run locally, this measures the CURRENT process tree, not a fresh login: a hard limit
  # can only ever be lowered, so if the shell invoking preflight.sh was started before
  # ~/.profile was fixed, every child inherits the old cap however correct the host is.
  # Only a genuinely new login session - or the remote path below - shows the real value.
  note "login shell hard nofile" "$hard inherited by this session; re-login (or use -H) to see the host's real limit"
else
  check "login shell hard nofile"  "$hard"                                           2000000
fi
prof=$(run "grep -hoE 'ulimit -n [0-9]+' ~/.profile ~/.bashrc 2>/dev/null | grep -oE '[0-9]+' | sort -n | head -1")
if [[ -n "$prof" ]] && (( prof < 2000000 )); then
  printf "  FAIL  %-34s %s\n" "profile ulimit -n" "$prof (caps every run; see setup/README.md)"; ((fails++))
else
  printf "  ok    %-34s %s\n" "profile ulimit -n" "${prof:-not set}"
fi

if [[ "$role" == "server" ]]; then
  [[ -n "$port" ]] && {
    bl=$(run "ss -ltn | awk '\$4 ~ /:$port\$/ {print \$3; exit}'")
    info "listen backlog on :$port" "${bl:-not listening}"
    [[ -z "$bl" ]] && { printf "  FAIL  %-34s %s\n" "broker listening on :$port" "no"; ((fails++)); }
  }
  for svc in xmq_server emqx mosquitto; do
    p=$(run "systemctl show -p MainPID --value $svc 2>/dev/null"); p=${p:-0}
    [[ "$p" == "0" ]] && p=$(run "pgrep -x $svc | head -1")
    [[ -n "$p" ]] && info "$svc nofile" "$(run "grep 'open files' /proc/$p/limits | awk '{print \$4}'")"
  done
else
  # scope global drops loopback, which can never be a source address for the broker.
  sec=$(run "ip -4 -o addr show scope global | awk '{print \$4}' | grep -c '/'")
  info "local IPv4 addresses" "${sec:-?} (need >1 for >64K connections to one broker)"

  # Source-port capacity. Two traps here, each of which has cost a 1M run:
  #
  # 1. ip_local_port_range is applied at runtime by "run_load_test.sh --apply-sysctl" and
  #    is NOT persisted unless setup/sysctl.d/port_range.conf is installed. A reboot then
  #    silently reverts to the kernel default 32768-60999 - 28232 ports per address
  #    instead of 64512. A 1M run died at client-846,576, which is 30 x 28232 exactly.
  # 2. __inet_hash_connect() walks candidate ports with a step of 2, taking one parity
  #    only; the other half is reserved for bind()-time allocation. A client using
  #    IP_BIND_ADDRESS_NO_PORT - SPTK does, on every outbound connect - therefore reaches
  #    only HALF the range unless it also sets IP_LOCAL_PORT_RANGE. Past that half the
  #    connect rate collapses ~90x instead of failing outright, so it reads as a broker
  #    slowdown rather than a client limit.
  #
  # Both ceilings are reported: compare the relevant one against the scenario's
  # client_count before blaming the broker.
  range=$(run "$SC -n net.ipv4.ip_local_port_range")
  read -r lo hi <<<"$range"
  ports=0
  [[ "$lo" =~ ^[0-9]+$ && "$hi" =~ ^[0-9]+$ ]] && (( hi >= lo )) && ports=$(( hi - lo + 1 ))
  info "ip_local_port_range" "${range:-<empty>}"
  check "ports per source address"   "$ports"                                          60000
  if (( ports > 0 )) && [[ "$sec" =~ ^[0-9]+$ ]] && (( sec > 0 )); then
    info "max connections to one broker" "$(( ports * sec ))"
    info "  if SPTK predates the fix" "$(( ports / 2 * sec )) (connect() reaches one parity)"
  fi
fi

mem=$(run "awk '/MemAvailable/{printf \"%.1f\", \$2/1048576}' /proc/meminfo")
info "MemAvailable" "${mem} GB"
info "cores" "$(run nproc)"

echo "--- $label: $fails failed, $warns warnings ---"
exit $(( fails > 0 ? 1 : 0 ))
