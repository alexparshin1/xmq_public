#!/bin/sh

# The server host and port:
export XMQ_SERVER_HOST=203.0.113.10
export XMQ_SERVER_PORT=1883

# The creation of the extra IPs:
export XMQ_CLIENT_SUBNET="203.0.113"
export XMQ_CLIENT_FIRST=150
export XMQ_CLIENT_COUNT=29
export XMQ_CLIENT_PREFIX="vm1-"

# The MQTT credentials, empty by default: the campaign brokers accept anonymous clients.
# A run against a cluster takes them from the environment (see Cluster/test_cluster.sh).
export XMQ_CLIENT_USER="${XMQ_CLIENT_USER:-}"
export XMQ_CLIENT_PASSWORD="${XMQ_CLIENT_PASSWORD:-}"
