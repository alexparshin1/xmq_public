/**
 * Explanations, ranges, and suggested values for the XMQ configuration parameters.
 *
 * Entries are keyed "<configuration section>.<field>", and the screens reference those keys
 * rather than carrying the wording themselves. Keeping it in one file means the help text,
 * the input bounds, and the "this looks unwise" warnings stay together and can't drift apart.
 *
 * Entry fields:
 *   title      Popup window title.
 *   text       Array of paragraphs.
 *   range      Human-readable valid range, shown in the popup. Optional.
 *   suggested  Human-readable suggested value, shown in the popup. Optional.
 *   min, max   Bounds applied to the input itself. Optional.
 *   warning    Function of the current value, returning a note to show next to the field,
 *              or null when there is nothing to say. Used for values that are allowed but
 *              rarely a good idea. Optional.
 */
const ParameterHelp = {

    "persistence.enabled": {
        title: "Persistence",
        text: [
            "Stores client sessions, their subscriptions, and undelivered messages in Redis, " +
            "so that they survive a server restart.",
            "When persistence is off, the server keeps all of that in memory only, and nothing " +
            "is recovered after a restart."
        ]
    },

    "persistence.redis_host": {
        title: "Redis host and port",
        text: [
            "Address of the Redis server holding the persisted state.",
            "Redis may run on the XMQ host or on a separate one. A remote Redis adds a network " +
            "round trip to every persisted write, so it is worth keeping the two close."
        ],
        suggested: "localhost:6379"
    },

    "persistence.redis_username": {
        title: "Redis credentials",
        text: [
            "Username and password sent when connecting to Redis.",
            "Leave both empty for a Redis that requires no authentication. For a Redis with a " +
            "password but no ACL users, fill in the password and leave the username empty."
        ]
    },

    "persistence.max_redis_connections": {
        title: "Max Redis connections",
        text: [
            "Upper bound on the Redis connection pool. XMQ uses one connection per thread that " +
            "touches Redis, so this needs to cover the persistence threads plus the session " +
            "threads that read stored state.",
            "Redis itself handles many connections cheaply, so a value that is somewhat too high " +
            "costs little; too low makes threads wait for each other."
        ],
        range: "1 .. 512",
        suggested: "32",
        min: 1,
        max: 512,
        warning: (value) => value > 256
                            ? "Very few deployments need more than 256 Redis connections."
                            : null
    },

    "persistence.clean_start": {
        title: "Clean start",
        text: [
            "Discards this node's persisted state when the server starts: its sessions and their " +
            "queued messages. Other nodes' state is left alone - a clean start never flushes the " +
            "whole Redis database.",
            "Useful for test environments that should begin from a known state. On a production " +
            "server it means every durable session is lost on each restart, which is usually the " +
            "opposite of why persistence was enabled."
        ],
        suggested: "off, except in test environments"
    },

    "web_service.listener_port": {
        title: "Service port number",
        text: [
            "TCP port serving this configuration interface and the control API behind it, over " +
            "HTTPS.",
            "It must differ from every MQTT listener port. Anyone who can reach it can attempt " +
            "to sign in, so on a public host it is worth restricting to a trusted network at the " +
            "firewall.",
            "The certificate is the server's own unless one has been installed at the paths in " +
            "the web_service section of the configuration. A self-signed certificate is warned " +
            "about by every browser; the server logs its fingerprint at startup so that the " +
            "warning can be answered by checking rather than by clicking through."
        ],
        range: "1024 .. 65535",
        suggested: "18883",
        min: 1024,
        max: 65535
    },

    "initial_setup.admin_password": {
        title: "Administrator account",
        text: [
            "The account this configuration interface is administered through. Its name is " +
            "always \"admin\": setup rewrites the account list, and this is the account it " +
            "leaves, so the configuration it produces always comes with a way in.",
            "It is an interface account, not an MQTT one. MQTT clients authenticate against the " +
            "accounts on the Users page.",
            "Changing it ends the current session, since the session carries the password it " +
            "signed in with. Sign in again with the new one.",
            "On a server being set up for the first time it does one more thing: a freshly " +
            "installed admin account has no password, and the interface answers on the server's " +
            "own machine alone for as long as that is true. Setting a password here is what " +
            "opens the interface to the network."
        ]
    },

    "initial_setup.node_name": {
        title: "Node name",
        text: [
            "Name this server is known by. It identifies the node in its own log, in the cluster " +
            "it belongs to, and in the client identifiers its bridges connect with.",
            "It must be unique among the servers this one is bridged or clustered with: two nodes " +
            "sharing a name displace each other's sessions. Naming it after the host it runs on, " +
            "with the MQTT port when a host runs more than one, keeps that straightforward.",
            "Letters, digits, dots, dashes, and underscores, starting with a letter, digit, or " +
            "underscore, and at least two characters long."
        ],
        suggested: "the host name, e.g. mqtt-primary"
    },

    "initial_setup.node_host": {
        title: "Node address",
        text: [
            "The host name or address other machines reach this server at. Different from the " +
            "node name, which only labels it: this one has to resolve from the machines that " +
            "connect here.",
            "Two things are built from it. Other nodes in a cluster are told to connect to this " +
            "address, and the certificate issued during setup is made out to it - a certificate " +
            "naming a host nobody connects by proves nothing to the node that connects by another.",
            "It is offered as this machine's own host name, which is right for a server reached " +
            "only from itself and usually wrong for one in a cluster. No port: the ports are set " +
            "below and are appended to this."
        ],
        suggested: "the name other machines resolve, e.g. mqtt-1.example.net"
    },

    "initial_setup.mqtt_port": {
        title: "MQTT port",
        text: [
            "TCP port for unencrypted MQTT connections.",
            "1883 is the registered MQTT port, and what clients try when told nothing else."
        ],
        range: "1024 .. 65535",
        suggested: "1883",
        min: 1024,
        max: 65535
    },

    "initial_setup.mqtt_ssl_port": {
        title: "MQTT+SSL port",
        text: [
            "TCP port for MQTT over TLS.",
            "8883 is the registered port for it. The listener needs a server certificate and key " +
            "to accept connections; install them under SSL Keys once the server is set up."
        ],
        range: "1024 .. 65535",
        suggested: "8883",
        min: 1024,
        max: 65535
    },

    "ssl_keys.verify_depth": {
        title: "Verify depth",
        text: [
            "How far up a client's certificate chain the server will follow when verifying it.",
            "0 turns client certificate verification off entirely: clients are not asked for a " +
            "certificate, and the TLS listener only proves the server's own identity. Above 0, " +
            "clients must present a certificate that the CA certificate above validates, within " +
            "this many intermediate signers."
        ],
        range: "0 .. 10",
        suggested: "0, unless clients authenticate by certificate"
    },

    "web_service.encrypted": {
        title: "Serve over HTTPS",
        text: [
            "Whether this configuration interface is served over TLS.",
            "It carries the administrator's password on every request, and everything the server " +
            "is configured with in both directions. On a network where anyone can watch the " +
            "traffic, turning this off hands them the password.",
            "Turning it on needs no preparation: if no certificate has been installed, the server " +
            "issues one to itself. Changing this takes effect at once, on the same port, and the " +
            "browser has to be sent to the other scheme afterwards - which this page does."
        ],
        suggested: "on, unless something in front of the server already terminates TLS"
    },

    "web_service.certfile": {
        title: "Interface certificate",
        text: [
            "The certificate this configuration interface is served with, which is what browsers " +
            "check before they will show the page without a warning.",
            "It is a different certificate from the broker's. The two are reached under different " +
            "names as often as not - clients connect to the MQTT port by one name, an " +
            "administrator opens this page by another - and a certificate replaced for one " +
            "should not silently change the other. Pointing both at the same file is a supported " +
            "arrangement, not a requirement.",
            "Until one is installed, the server issues a certificate to itself. That is enough to " +
            "encrypt the connection, and it is not enough to remove the browser's warning: only a " +
            "certificate from an authority the browser already trusts does that."
        ],
        suggested: "a certificate naming the host this interface is opened by"
    },

    "web_service.keyfile": {
        title: "Interface key",
        text: [
            "The private key belonging to the interface certificate.",
            "It is installed together with the certificate: the two have to match, and a key that " +
            "does not go with the certificate leaves the interface unable to answer at all.",
            "The server stores it readable by its own account only. It never leaves the machine, " +
            "and nothing in this interface will show it back."
        ],
        suggested: "the key the certificate was issued for"
    },

    "authentication.allow_anonymous": {
        title: "Allow anonymous",
        text: [
            "Lets MQTT clients connect without a username and password.",
            "Convenient on a closed network, and a way for anyone who can reach the port to " +
            "publish and subscribe. It does not affect this interface, which always requires " +
            "signing in."
        ],
        suggested: "off, unless the server is reachable only from a trusted network"
    },

    "listener.threads": {
        title: "Listener thread count",
        text: [
            "Threads accepting and serving connections on this listener.",
            "These share the host's physical cores with the send, receive, and persistence " +
            "threads, and no group should exceed the physical core count. Count every listener: " +
            "two listeners with four threads each are eight threads, not four."
        ],
        range: "1 .. 64, and no more than the host's physical core count",
        suggested: "4",
        min: 1,
        max: 64,
        warning: (value) => value > 8
                            ? "Check the host: thread groups should not exceed its physical core count."
                            : null
    },

    "listener.bind_ip": {
        title: "Bind IP address",
        text: [
            "Which local address the listener accepts connections on.",
            "0.0.0.0 accepts on every interface of the host. A specific address restricts the " +
            "listener to that interface, which is a way to keep a port on an internal network " +
            "only. Empty means the same as 0.0.0.0."
        ],
        suggested: "0.0.0.0"
    },

    "listener.protocol": {
        title: "Listener protocol",
        text: [
            "MQTT is a plain TCP listener. MQTT+SSL is the same protocol over TLS, using the " +
            "certificates from the SSL Keys page.",
            "An MQTT+SSL listener is not created if those keys are missing or fail to load: the " +
            "server logs an error and carries on without it."
        ]
    },

    "bridge.mode": {
        title: "Bridge mode",
        text: [
            "Which way messages travel over this bridge.",
            "\"out\" forwards messages published on this server to the remote one. \"in\" subscribes " +
            "to the remote server and republishes what it sends here. \"inout\" does both.",
            "A topic can narrow this further: a topic with its own direction is carried only that " +
            "way, whatever the bridge mode says."
        ],
        suggested: "out, unless the remote server also has messages this one needs"
    },

    "bridge.client_id": {
        title: "Client ID",
        text: [
            "MQTT client identifier this bridge connects with.",
            "It must stay the same across reconnects: with a non-clean session, the remote broker " +
            "keys the bridge's stored subscriptions and queued messages by this id, and a changing " +
            "id leaves orphaned sessions behind on the remote.",
            "Left empty, XMQ derives one from the two node names."
        ],
        suggested: "leave empty unless the remote requires a particular id"
    },

    "bridge.clean_session": {
        title: "Clean session",
        text: [
            "Whether the bridge asks the remote broker to forget its session on disconnect.",
            "Off is what a bridge normally wants: the remote keeps the subscriptions and queues " +
            "messages while the link is down, so a brief outage doesn't lose them. On means the " +
            "session starts empty each time, and anything published while disconnected is gone."
        ],
        suggested: "off"
    },

    "bridge.encrypted": {
        title: "Encrypted bridge connection",
        text: [
            "Connects to the remote broker over TLS, using the key files named below.",
            "Unlike the server's own SSL Keys page, these are paths to files already on the XMQ " +
            "host, not uploads: /etc/xmq/certs on Linux, C:/ProgramData/xmq/certs on Windows. The " +
            "remote broker must be listening on its TLS port."
        ]
    },

    "bridge.topic_pattern": {
        title: "Topic pattern",
        text: [
            "MQTT topic filter selecting the messages this entry carries, such as sensors/# or " +
            "site/+/status.",
            "Topics keep the name they were published with: there is no rewriting between the two " +
            "servers.",
            "A bridge with no topics carries nothing, so at least one entry is needed."
        ],
        suggested: "the narrowest filter that covers what the remote needs"
    },

    "logging.log_to": {
        title: "Log to file",
        text: [
            "Path of the server log file, on the machine running XMQ.",
            "The default log directory is /var/log/xmq on Linux and C:/ProgramData/xmq/logs on " +
            "Windows. The server must be able to write there: on a packaged install that means a " +
            "directory owned by the account XMQ runs as.",
            "Rotating the file is left to the system's own log rotation."
        ],
        suggested: "xmq_server.log in the platform's log directory"
    },

    "logging.min_log_level": {
        title: "Minimum log level",
        text: [
            "A ceiling on how verbose any subject can be, not a floor.",
            "Each subject below has its own level, and the server clips it to this one. With this " +
            "set to INFO, a subject asking for DEBUG still logs at INFO, so this single setting " +
            "turns the whole log down without touching the individual subjects.",
            "Levels run PANIC, ERROR, WARNING, NOTICE, INFO, DEBUG, from quietest to most verbose."
        ],
        suggested: "INFO in production, DEBUG while investigating"
    },

    "logging.publish": {
        title: "Publish and Ack logging",
        text: [
            "These two subjects produce a log line per message, not per client.",
            "At the message rates XMQ is built for, DEBUG here can generate far more log volume " +
            "than the rest of the server put together, and the writing itself costs throughput. " +
            "Worth turning up only while chasing a specific problem."
        ],
        suggested: "ERROR, raised temporarily when investigating"
    },

    "server_limits.send_threads": {
        title: "Message send threads",
        text: [
            "Threads delivering messages to subscribers.",
            "Testing shows no thread group should exceed the number of physical cores on the " +
            "server host. The groups share those cores, so count the send threads, the receive " +
            "threads, the persistence threads, and the per-listener threads together, not each " +
            "on its own.",
            "XMQ pins its threads to physical cores by default, so adding threads beyond the core " +
            "count buys contention rather than throughput."
        ],
        range: "1 .. 64, and no more than the host's physical core count",
        suggested: "3",
        min: 1,
        max: 64,
        warning: (value) => value > 8
                            ? "Check the host: thread groups should not exceed its physical core count."
                            : null
    },

    "server_limits.receive_threads": {
        title: "Message receive threads",
        text: [
            "Threads reading messages published by clients. When the broker has no persistent store, " +
            "no bridge and no cluster node, they also match and deliver what they read.",
            "The same rule as the send threads: no group should exceed the host's physical core " +
            "count, and all the groups share those cores. Use auto to run four."
        ],
        range: "1 .. 64, and no more than the host's physical core count",
        suggested: "4",
        min: 1,
        max: 64,
        warning: (value) => value > 8
                            ? "Check the host: thread groups should not exceed its physical core count."
                            : null
    },

    "server_limits.delivery_threads": {
        title: "Message delivery threads",
        text: [
            "Threads delivering queued messages to subscribers.",
            "The delivery pool is separate from the send and receive pools. Keep the count at or " +
            "below the number of physical cores shared by all of the server's thread groups.",
            "Use auto to run two delivery workers."
        ],
        range: "1 .. 256, or auto",
        suggested: "auto",
        min: 1,
        max: 256,
        allowAuto: true,
        warning: (value) => value > 8
                            ? "Check the host: all thread groups share the physical cores."
                            : null
    },

    "server_limits.max_topic_alias": {
        title: "Max topic alias",
        text: [
            "Highest topic alias an MQTT 5 client may use on a connection.",
            "Aliases let a client send a long topic name once and refer to it by number afterwards. " +
            "The server holds the alias table per connection, so a large value costs memory on " +
            "every connection."
        ],
        range: "1 .. 65534",
        suggested: "128",
        min: 1,
        max: 65534
    },

    "server_limits.max_packet_size": {
        title: "Max packet size",
        text: [
            "Largest MQTT packet the server accepts, in bytes. Clients are told this limit when " +
            "they connect, and a larger packet is rejected.",
            "This bounds what a single client can make the server buffer, so it is a memory " +
            "safeguard as much as a protocol setting."
        ],
        range: "1024 .. 268435456 (256 MB)",
        suggested: "268435456 (256 MB)",
        min: 1024,
        max: 268435456
    },

    "queue_limits.max_size": {
        title: "Max queue size",
        text: [
            "Most messages held for one client session before further messages are dropped.",
            "This is a per-session bound, so the worst case is this many messages times the number " +
            "of sessions. With persistence enabled the queued messages are in Redis; without it " +
            "they are in server memory."
        ],
        range: "2 .. 10000000",
        suggested: "50000",
        min: 2,
        max: 10000000
    },

    "queue_limits.max_inflight_messages": {
        title: "Max inflight messages",
        text: [
            "How many QoS 1 and QoS 2 messages may be awaiting acknowledgement on a session at once.",
            "Higher values keep the link busy while acknowledgements are in flight, which matters " +
            "on slow or distant connections. Lower values bound how much a slow consumer can make " +
            "the server hold."
        ],
        range: "1 .. 65535",
        suggested: "32768",
        min: 1,
        max: 65535
    },

    "persistence.max_queued_writes": {
        title: "Max queued writes",
        text: [
            "How many messages may be delivered while their Redis records are still being written.",
            "At 0 every message waits for its own record to become durable before it is sent. " +
            "That is the safest setting, and the slowest: a single Redis connection answers about " +
            "38,000 unpipelined writes per second, against about 600,000 pipelined.",
            "Above 0, record writes pipeline instead, and a record whose message is acknowledged " +
            "within a millisecond is never written at all. The value is also how many messages " +
            "could be lost if the server were killed outright - on top of what Redis itself loses " +
            "when its machine goes down, which its appendfsync setting decides.",
            "When the window is full, the publisher's connection is not read until Redis catches " +
            "up: the publisher slows down, nobody else does."
        ],
        range: "0 .. 100000",
        suggested: "1000 (the default): at 60,000 messages a second that is under 20 ms of traffic",
        min: 0,
        max: 100000,
        warning: (value) => value > 1000
                            ? "A crash could lose up to this many messages."
                            : null
    }
};

/**
 * Returns the help entry for a parameter, or null when the parameter has no help defined.
 * @param {String} helpKey    Parameter key, "<section>.<field>"
 * @return {Object|null} the help entry
 */
export function parameterHelp(helpKey)
{
    if (!helpKey)
    {
        return null;
    }
    return ParameterHelp.hasOwnProperty(helpKey) ? ParameterHelp[helpKey] : null;
}

export default ParameterHelp;
