# XMQ MQTT server
#
# Installs the released .deb rather than building from source, so the image holds
# exactly the binaries published on xmq.sptk.net.
#
#   docker build -t xmq:0.9.19 docker/
#   docker run -p 1883:1883 xmq:0.9.19
#
# Debian forky is the base because the package needs libstdc++6 >= 15 and
# libc6 >= 2.38, and the forky build of the package is made against exactly it.
# Trixie still carries libstdc++6 14, so it cannot run this build.

FROM debian:forky-slim

ARG XMQ_VERSION=0.9.19
ARG SPTK_VERSION=5.6.13
ARG XMQ_DISTRO=debian-forky
ARG XMQ_DEB_URL=https://xmq.sptk.net/download/SPTK-${SPTK_VERSION}/${XMQ_DISTRO}/xmq-server_${XMQ_VERSION}_amd64.deb

# openssl and jq stay in the image: the entrypoint generates the certificate the
# TLS listener needs and edits the configuration from environment variables.
#
# The broker's own unit tests ship in the server package and are the largest file in it.
# They test the build machine, not this container, so they go; the load generators
# (xmq_scn, xmq_pub, xmq_sub, xmq_con) stay, because running them against a broker is
# something someone may well want to do from here.
RUN set -eux; \
    apt-get update; \
    apt-get install -y --no-install-recommends ca-certificates curl jq openssl; \
    curl -fsSL -o /tmp/xmq.deb "${XMQ_DEB_URL}"; \
    apt-get install -y --no-install-recommends /tmp/xmq.deb; \
    rm -f /tmp/xmq.deb; \
    rm -f /etc/systemd/system/xmq_server.service /etc/systemd/system/xmq_test_suite.service; \
    rm -f /usr/local/bin/xmq_unit_tests; \
    apt-get purge -y curl; \
    apt-get autoremove -y; \
    rm -rf /var/lib/apt/lists/*

# 0.9.15 and later ship no accounts file at all - the server writes one on first start,
# with no password on the administrator until someone sets it. This removal is kept for
# the older versions XMQ_VERSION can still be pointed at: up to 0.9.14 the package
# carried an accounts file with known passwords, and leaving that in a public image
# would mean every container starting with credentials taken from the internet.
RUN rm -f /etc/xmq/xmq_users.conf

COPY entrypoint.sh /usr/local/bin/xmq-entrypoint.sh
RUN chmod +x /usr/local/bin/xmq-entrypoint.sh

# The server writes its configuration, accounts and certificate here, so this is the
# directory to mount if any of it should outlive the container.
VOLUME /etc/xmq

# 1883 MQTT, 8883 MQTT over TLS, 18883 the configuration web interface
EXPOSE 1883 8883 18883

# Runs as root by default so a mounted /etc/xmq owned by any user stays writable.
# Pass --user to run unprivileged; the ports are all above 1024.
ENTRYPOINT ["/usr/local/bin/xmq-entrypoint.sh"]
