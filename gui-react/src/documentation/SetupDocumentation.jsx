import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function SetupDocumentation() {
    return <DocumentationPage page="setup">
            <section>
            <h4 id="basic-setup">Basic MQTT setup</h4>

            <p>
                Everything in this section can be set from the configuration interface, and is written back to
                <code>/etc/xmq/xmq_server.conf</code>. The user data is stored in the file
                <code>/etc/xmq/xmq_users.conf</code>. Hand-editing these files is still possible;
                if you do, restart the server afterwards, since a running server rewrites the files
                when settings change and would overwrite your edit.
            </p>
            <p>
                The XMQ configuration interface is available through the browser at{" "}
                <code>https://xmq_host:18883</code>, where xmq_host is the host the XMQ server runs
                on. It is administered through the <code>admin</code> account. A server that has
                never been set up is the exception in two ways: it answers at{" "}
                <code>https://127.0.0.1:18883</code> and nowhere else, and <code>admin</code> has
                no password yet &mdash; the interface signs the administrator in by itself and
                opens the <b>Initial Setup</b> page, which is where the password is set.
            </p>

            <h5>Starting from scratch</h5>

            <p>
                The <b>Initial Setup</b> page asks for the few settings a working server cannot be
                without &mdash; an administrator password, a node name, the MQTT, MQTT+SSL, and
                interface ports, and optionally a Redis address &mdash; and builds a configuration
                around them. It is the quickest way to bring up a new installation, and the way
                back to a known state when a configuration has been edited into a corner.
            </p>

            <p>
                The <code>admin</code> account comes first on that page because it is what the
                interface is administered through: setup creates the account if it is not there,
                and gives it the password entered either way, so the configuration it produces
                always comes with a way in. Since a session carries the password it signed in with,
                changing it ends the current session &mdash; sign in again with the new password.
            </p>

            <p>
                The interface is HTTPS with a certificate the server issued itself, so the first
                visit is met with the browser's <b>Your connection is not private</b> warning
                (Chrome: <code>NET::ERR_CERT_AUTHORITY_INVALID</code>): press <b>Advanced</b> and
                <b>Proceed to ... (unsafe)</b>. Nothing is wrong - no browser trusts a
                certificate it has never seen, and a fresh installation has nothing else to
                offer yet. The <b>SSL Keys</b> page installs a certificate that a browser does
                trust when one exists, and reissues the self-signed pair when it does not.
            </p>

            <p>
                On a server being set up for the first time, that password does one more thing: it
                is what moves the interface off the loopback address. Until it is set the interface
                accepts connections from the server&apos;s own machine only, because an account
                that anyone can sign in to without a password has no business being reachable from
                a network. The account is refused everywhere else in the meantime &mdash; an MQTT
                client offering <code>admin</code> and no password is not connected.
            </p>

            <p>
                It is the one page that discards settings rather than changing them. Everything it
                does not ask about is taken from <code>/etc/xmq/xmq_server.conf.template</code>, the
                copy of the shipped configuration the installer leaves behind, so listeners,
                bridges, limits, and logging settings all go back to their defaults. The template
                itself is only read; the result is written to <code>xmq_server.conf</code>.
            </p>

            <p>
                The accounts are reset with them: <code>xmq_users.conf</code> is rewritten with
                <code>admin</code>, using the password entered on the page, and the internal
                <code>cluster</code> account, using the cluster password entered on the page.
                Enter the same cluster password on every node sharing the user database.
                Setup does not enable cluster membership; use Enable on the first node and
                Invite and Accept for the others. MQTT clients that authenticate will need their
                accounts adding again on the Users page. Installed certificate files are kept,
                being files of their own rather than configuration.
            </p>

            <p>
                Submitting restarts the MQTT server, which disconnects every client. Changing the
                interface port takes effect at once too: the interface starts answering on the new
                port before it stops answering on the old one, and the browser is sent there.
                Connections already open are served to the end, so the reply announcing the move
                still arrives. A port that cannot be taken &mdash; because something else holds it
                &mdash; leaves the interface where it was, and the page says so instead of sending
                the browser nowhere.
            </p>

            <p>
                A missing configuration is not an error: the server writes a starting one from
                <code>xmq_server.conf.template</code>, or from its own defaults if the template is
                gone too, and comes up on the ports above. A missing <code>xmq_users.conf</code> is
                written too, with an <code>admin</code> account that has no password and the
                internal <code>cluster</code> account. No accounts are installed by the package:
                a password shipped in a file is a password everyone who downloaded the package
                knows.
            </p>

            <h5>When the interface will not start</h5>

            <p>
                The Initial Setup page needs the interface to be running, which is no help when the
                configuration is what stops the server from starting: a port already taken, a
                listener bound to an address this host does not have, or a file edited into
                something that will not parse. The same reset is available from the command line,
                where nothing needs to be running:
            </p>

            <pre className="userManualCode">{`sudo systemctl stop xmq
sudo xmq_server --reset-configuration   # or start the service again afterwards`}</pre>

            <p>
                It replaces <code>xmq_server.conf</code> with the starting configuration and carries
                on into a normal start. The file it replaced is kept beside it as
                <code>xmq_server.conf.old</code>, since it is usually still worth reading. Accounts
                are not touched &mdash; they are in their own file, and are not what stops a server
                from starting &mdash; except that one for <code>admin</code> is created, without a
                password, if there are no accounts at all.
            </p>

            <h5>Port numbers</h5>

            <p>
                Listeners are configured on the <b>Listeners</b> page. Each one is a port, a
                protocol, and a thread count. The shipped configuration defines two:
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Port</th>
                    <th>Protocol</th>
                    <th>Purpose</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>1883</td>
                    <td>MQTT</td>
                    <td>Plain TCP, the standard MQTT port</td>
                </tr>
                <tr>
                    <td>8883</td>
                    <td>MQTT+SSL</td>
                    <td>TLS, the standard secure MQTT port</td>
                </tr>
                <tr>
                    <td>18883</td>
                    <td>HTTP</td>
                    <td>The configuration interface and its control API</td>
                </tr>
                </tbody>
            </table>

            <p>
                These are the standard MQTT ports, so most clients need no port setting at all.
                If another broker on the same host already holds them, either stop it or move one
                of the two: the Listeners page changes XMQ's ports, and the change takes effect as
                soon as the listener is saved.
            </p>

            <p>
                A listener also carries a bind address. <code>0.0.0.0</code> accepts on every
                interface; naming one address restricts the listener to it, which is a simple way
                to keep a port on an internal network. Disabling a listener keeps its settings but
                closes its port.
            </p>

            <p>
                The service port itself is set on the <b>Service</b> page, and changing it takes
                effect at once: the interface starts answering on the new port before it stops
                answering on the old one, and the page follows it there. Connections already open
                are served to the end. If something else already holds the port, the interface
                stays where it is and the page says so.
            </p>

            <h5>SSL certificates</h5>

            <p>
                An <code>MQTT+SSL</code> listener needs a server key and certificate. Install them
                from the <b>SSL Keys</b> page: choosing a file uploads its contents, and the server
                writes it into <code>/etc/xmq/certs</code>
                (<code>C:\ProgramData\xmq\certs</code> on Windows) and records the path. A previous
                file of the same name is kept beside the new one with an <code>.old</code> suffix.
            </p>

            <ul>
                <li><b>Server key</b> and <b>server certificate</b> are required for TLS.</li>
                <li><b>CA certificate</b> is needed only to verify client certificates.</li>
                <li>
                    <b>Verify depth</b> of 0 means clients are not asked for a certificate: the TLS
                    listener proves the server's identity and no more. Above 0, a client must
                    present a certificate that the CA certificate validates, within that many
                    intermediate signers.
                </li>
            </ul>

            <p>
                If the keys are missing or fail to load, the server logs an error, skips the
                MQTT+SSL listener, and carries on serving the plain one. That is worth checking in
                the log after installing certificates for the first time.
            </p>

            <h5>Logging</h5>

            <p>
                The <b>Logging</b> page sets the log file and the level per subject. The
                <b>minimum log level</b> is a ceiling rather than a floor: each subject has its own
                level, and the server clips it to this one. Turning the ceiling down to
                <code>INFO</code> quietens everything without touching the individual subjects,
                and turning it up to <code>DEBUG</code> lets them speak at their configured level.
            </p>

            <p>
                Levels are <code>PANIC</code>, <code>ERROR</code>, <code>WARNING</code>,
                <code>NOTICE</code>, <code>INFO</code>, <code>DEBUG</code>, from quietest to most
                verbose. Two subjects deserve care: <b>Publish</b> and <b>Acks</b> produce a line
                per message, not per client. At the rates XMQ is built for, <code>DEBUG</code>
                there can outweigh everything else the server logs and cost real throughput. Leave
                them at <code>ERROR</code> unless you are chasing something specific.
            </p>

            <p>
                Log rotation is left to the system. Session errors are always logged at
                <code>ERROR</code> and are not configurable.
            </p>

            <h5>Users and anonymous access</h5>

            <p>
                Accounts on the <b>Users</b> page serve both MQTT clients and the configuration interface. An
                administrator may change the configuration; a non-administrator can connect but not
                administer. <b>Allow anonymous</b> lets MQTT clients connect with no credentials at
                all; it does not affect the configuration interface, which always requires signing in.
            </p>

            <h5>Persistence</h5>

            <p>
                Without persistence the server keeps everything in memory and nothing survives a
                restart. With it, client sessions, their subscriptions, and undelivered QoS 1 and
                QoS 2 messages are stored in Redis. Redis is a separate service, not part of XMQ:
                install and start it yourself, then enable persistence on the <b>Persistence</b>
                page.
            </p>

            <p>
                If the server cannot reach Redis at startup it logs the error and keeps running
                with in-memory storage. The server stays up and nothing is persisted, so the log is
                worth a look after enabling it.
            </p>

            <p>
                What survives a failure is decided in two places: <b>Max queued writes</b> on that
                page bounds what the server can lose, and Redis's own <code>appendfsync</code>{" "}
                setting decides what Redis loses when its machine goes down. Redis's defaults keep
                only periodic snapshots, so turn on <code>appendonly yes</code>. The two modes worth
                using - Durable and Super-Durable - and what each can lose are described on{" "}
                <a href="https://xmq.sptk.net/xmq_persistence" target="_blank"
                   rel="noopener noreferrer">xmq.sptk.net/xmq_persistence</a>.
            </p>
            </section>
            <section>
            <h4 id="advanced-setup">Advanced setup</h4>

            <h5>MQTT bridges</h5>

            <p>
                A bridge is a client connection XMQ makes to another MQTT broker, carrying an
                agreed set of topics. The remote broker does not have to be XMQ: bridging to
                Mosquitto, EMQX, or anything else speaking MQTT works the same way. Bridges are
                configured on the <b>Bridges</b> page.
            </p>

            <p>
                A bridge has a direction, and so does each of its topics:
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Mode</th>
                    <th>Meaning</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>out</code></td>
                    <td>Messages published on this server are forwarded to the remote one</td>
                </tr>
                <tr>
                    <td><code>in</code></td>
                    <td>This server subscribes to the remote one and republishes what it sends</td>
                </tr>
                <tr>
                    <td><code>inout</code></td>
                    <td>Both directions</td>
                </tr>
                </tbody>
            </table>

            <p>
                Each topic entry carries a pattern, a direction of its own, and the QoS to
                subscribe with. A topic's direction narrows the bridge's: a topic marked
                <code>out</code> on an <code>inout</code> bridge travels outbound only. Topics keep
                the name they were published with; there is no rewriting between the two servers.
                A bridge with no topics carries nothing.
            </p>

            <h5>Things worth getting right</h5>

            <ul>
                <li>
                    <b>Client ID must be stable.</b> The remote broker keys the bridge's stored
                    subscriptions and queued messages by it. An id that changes between reconnects
                    leaves orphaned sessions accumulating on the remote. Left empty, XMQ derives one
                    from the two node names, which is stable.
                </li>
                <li>
                    <b>Leave clean session off.</b> The remote then keeps the subscriptions and
                    queues messages while the link is down, so a brief outage does not lose them.
                    With it on, the session starts empty each time and anything published while
                    disconnected is gone.
                </li>
                <li>
                    <b>Loops.</b> XMQ marks its bridge connections with an origin node, so it will
                    not echo back a message that arrived over the bridge. Bridging through a broker
                    that does not do the same, in both directions, on overlapping topics, can still
                    produce a loop.
                </li>
                <li>
                    <b>Encrypted bridges</b> use key files already present on the XMQ host, given by
                    path. That is the opposite of the SSL Keys page, which uploads content. The
                    remote broker must be listening on its TLS port.
                </li>
            </ul>

            <h5>Bridging two XMQ servers, step by step</h5>

            <p>
                This walks through joining two XMQ servers so that a client on either one sees
                traffic published on the other. Call them <b>mq-a</b> and <b>mq-b</b>; substitute
                your own host names throughout. One bridge is enough, defined on <b>mq-a</b> and
                pointing at <b>mq-b</b>, in <code>inout</code> mode - that single bridge carries
                both directions. Do not define a second one on mq-b pointing back; see the end of
                this section for why.
            </p>

            <p>
                The shipped configuration defines <b>no bridges and no cluster nodes</b>, so the
                Bridges page is empty on a fresh installation. Everything below is added by hand.
            </p>

            <p><b>Before you start</b></p>

            <ul>
                <li>Both servers installed, running, and reachable from each other on their MQTT
                    port - a bridge is an ordinary outbound MQTT connection, so a firewall between
                    the hosts needs that port open in both directions.
                </li>
                <li>Decide which topics to carry. This example uses <code>test/#</code>. Prefer a
                    specific prefix to <code>#</code>: it keeps the link's traffic to what you
                    actually meant to share.
                </li>
                <li>An account on each server for the other's bridge to log in as. The bridge
                    authenticates like any client, so it needs a real account on the
                    <em>remote</em> server. A non-administrator account is enough; create it on the
                    <b>Users</b> page if you do not already have one.
                </li>
            </ul>

            <p><b>Step 1 - add the bridge on mq-a</b></p>

            <p>
                Open <code>http://mq-a:18883</code>, sign in as an administrator, and go to the
                <b>Bridges</b> page. It is a master-detail page: the bridge list on top, and the
                settings of the selected bridge below. Press <b>Add</b>, fill the settings in, and
                press <b>Save Bridge</b>:
            </p>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Field</th>
                    <th>Value on mq-a</th>
                    <th>Notes</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td>Node name</td>
                    <td><code>mq-b</code></td>
                    <td>The name this server knows the remote by. It appears in the log and marks
                        messages that arrived over the link, which is how loops are prevented.
                        A name already used by another bridge is rejected.
                    </td>
                </tr>
                <tr>
                    <td>Host / port</td>
                    <td><code>mq-b</code> / <code>1883</code></td>
                    <td>The remote MQTT listener, not its configuration port. A second bridge to
                        the same host and port is rejected: it would carry every message twice.
                    </td>
                </tr>
                <tr>
                    <td>Mode</td>
                    <td><code>inout</code></td>
                    <td>Both directions.</td>
                </tr>
                <tr>
                    <td>Username</td>
                    <td>the account on <b>mq-b</b></td>
                    <td>Credentials for the remote server, not this one.</td>
                </tr>
                <tr>
                    <td>Password</td>
                    <td>its password</td>
                    <td></td>
                </tr>
                <tr>
                    <td>Client ID</td>
                    <td>leave empty</td>
                    <td>XMQ then derives a stable id from the two node names. Set one only if the
                        remote requires a particular id, and never one that varies.
                    </td>
                </tr>
                <tr>
                    <td>Enabled</td>
                    <td>ticked</td>
                    <td></td>
                </tr>
                <tr>
                    <td>Clean session</td>
                    <td>unticked</td>
                    <td>So mq-b keeps the subscriptions and queues messages while the link is down.</td>
                </tr>
                <tr>
                    <td>Encrypted</td>
                    <td>unticked</td>
                    <td>Tick it only when bridging to the remote's TLS port; it reveals the key
                        file fields, which are paths on this host.
                    </td>
                </tr>
                </tbody>
            </table>

            <p>
                Then fill the <b>Topics</b> table below the settings. Press its <b>Add</b> and give
                the topic a <b>Pattern</b> of <code>test/#</code>, a <b>Direction</b> of
                <code>inout</code>, and a <b>QoS</b> of 1. A bridge with no topics connects and
                carries nothing, so this table is not optional.
            </p>

            <p>
                Press <b>Save Bridge</b>. The bridge appears in the list with its topic count, and
                the configuration file is written immediately.
            </p>

            <p><b>Step 2 - nothing to do on mq-b</b></p>

            <p>
                mq-b needs no bridge of its own. It needs only the account the bridge signs in
                with, which it already has if you used one of its existing users. Nothing on mq-b
                records that a bridge exists: to it, mq-a's bridge is an ordinary client that
                subscribes and publishes.
            </p>

            <p><b>Step 3 - press Apply on each server</b></p>

            <p>
                This step is easy to miss. Saving a bridge stores it, but does <em>not</em> start
                the connection. <b>Apply</b>, on the Bridges page, rebuilds the connections from
                the configuration as it now stands: the bridges that were running are stopped, and
                the configured ones are started. Press it on <b>mq-a</b>.
            </p>

            <p>
                Neither server has to be restarted, and mq-b does not have to be up yet: a bridge
                whose remote is not answering retries on its own, the wait between attempts growing
                to half a minute and dropping back as soon as it connects.
            </p>

            <p>
                Restarting the server has the same effect, since bridges are started with it, and
                remains the way to apply a change made by editing the configuration file directly:
            </p>

            <pre className="userManualCode">{`sudo systemctl restart xmq_server`}</pre>

            <p><b>Step 4 - confirm the link came up</b></p>

            <p>
                With <b>Connections</b> at <code>DEBUG</code> on the <b>Logging</b> page, mq-a's
                log carries two lines for the bridge:
            </p>

            <pre className="userManualCode">{`Bridge to mq-b forwarding 1 outbound topic(s).
Bridge to mq-b (mq-b:1883) connected, 1 inbound topic(s).`}</pre>

            <p>
                Both counts matter. <code>connected</code> with <b>0 inbound topics</b> means the
                bridge reached the remote but carries nothing - almost always an empty or
                mistyped Topics field.
            </p>

            <p><b>Step 5 - prove a message crosses</b></p>

            <p>
                Subscribe on one server and publish on the other. The subscriber has no other way
                to see the message, so receiving it is itself the proof the bridge carried it:
            </p>

            <pre className="userManualCode">{`xmq_sub -h mq-b -p 1883 -u user -P secret -t test/bridge -q 1 -v -C 1 -W 15

xmq_pub -h mq-a -p 1883 -u user -P secret -t test/bridge -q 1 -m "over the bridge"`}</pre>

            <p>
                Then swap the two hosts and repeat, to check the other direction: one
                <code>inout</code> bridge carries both, but the two use different halves of it, and
                it is quite possible for one to work while the other does not.
            </p>

            <p>
                For a measured run rather than a single message, <code>xmq_scn</code> can publish
                to one server and subscribe on the other:
            </p>

            <pre className="userManualCode">{`xmq_scn -s Basic/Point-To-Point-1K-1K-1K-1K.json \\
        --host mq-a --port 1883 --subscriber-host mq-b --subscriber-port 1883`}</pre>

            <p>
                Every message it counts has crossed the bridge, so the reported latency includes
                the extra broker hop and the round trip between the hosts. Compare it against the
                same scenario run without <code>--subscriber-host</code> to see what the link costs.
            </p>

            <p><b>If nothing crosses</b></p>

            <ul>
                <li><b>No bridge line in the log at all</b> - <b>Apply</b> was not pressed after
                    the bridge was saved, or the bridge is not enabled. Saving stores the bridge;
                    it does not start it.
                </li>
                <li><b>Repeated connect failures</b> - the host or port is wrong, the port is
                    blocked, or the credentials name an account that does not exist on the
                    <em>remote</em> server. The log names the reason.
                </li>
                <li><b>Connected, nothing carried</b> - the topic pattern does not cover the topic
                    being published. Patterns are matched as MQTT subscriptions:
                    <code>test/#</code> covers <code>test/bridge</code>, while <code>test</code>
                    on its own does not.
                </li>
                <li><b>One direction only</b> - check the mode on both bridges. A bridge in
                    <code>out</code> mode forwards but never subscribes.
                </li>
                <li><b>Everything arrives twice</b> - a bridge is defined on both servers, each
                    covering the same topics. One <code>inout</code> bridge carries both
                    directions by itself, so the second one only moves the same message a second
                    way; remove it. The subscribers on the publishing server still see one copy,
                    which is what makes this look like a delivery fault rather than a
                    configuration one. This is explained under <i>Why one bridge, and not one on
                        each server</i> below.
                </li>
            </ul>

            <p><b>Why one bridge, and not one on each server</b></p>

            <p>
                An <code>inout</code> bridge already carries both directions on its own: it
                subscribes on the remote for what comes in, and subscribes locally for what goes
                out. A second bridge, defined on the other server and pointing back, adds nothing
                but a second copy - the message arrives once because this server pulled it, and
                again because the other server pushed it. Both copies are legitimate deliveries,
                so nothing detects or suppresses them: a subscriber simply receives everything
                twice.
            </p>

            <p>
                Define the bridge on one of the two servers only. Which one does not matter.
            </p>

            <p>
                Bridged traffic is not sent back the way it came - a message is marked with the
                node it arrived from, and a bridge subscription is never given a marked message.
                That marking is XMQ's own, so it only covers the bridges XMQ itself makes. When
                the broker at the other end is doing the bridging, what protects you is described
                next.
            </p>

            <h5>More than two servers</h5>

            <p>
                Bridged traffic is not passed on. A message that reached this server over one
                bridge is delivered to its own subscribers, but it is not handed to a second
                bridge - that is the same rule that stops a message going back where it came
                from, and it cannot tell "back" from "onward".
            </p>

            <p>
                So the shapes that look natural do not work. In a chain
                <code>A - B - C</code>, B sees everything from both, but A and C never see each
                other. In a star, the hub sees every spoke and no spoke sees another.
            </p>

            <p>
                Give every pair its own bridge instead. Three servers need three:
                <code>A-B</code>, <code>A-C</code> and <code>B-C</code>, each defined once, on
                either end. Every server then has a direct link to every other, so nothing needs
                relaying - and because nothing is relayed, nothing is duplicated either: each
                message arrives exactly once at each server.
            </p>

            <p>
                The cost is that the number of bridges grows as the square of the number of
                servers: three servers need three bridges, four need six, five need ten. Bridging
                suits a handful of servers, and the arithmetic is what limits it.
            </p>

            <h5>Bridging to a broker that is not XMQ</h5>

            <p>
                A bridge to Mosquitto, EMQX, or anything else speaking MQTT is configured exactly
                as above; only the remote's own settings differ. One of them is worth getting
                right, because it decides whether messages can circulate endlessly between the two
                brokers.
            </p>

            <p>
                <b>Configure the remote's bridge to use MQTT 5.</b> In Mosquitto that is one line
                in its bridge block:
            </p>

            <pre className="userManualCode">{`connection xmq
address xmq-host:1883
topic test/# both 0 "" ""
remote_username user
remote_password secret
bridge_protocol_version mqttv50`}</pre>

            <p>
                MQTT 5 defines two subscription options that exist for precisely this purpose.
                <b>No Local</b> tells the broker not to send a subscriber back what that same
                connection published, which is what stops a message going round; and <b>Retain As
                Published</b> keeps a retained message retained as it crosses. A bridge that
                subscribes with them cannot be echoed to, and XMQ honours both, whoever set them.
                XMQ's own bridges set them too, which is why a bridge between two XMQ servers needs
                nothing configured for this.
            </p>

            <p>
                Mosquitto defaults to <code>mqttv311</code> instead. Such a bridge still connects
                and still carries messages, but the loop protection is absent: MQTT 3.1.1 has no
                subscription options to carry it, and the non-standard "bridge" protocol variant
                Mosquitto tries first - a protocol level with the high bit set - is not something
                XMQ accepts. Mosquitto notices the refusal and reconnects as an ordinary client,
                so the only visible cost is one rejected connection attempt, and the invisible one
                is that XMQ cannot tell that client is a bridge.
            </p>

            <p>
                With a 3.1.1 remote bridge, then, keep the topic patterns from overlapping in both
                directions - carry <code>a/#</code> one way and <code>b/#</code> the other, rather
                than <code>#</code> both ways - or drive the link entirely from the XMQ side, where
                the origin marking applies.
            </p>

            <h5>Server limits</h5>

            <p>
                The <b>Server Limits</b> page holds the thread counts and the per-session bounds.
                The rule for threads is short: XMQ pins its threads to physical cores, and no thread
                group should be larger than the host's physical core count. The groups share those
                cores, so count the send threads, the receive threads, the persistence threads and
                the per-listener threads together rather than each on its own. Adding threads past
                that buys contention, not throughput.
            </p>
            </section>
    </DocumentationPage>;
}
