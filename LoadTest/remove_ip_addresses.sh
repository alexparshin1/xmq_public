#!/usr/bin/env bash
# Removes secondary IP addresses added by make_ip_addresses.sh.
set -euo pipefail

usage() {
  cat <<EOF
Usage: $(basename "$0") [-i iface] [-s subnet] [-p prefix_len] [-f first] [-c count]

  -i  interface to remove addresses from (default: autodetect via default route)
  -s  subnet, first three octets (default: 10.0.1)
  -p  CIDR prefix length (default: 24)
  -f  first host octet (default: 101)
  -c  number of addresses to remove (default: 14)

Example: $(basename "$0") -i ens5 -s 10.0.1 -p 24 -f 101 -c 14
EOF
}

iface=""
subnet="10.0.1"
prefix_len=24
first=101
count=14

while getopts "i:s:p:f:c:h" opt; do
  case "$opt" in
    i) iface="$OPTARG" ;;
    s) subnet="$OPTARG" ;;
    p) prefix_len="$OPTARG" ;;
    f) first="$OPTARG" ;;
    c) count="$OPTARG" ;;
    h) usage; exit 0 ;;
    *) usage; exit 1 ;;
  esac
done

if [[ -z "$iface" ]]; then
  # The interface holding this subnet, not the one holding the default route: with a VPN up the
  # default route is a tunnel, and this would then look for the addresses on the wrong interface
  # and report having removed nothing. The companion script chooses the same way.
  iface=$(ip -o -4 addr show | awk -v s="${subnet}." '$4 ~ "^"s { print $2; exit }')
fi

if [[ -z "$iface" ]]; then
  iface=$(ip -o -4 route show to default | awk '$5 !~ /^(tun|tap|ppp|wg)/ { print $5; exit }')
  if [[ -z "$iface" ]]; then
    echo "Could not autodetect an interface for ${subnet}.0/${prefix_len}; pass -i <iface>" >&2
    exit 1
  fi
fi

last=$((first + count - 1))
echo "Removing ${count} addresses ${subnet}.${first}-${last}/${prefix_len} from ${iface}"

for i in $(seq "$first" "$last"); do
  ip_addr="${subnet}.${i}/${prefix_len}"
  if ! ip -4 addr show dev "$iface" | grep -q " ${subnet}.${i}/"; then
    echo "  ${ip_addr} not present on ${iface}, skipping"
    continue
  fi
  sudo ip addr del "$ip_addr" dev "$iface"
  #echo "  removed ${ip_addr}"
done
