#!/usr/bin/env bash
#
# TLS listeners on the competing brokers of the bench host, the way the TLS Point-To-Point
# scenarios (tls_p2p_campaign.sh) expect them. Written down from what was done by hand on the home
# bench on 2026-10-15; run it on the AWS broker instance, and again on any new bench host.
#
#   ./brokers_tls_setup.sh <ssh host>              apply (idempotent; every file it changes is copied to *.bak-before-tls once)
#   ./brokers_tls_setup.sh <ssh host> --check      only say what is there
#
# One self-signed certificate for all of them, /etc/bench-tls/bench.{crt,key,p12}, the key readable
# by everyone: a closed bench, and the load client verifies nothing. TLS ports:
#
#     XMQ 8884 (its own configuration)   FlashMQ 8885   Mosquitto 8883   EMQX 8882   HiveMQ CE 8886
#
set -u
host=${1:?ssh host}
check=0; [ "${2:-}" = "--check" ] && check=1
addr_ip=${BENCH_ADDR:-}            # the address the certificate names besides the host name

ssh -o BatchMode=yes "$host" "CHECK=$check ADDR_IP='$addr_ip' bash -s" <<'REMOTE'
set -u
say() { echo "[$(hostname)] $*"; }
backup() { [ -f "$1" ] && [ ! -f "$1.bak-before-tls" ] && sudo -n cp -p "$1" "$1.bak-before-tls"; return 0; }

if [ "$CHECK" = 1 ]; then
    for f in /etc/bench-tls/bench.crt /etc/flashmq/flashmq.conf /etc/mosquitto/conf.d/tls.conf /etc/emqx/emqx.conf /opt/hivemq-ce-*/conf/config.xml; do
        printf '%s: ' "$f"; sudo -n grep -c "bench-tls\|8885\|8883\|8882\|tls-tcp-listener" $f 2>/dev/null || echo missing
    done
    exit 0
fi

# --- the certificate
if [ ! -f /etc/bench-tls/bench.crt ]; then
    sudo -n mkdir -p /etc/bench-tls
    san="DNS:$(hostname)"; [ -n "$ADDR_IP" ] && san="$san,IP:$ADDR_IP"
    (cd /etc/bench-tls &&
     sudo -n openssl req -x509 -newkey rsa:2048 -nodes -keyout bench.key -out bench.crt -days 365 \
         -subj "/CN=$(hostname)" -addext "subjectAltName=$san" 2>/dev/null &&
     sudo -n openssl pkcs12 -export -in bench.crt -inkey bench.key -out bench.p12 -name bench -passout pass:benchpass)
    sudo -n chmod 755 /etc/bench-tls; sudo -n chmod 644 /etc/bench-tls/bench.*
    say "certificate made"
fi

# --- FlashMQ, 8885
if [ -f /etc/flashmq/flashmq.conf ] && ! sudo -n grep -q "port 8885" /etc/flashmq/flashmq.conf; then
    backup /etc/flashmq/flashmq.conf
    sudo -n tee -a /etc/flashmq/flashmq.conf > /dev/null <<E

listen {
  protocol mqtt
  inet_protocol ip4
  port 8885
  fullchain /etc/bench-tls/bench.crt
  privkey /etc/bench-tls/bench.key
  allow_anonymous true
  tcp_nodelay     true
}
E
    say "FlashMQ: TLS listener 8885"
fi

# --- Mosquitto, 8883. Its AppArmor profile reads /etc/mosquitto and not /etc/bench-tls.
if [ -d /etc/mosquitto/conf.d ] && [ ! -f /etc/mosquitto/conf.d/tls.conf ]; then
    sudo -n cp /etc/bench-tls/bench.crt /etc/bench-tls/bench.key /etc/mosquitto/certs/
    sudo -n chown mosquitto:mosquitto /etc/mosquitto/certs/bench.*; sudo -n chmod 640 /etc/mosquitto/certs/bench.key
    sudo -n tee /etc/mosquitto/conf.d/tls.conf > /dev/null <<E
listener 8883 0.0.0.0
certfile /etc/mosquitto/certs/bench.crt
keyfile /etc/mosquitto/certs/bench.key
allow_anonymous true
E
    say "Mosquitto: TLS listener 8883"
fi

# --- EMQX, 8882: tuned as the plain listener is - at the defaults (16 acceptors, backlog 1024) a burst
# of handshakes at 10,000 connections a second loses clients (24972 of 25000 connected, 2026-10-15).
if [ -f /etc/emqx/emqx.conf ] && ! sudo -n grep -q "bench-tls" /etc/emqx/emqx.conf; then
    backup /etc/emqx/emqx.conf
    sudo -n python3 - <<'E'
import re
p = "/etc/emqx/emqx.conf"
s = open(p).read()
block = '''listeners.ssl.default {
    bind            = "0.0.0.0:8882"
    max_connections = 2000000
    acceptors       = 64
    tcp_options {
        backlog   = 8192
        buffer    = 4KB
        keepalive = "120,30,3"
    }
    ssl_options {
        certfile = "/etc/bench-tls/bench.crt"
        keyfile  = "/etc/bench-tls/bench.key"
    }
}'''
if "listeners.ssl.default" in s:
    s = re.sub(r"listeners\.ssl\.default\s*\{.*?\n\}", block, s, count=1, flags=re.S)
else:
    s += "\n" + block + "\n"
open(p, "w").write(s)
E
    say "EMQX: TLS listener 8882"
fi

# --- HiveMQ CE, 8886: a PKCS12 keystore
for c in /opt/hivemq-ce-*/conf/config.xml; do
    [ -f "$c" ] || continue
    if ! sudo -n grep -q "tls-tcp-listener" "$c"; then
        backup "$c"
        sudo -n python3 - "$c" <<'E'
import sys
p = sys.argv[1]
s = open(p).read()
tls = '''        <tls-tcp-listener>
            <port>8886</port>
            <bind-address>0.0.0.0</bind-address>
            <tls>
                <keystore>
                    <path>/etc/bench-tls/bench.p12</path>
                    <password>benchpass</password>
                    <private-key-password>benchpass</private-key-password>
                </keystore>
            </tls>
        </tls-tcp-listener>
    </listeners>'''
assert s.count("    </listeners>") == 1
open(p, "w").write(s.replace("    </listeners>", tls))
E
        say "HiveMQ: TLS listener 8886"
    fi
done
REMOTE
