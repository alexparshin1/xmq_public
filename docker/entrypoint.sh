#!/bin/sh
# Prepares a container's /etc/xmq and starts the server in the foreground.
#
# The server can create its own configuration, but then there is nothing to edit
# before it starts. Seeding the file here instead means the environment variables
# below are applied to the first run as well as to later ones.
set -eu

CONF=/etc/xmq/xmq_server.conf
TEMPLATE=/etc/xmq/xmq_server.conf.template
CERTS=/etc/xmq/certs

if [ ! -f "$CONF" ]; then
    # ${ProgramCerts} is expanded by the server when it writes its own configuration;
    # a copied template has to have it substituted here.
    sed "s|\${ProgramCerts}|$CERTS|g" "$TEMPLATE" > "$CONF"
    echo "xmq: wrote a starting configuration to $CONF"
fi

# The TLS listener on 8883 and the web interface on 18883 both refuse to start
# without a certificate. A self-signed one keeps the container usable out of the
# box; mount your own over /etc/xmq/certs for anything real.
if [ ! -f "$CERTS/node.crt" ] || [ ! -f "$CERTS/node.key" ]; then
    mkdir -p "$CERTS"
    openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
        -keyout "$CERTS/node.key" -out "$CERTS/node.crt" \
        -subj "/CN=${XMQ_CERT_CN:-xmq}" >/dev/null 2>&1
    echo "xmq: generated a self-signed certificate in $CERTS"
fi

# Environment overrides. Nothing here touches logging: the broker always writes to
# stdout as well as to its file, and stdout is where docker logs reads it from.
# clean_start defaults to false here, unlike the packaged configuration, which starts
# from a clean store every time. Someone who sets XMQ_PERSISTENCE=true is asking for
# state to outlive the process, and wiping it on startup is the opposite of that.
# allow_anonymous defaults to true here, unlike the packaged configuration: someone who
# runs the image is trying the broker out, and a first attempt that fails on credentials
# nobody has been told about is the shortest way to lose them. It is the wrong default the
# moment the container is reachable from anywhere else - hence the warning printed below,
# and XMQ_ALLOW_ANONYMOUS=false to turn it off.
TMP=$(mktemp)
jq --argjson anon "${XMQ_ALLOW_ANONYMOUS:-true}" \
   --argjson persistence "${XMQ_PERSISTENCE:-false}" \
   --argjson clean "${XMQ_CLEAN_START:-false}" \
   --arg redis "${XMQ_REDIS_URI:-redis://redis_server:6379}" \
   --arg level "${XMQ_LOG_LEVEL:-INFO}" \
   '.authentication.allow_anonymous = $anon
    | .persistence.enabled = $persistence
    | .persistence.clean_start = $clean
    | .persistence.redis_uri = $redis
    | .logging.min_log_level = $level' \
   "$CONF" > "$TMP"
mv "$TMP" "$CONF"

if [ "${XMQ_ALLOW_ANONYMOUS:-true}" = "true" ]; then
    echo "xmq: anonymous access is ON (the default) - set XMQ_ALLOW_ANONYMOUS=false before this container is reachable from a network you do not control"
fi

# The broker writes the log to stdout AND to a file. In a container that file is a
# copy of what docker logs already has, and nothing rotates it, so it grows inside
# the container until the disk fills. Pointing it at /dev/null keeps stdout as the
# only log. Set XMQ_LOG_FILE=keep to have the file as well - worth mounting
# /var/log/xmq somewhere in that case, so it is not stuck in the container layer.
if [ "${XMQ_LOG_FILE:-discard}" = "discard" ]; then
    LOG_TO=$(jq -r '.logging.log_to' "$CONF")
    case "$LOG_TO" in
        /*) LOG_PATH="$LOG_TO" ;;
        *)  LOG_PATH="/var/log/xmq/$LOG_TO" ;;
    esac
    mkdir -p "$(dirname "$LOG_PATH")"
    if [ ! -e "$LOG_PATH" ] || [ -L "$LOG_PATH" ]; then
        ln -sf /dev/null "$LOG_PATH"
    fi
fi

# Until the administrator has a password the configuration interface answers on the container's
# own loopback, which a published port does not reach. The password is set once, from outside,
# and kept in the /etc/xmq volume, as on any other installation:
#
#   docker exec -it <container> xmq_server --set-password admin
#   docker restart <container>

# No flag needed to stay in the foreground, which is what a container wants: the broker
# runs where it is started. It used to fork itself into the background unless told not
# to, and Docker read the parent exiting as the container having finished.
exec xmq_server "$@"
