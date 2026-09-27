#!/usr/bin/env bash
# Adds secondary IP addresses to an interface so xmq_scn can bind clients
# across multiple source IPs (see 1M-Connection-Test-Plan.md, section 3).
set -euo pipefail

usage() {
  cat <<EOF
Usage: $(basename "$0") [-i iface] [-s subnet] [-p prefix_len] [-f first] [-c count] [-v]

  -i  interface to add addresses to (default: autodetect via default route)
  -s  subnet, first three octets (default: 10.0.1)
  -p  CIDR prefix length (default: 24)
  -f  first host octet (default: 101)
  -c  number of addresses to add (default: 14)
  -v  verbose: print each address as it's added or found already present
      (default: just the summary line)

Example: ./make_ip_addresses.sh -s 172.31.14 -p 24 -f 150 -c 14

EOF
}

iface=""
subnet="10.0.1"
prefix_len=24
first=101
count=14
verbose=0

while getopts "i:s:p:f:c:vh" opt; do
  case "$opt" in
    i) iface="$OPTARG" ;;
    s) subnet="$OPTARG" ;;
    p) prefix_len="$OPTARG" ;;
    f) first="$OPTARG" ;;
    c) count="$OPTARG" ;;
    v) verbose=1 ;;
    h) usage; exit 0 ;;
    *) usage; exit 1 ;;
  esac
done

if [[ -z "$iface" ]]; then
  # The interface that already holds an address in this subnet, before anything else. These
  # addresses exist to be bound as sources for traffic to a broker on the same subnet, so the
  # interface that reaches it is the one that carries it - which the default route does not
  # answer: with a VPN up, the default route is tun0, and 29 addresses went onto it. They were
  # then unreachable from the LAN, and the run bound sources that could not carry its traffic.
  iface=$(ip -o -4 addr show | awk -v s="${subnet}." '$4 ~ "^"s { print $2; exit }')
fi

if [[ -z "$iface" ]]; then
  # Nothing on this subnet yet, so fall back to the default route - skipping tunnels, which are
  # never what is wanted here.
  iface=$(ip -o -4 route show to default | awk '$5 !~ /^(tun|tap|ppp|wg)/ { print $5; exit }')
  if [[ -z "$iface" ]]; then
    echo "Could not autodetect an interface for ${subnet}.0/${prefix_len}; pass -i <iface>" >&2
    exit 1
  fi
fi

last=$((first + count - 1))
echo "Adding or verifying ${count} addresses ${subnet}.${first}-${last}/${prefix_len} to ${iface}.."

conflicts=()

# Preflight: an address already leased to another live host on the LAN (e.g. by DHCP) will
# silently blackhole any connection that binds to it here - peers keep resolving it to the
# other host's MAC, not this interface's, so replies never arrive back. Ping every candidate
# up front, in parallel (one -W1 ping each, sequentially that's ~1s x count), and skip rather
# than add on top of a device that's already answering.
# The addresses the interface already carries, read once, up front.
#
# NOT "ip addr show | grep -q" per address, which is what this used to do: grep -q exits the
# moment it matches, ip dies of SIGPIPE, and with pipefail (set above) the pipeline then reports
# failure - so an address that IS present is intermittently reported missing. Measured on a busy
# machine: two false misses in twenty checks. The second loop then waited on a ping the first loop
# had never started ("ping_pid[$i]: unbound variable") and took three load-test runs down with it.
existing_addrs=$(ip -4 -o addr show dev "$iface" | awk '{ split($4, a, "/"); print a[1] }')

has_addr() {
  grep -qxF -- "$1" <<< "$existing_addrs"
}

declare -A ping_pid
for i in $(seq "$first" "$last"); do
  if has_addr "${subnet}.${i}"; then
    continue # already present on iface, skip re-adding and pinging it
  fi
  ping -c 1 -W 1 "${subnet}.${i}" >/dev/null 2>&1 &
  ping_pid[$i]=$!
done

for i in $(seq "$first" "$last"); do
  ip_addr="${subnet}.${i}/${prefix_len}"
  if has_addr "${subnet}.${i}"; then
    [[ "$verbose" -eq 1 ]] && echo "  ${ip_addr} already present on ${iface}, skipping"
    continue
  fi

  if wait "${ping_pid[$i]}"; then
    echo "  WARNING: ${subnet}.${i} already answers on the network (in use by another host) - skipping" >&2
    conflicts+=("${subnet}.${i}")
    continue
  fi

  sudo ip addr add "$ip_addr" dev "$iface"
  [[ "$verbose" -eq 1 ]] && echo "  added ${ip_addr}"
done

if [[ ${#conflicts[@]} -gt 0 ]]; then
  echo "Skipped ${#conflicts[@]} address(es) already in use by other hosts: ${conflicts[*]}" >&2
fi
