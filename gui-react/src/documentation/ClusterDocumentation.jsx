import React from "react";
import {Link} from "react-router-dom";
import DocumentationPage from "./DocumentationPage";

export default function ClusterDocumentation() {
    return <DocumentationPage page="cluster">
            <section>
            <h4 id="cluster-setup">Setting up a Cluster</h4>

            <p><b>Available since XMQ 1.0.0.</b></p>

            <p>
                A cluster is a set of XMQ nodes that share one storage (Redis) and one accounts
                database (PostgreSQL or MySQL), so that a client connecting to any node sees the
                same sessions, the same accounts and the same retained messages. A node joins a
                cluster when an administrator selects its registered configuration and presses
                Join cluster. The nodes already share their administrator and cluster accounts;
                a second confirmation on the target is not required. The selected node is checked
                over TLS and configured, then its MQTT service is restarted within the same process.
                The web interface remains available.
            </p>

            <p>
                The steps below set up two nodes, <code>node-a</code> and <code>node-b</code>, on
                two hosts. Everything is done from the configuration interface: a cluster is
                declared by an administrator there, not by editing a file on each node.
            </p>

            <h5>Prerequisites</h5>

            <ul>
                <li><b>XMQ 1.0.0 or newer, installed on both nodes</b> - see
                    <Link to="/xmq_doc_install#install">Download and install binaries</Link>.</li>
                <li><b>PostgreSQL</b> (or MySQL) reachable from both nodes - the shared
                    configuration and the shared accounts. The table <code>xmq_node</code> holds
                    what an administrator edits: a row is a node, a column is a setting. A node
                    that cannot reach the database does not start.</li>
                <li><b>Redis</b> reachable from both nodes - membership, leases, sessions and
                    retained messages. Any database number works; the cluster keeps its keys
                    under <code>cluster:</code> and its sessions under
                    <code>session_&lt;client_id&gt;</code>.</li>
                <li><b>Nothing else.</b> The TLS certificates the nodes link to one another with
                    are created by the nodes themselves, one per node, during the initial setup -
                    there is no certificate to obtain or copy before starting.</li>
            </ul>

            <p>
                Before step 1, create the shared User Database and its SQL user
                on the PostgreSQL host. This example uses the default database name
                <code> xmq</code>; both nodes will use this same database:
            </p>

            <pre className="userManualCode">{`sudo -u postgres psql -c "CREATE ROLE xmq LOGIN PASSWORD 'yourpassword'"
sudo -u postgres createdb -O xmq xmq

# Verify:
psql "postgresql://xmq:yourpassword@db-host/xmq" -c '\\dt'
# (no tables yet - Initial Setup creates them)`}</pre>

            <h5>1. Install and set up node A</h5>

            <p>
                Install the package on the first host, start the server, and open the
                configuration interface. A server that has never been set up answers on the
                loopback address only, and its <code>admin</code> account has no password yet:
                the interface signs the administrator in by itself, and the <b>Initial Setup</b>
                page opens.
            </p>

            <pre className="userManualCode">{`# On node-a, after installing the package:
sudo systemctl enable --now xmq_server

# The interface is on the loopback address until a password is set. From the
# host itself, or through a tunnel:  ssh -L 18883:127.0.0.1:18883 node-a
#   -> https://127.0.0.1:18883`}</pre>

            <p>
                The first visit is met with the browser's <b>Your connection is not private</b>
                warning (Chrome: <code>NET::ERR_CERT_AUTHORITY_INVALID</code>). That is expected
                and not a fault: the interface serves HTTPS with a certificate it issued itself,
                and no browser trusts a certificate it has never seen. Press <b>Advanced</b> and
                then <b>Proceed to 127.0.0.1 (unsafe)</b> to carry on - the warning is the
                browser telling you it cannot verify who is at the other end, which is true of
                every self-signed certificate, this installation's included.
            </p>

            <p>
                After the setup page is submitted the warning appears once more: that is the
                node's own certificate, issued by the setup a moment ago, which the browser has
                not seen either. Accept it the same way. The certificate is worth replacing with
                one a certificate authority has signed once the installation stops being a test
                one - the <b>SSL Keys</b> page installs one. The nodes register their certificates
                in the shared User Database for peer verification (step 3).
            </p>

            <p>
                Fill in the <b>Initial Setup</b> page from top to bottom:
            </p>

            <ol>
                <li><b>Administrator</b>: the username is <code>admin</code>. Enter its new
                    password in <b>Password</b> and repeat it in <b>Repeat password</b>.</li>
                <li><b>This Server</b>: set <b>Node name</b> to <code>node-a</code>.
                    Set <b>Node address</b> to the DNS name or IP address the other nodes use
                    to reach this host, without a scheme or port.</li>
                <li><b>Ports</b>: check <b>MQTT port</b> (<code>1883</code>),
                    <b> MQTT+SSL port</b> (<code>8883</code>), and <b>Web interface port</b>
                    (<code>18883</code>). Change them if needed; all three must differ.</li>
                <li><b>Redis</b>: enter the Redis hostname in <b>Redis host</b> and its port
                    in <b>port</b> (default <code>6379</code>). Use the Redis server that all
                    nodes will share.</li>
                <li><b>Accounts Database</b>: the <b>User Database URI</b> row contains
                    a server-type selector, <b>host</b>, and <b>database</b> (default
                    <code> xmq</code>). Select <b>PostgreSQL</b> (or <b>MySQL</b>), enter the
                    shared database hostname and database name, then fill in <b>Database user</b>
                    and <b>Database password</b> below.</li>
            </ol>

            <p>
                The database <b>host</b> may include a port. <b>Database user</b> and
                <b> Database password</b> are the SQL credentials, separate from the administrator
                password above. The interface builds the URI from these fields. If the database
                already holds an <code>admin</code> account, enter that account's password in
                the <b>Administrator</b> section. Enter the shared cluster password in the
                <b> Cluster account</b> section. Configuring the shared database and Redis
                registers the node for discovery; it does not enable cluster mode or join a cluster.
            </p>

            <p>
                Initial Setup on node A creates the <code>admin</code> and internal
                <code> cluster</code> accounts in the shared user database. The
                <code> cluster</code> account is created once for the entire cluster: every node
                uses that same account. Enter the same cluster password during Initial Setup on
                every node; an existing account is checked before setup changes are applied.
            </p>

            <p>
                Press <b>Submit and restart</b>. After the certificate warning described above,
                sign in as <code>admin</code> with the password you just set.
            </p>

            <p>
                This page is also where the node's certificate is created: a self-signed
                certificate of the node's own, made out to the node host entered above, is
                written to <code>/etc/xmq/certs/node.crt</code> together with its key, and both
                the configuration interface and the MQTT+SSL listener are pointed at that pair.
                One certificate per node, never copied between them - it is the node's identity
                on the cluster's links.
            </p>

            <p>
                <b>Verify:</b> the interface now answers on the node host, with the new
                certificate:
            </p>

            <pre className="userManualCode">{`openssl s_client -connect node-a:18883 </dev/null 2>/dev/null \
    | openssl x509 -noout -subject -dates
# subject= ... CN=node-a   (the node host given on the setup page)`}</pre>

            <h5>2. Install and set up node B</h5>

            <p>
                The same on the second host: install, start, open the interface through the
                loopback or a tunnel, and fill the <b>Initial Setup</b> page with node B's own
                <b>Node name</b> (<code>node-b</code>), <b>Node address</b>, and ports.
                Its certificate is created the same way, made out to <b>its</b> address.
            </p>

            <p>Every node in the cluster must follow these rules:</p>

            <ul>
                <li><b>Node name must be unique across the entire cluster.</b> Node B must use
                    <code> node-b</code>, not node A's <code>node-a</code>. Give each additional
                    node a different name.</li>
                <li><b>User Database URI must be identical on every node.</b> Use the same
                    database server type, <b>host</b>, <b>database</b>, <b>Database user</b>,
                    and <b>Database password</b> as node A, so that the assembled URI points
                    every node at the same shared accounts and cluster configuration.</li>
                <li><b>Redis host must be identical on every node.</b> Use the same Redis
                    hostname and port as node A. If a Redis database number is specified later
                    in a URI, that number must also be the same on every node.</li>
            </ul>

            <p>
                Enter these matching values during Initial Setup, and use the administrator
                password and cluster password set on node A. Node B uses the accounts already created in the shared
                database in step 1, including the same <code>cluster</code> account.
            </p>

            <p>
                <b>Verify:</b> the same command against node B returns a certificate naming
                <code>node-b</code>. The two nodes now have two different certificates - check
                with <code>openssl x509 -noout -fingerprint -in /etc/xmq/certs/node.crt</code> on
                each, and see that the two differ.
            </p>

            <h5>3. Verify node and certificate registration</h5>

            <p>
                Every node configured with the shared <b>User Database URI</b> registers its
                own row in <code>xmq_node</code>, including its name, TLS address, and PEM
                certificate in the <code>certificate</code> column. At startup, a node publishes
                its current certificate and reads the other nodes' certificates from the same
                table into its local trusted peer files. A reissued certificate is updated in
                the shared database on startup.
            </p>

            <p>
                Cluster links and join requests use these registered certificates for TLS peer
                verification. There is no manual certificate exchange or separate
                <b> Trusted Nodes</b> entry to create for each peer.
            </p>

            <pre className="userManualCode">{`psql "postgresql://xmq:yourpassword@db-host/xmq" \
    -c "SELECT name, host_port, length(certificate) AS certificate_length FROM xmq_node ORDER BY name"
# node-a | node-a:8883 | (non-zero length)
# node-b | node-b:8883 | (non-zero length)`}</pre>

            <p>
                <b>Verify:</b> both rows have a non-empty certificate, and the peer certificates
                are on disk. If node A started before node B registered, restart node A to load
                node B's certificate from the shared database.
            </p>

            <pre className="userManualCode">{`ls /etc/xmq/certs/peers/
# node-b.crt   (on node-a)   /   node-a.crt   (on node-b)`}</pre>

            <h5>4. Join node B to the cluster</h5>
            <p>
                On node A select <b>Cluster</b> mode on the <b>Dashboard</b> and confirm the
                shared accounts database and Redis storage. This creates the first member.
                Keep node B in <b>Standalone</b> until you explicitly join it.
            </p>
            <p>
                On node A's <b>Cluster Configuration</b> page select the registered
                <code> node-b</code> row and press <b>Join cluster</b>. Confirm the target and
                MQTT restart. The cluster uses the password configured during Initial Setup;
                there is no additional password dialog or Accept step on node B.
            </p>
            <p>
                Before changing anything, the system verifies TLS, the selected UUID, server
                version, shared database, and Redis membership and capacity. Node B must use
                the same shared User Database URI. Its clients disconnect while MQTT restarts;
                local standalone sessions and queued messages are not migrated into the cluster.
            </p>
            <h5>5. Verify the join</h5>
            <p>
                Acknowledgement means node B accepted the configuration and is restarting MQTT;
                its row initially says <b>Joining</b>. The page refreshes automatically after the transition: both
                rows should say <b>Clustered</b>. A failed startup is reported as <b>Failed</b>,
                with the reason, and the last-good configuration is retained for rollback.
                The process and its web interface keep running, including when started manually.
                If MQTT cannot start with the new role, the node tries its previous configuration
                without restarting the process; the failure remains visible in the node list.
            </p>
            <pre className="userManualCode">{`redis-cli -n 0 zrange cluster:members 0 -1
# two GUIDs after node B has restarted and joined`}</pre>

            <h5>6. Check that the cluster carries traffic</h5>

            <p>
                Publish a retained message through one node and read it through the other: a
                retained message is synchronised across the cluster, so a subscriber on node B
                receives what was published on node A.
            </p>

            <pre className="userManualCode">{`# Through node A:
xmq_pub -h node-a -p 1883 -u admin --password admin -t cluster/test \
    -m "hello from node-a" -q 1 -r

# Through node B:
xmq_sub -h node-b -p 1883 -u admin --password admin -t cluster/test -C 1 -W 5`}</pre>

            <p>
                If the message arrives, the cluster is whole. A client that then connects to either
                node for an ordinary session is served by that node, and the other nodes forward
                to it whatever its subscribers are subscribed to.
            </p>
            <h5>7. Leave the cluster without deleting the server configuration</h5>
            <p>
                Select the node in the list and press <b>Leave cluster</b> in the action panel on the right.
                Confirm its name. The selected server drains its
                cluster traffic, removes its membership, preserves its configuration in
                <code> xmq_node</code>, and restarts MQTT as <b>Standalone</b> within the same process. The web interface stays
                available. Other cluster members
                continue running. Returning to this cluster requires an explicit Join cluster action again.
            </p>
            <h5>8. Remove an obsolete server configuration</h5>
            <p>
                <b>Remove node</b> deletes configuration, not membership. First leave the cluster.
                If the server still uses this database, select it and press <b>Retire configuration</b>: MQTT stops and the database records
                that this configuration is no
                longer to be used. An administrator can then remove the retired row.
            </p>
            <p>
                If a server has been lost or its database URI was changed manually, UUID and
                Offline status cannot prove that the old configuration is unused. Use
                <b> Force remove node</b> only after confirming this explicitly. A removal marker
                is retained in this database so a server returning with the old UUID cannot
                silently recreate its configuration. Other databases remain independent.
            </p>
            </section>
    </DocumentationPage>;
}
