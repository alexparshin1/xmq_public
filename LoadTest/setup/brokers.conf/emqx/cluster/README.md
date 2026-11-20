# Four-node EMQX cluster on the home stand

This is the functional stand for the cluster comparison. Published numbers come from
AWS, where the cluster has four separate `c5n.2xlarge` instances; here two nodes share
each of two 8-core hosts.

## Version and licence

**EMQX 5.8.9, open source (Apache 2.0).** It is the last open-source release in the
`emqx/emqx` package repository and the only one that may be clustered without a
commercial licence.

EMQX Enterprise 6.3.2 was installed first and cannot be used here: its community
licence permits a single node, and the second node fails at startup with

```text
{application_start_failure,emqx_license,
 {"SINGLE_NODE_LICENSE, make sure this node and peer nodes are configured with a valid license", ...}}
```

That is a licence limit, not a configuration error. A licensed Enterprise cluster
would need a commercial key; the 6.3.2 single-node results with the community licence
remain valid only as single-node results.

## Topology

| Node | Host | Address | MQTT | MQTT+TLS | Dashboard |
|---|---|---|---|---|---|
| emqx1 | thinker11 | 10.1.1.243 | 1882 | 8882 | 18082 / 18084 |
| emqx2 | thinker11 | 10.1.1.243 | 1883 | 8883 | 18083 / 18085 |
| emqx3 | theater | 10.1.1.242 | 1882 | 8882 | 18082 / 18084 |
| emqx4 | theater | 10.1.1.242 | 1883 | 8883 | 18083 / 18085 |

All four are core nodes. Ports repeat across hosts but are unique within a host.
Erlang distribution uses epmd on 4369 and one port pair per node (the nodes came up
on 4371/5371 and 4372/5372; no fixed range is configured).

Cluster name `emqxcl`, cookie `emqxsecretcookie`, discovery by static seeds:

```text
emqx1@10.1.1.243, emqx2@10.1.1.243, emqx3@10.1.1.242, emqx4@10.1.1.242
```

## Installation and optimisation

Each node is a `systemd` instance of `emqx-node@.service` with its own environment
file in `/etc/emqx/nodes/`. Data and logs are per node under
`/var/lib/emqx/<node>` and `/var/log/emqx/<node>`. The packaged single-node
`emqx.service` is disabled.

### Operating system

`99-emqx-cluster.conf` (installed as `/etc/sysctl.d/zz-emqx-cluster.conf`, so it
overrides the older bench files):

- file descriptors and Erlang ports: `fs.file-max` and `fs.nr_open` 16,777,216;
- connection backlog: `somaxconn`, `tcp_max_syn_backlog`, `netdev_max_backlog`
  65,535; listener backlog 16,384;
- ephemeral ports `1024-65535`, `tcp_tw_reuse`, shorter `tcp_fin_timeout`,
  larger TIME_WAIT table;
- socket buffers 16 MB maximum, BBR congestion control with `fq`;
- `tcp_slow_start_after_idle=0`, `tcp_mtu_probing=1`, `vm.swappiness=10`.

Each unit has `LimitNOFILE=8388608`. The `emqx` user also has a matching
`/etc/security/limits.d/99-emqx-limits.conf` for manual starts.

### Erlang VM

The hosts have 8 cores and run two nodes each, so each node is pinned to four
schedulers to avoid two Erlang VMs oversubscribing the same cores:

```text
ERL_FLAGS=+S 4:4 +SDcpu 2:2 +SDio 4
```

On AWS, where each node owns a whole 8-vCPU instance, this line should be omitted
so each node uses all eight schedulers. Node limits:

```text
node.process_limit    = 4194304
node.max_ports        = 4194304
node.max_ets_tables   = 524288
node.dist_buffer_size = 32768
```

### Listeners and MQTT

```text
listeners.tcp.default.bind             = 0.0.0.0:1882 or :1883
listeners.tcp.default.max_connections  = 2000000
listeners.tcp.default.acceptors        = 64
listeners.tcp.default.enable_authn     = false
tcp_options.backlog                    = 16384
tcp_options.buffer                     = 4KB
tcp_options.nodelay                    = true
tcp_options.keepalive                  = 120,30,3
listeners.ssl.default                  = same, on 8882 or 8883
listeners.ws.default / wss.default     = disabled
```

Authentication is off because these scenarios measure the broker, not a login; the
XMQ side runs the same way for the cluster comparison. Retainer is off, and logs are
at warning level. MQTT limits are raised for high-rate QoS 1 fan-in and fan-out:

```text
mqtt.max_inflight                    = 128
mqtt.max_mqueue_len                  = 100000
force_shutdown.max_mailbox_size      = 100000
```

## Verification

```sh
# four nodes, none stopped
sudo -u emqx bash -c 'set -a; . /etc/emqx/nodes/node1.env; emqx ctl cluster status'

# each node uses four schedulers on this stand
for n in 1 2 3 4; do
  sudo -u emqx bash -c "set -a; . /etc/emqx/nodes/node$n.env; \
    emqx eval 'erlang:system_info(schedulers_online)'"
done

# listeners
ss -ltn | grep -E ':(1882|1883|8882|8883|18082|18083)\b'
```

Cross-node routing was checked by subscribing on `10.1.1.242:1882` and publishing on
`10.1.1.243:1882`; the message arrived.

## AWS mapping

For the published run, use four `c5n.2xlarge` nodes, one per instance, MQTT 1882,
MQTT+TLS 8882, dashboard 18082, static seeds with the four private addresses. Do not
copy the four-scheduler `ERL_FLAGS` line there; each node has the whole instance.
Everything else in this document (limits, listeners, MQTT settings, warning logs)
applies unchanged.
