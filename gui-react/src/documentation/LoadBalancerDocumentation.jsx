import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function LoadBalancerDocumentation() {
    return <DocumentationPage page="load-balancer">
        <section>
            <h4 id="load-balancer">A Load Balancer in Front of a Cluster</h4>

            <p>
                A client may connect to any node of an XMQ cluster. The node it reaches becomes the
                owner of its session, taking the session over from the node that served it before -
                subscriptions, queued messages and all. That is what makes a load balancer in front
                of the cluster simple: it distributes new connections and nothing else. No session
                affinity, no sticky sessions and no shared session table are needed.
            </p>

            <p>
                The balancer sees ordinary MQTT connections. It must pass TCP through unchanged,
                health-check every node, and stop sending new connections to a node that does not
                answer. TLS is passed through as well: the nodes terminate it, not the balancer.
            </p>

            <h5>What the balancer has to do</h5>

            <ul>
                <li><b>Spread new connections over the nodes.</b> Least-connections suits MQTT,
                    whose connections are long-lived: round-robin sends the next client to a node
                    without regard to how many it already serves.</li>
                <li><b>Check each node's MQTT port</b> - <code>1883</code>, and <code>8883</code>
                    for TLS - and take a node that stops answering out of the balancing until it
                    answers again.</li>
                <li><b>Use no affinity.</b> A client that reconnects may land on another node; its
                    session follows it. Source-address hashing would only concentrate clients and
                    make a node's failure hurt more.</li>
                <li><b>Leave MQTT alone.</b> No protocol parsing, no TLS termination and no idle
                    timeout shorter than the clients' keep-alive.</li>
            </ul>

            <h5>IPVS with keepalived</h5>

            <p>
                IPVS is in the Linux kernel: a virtual service with one real server per node.
                It keeps no buffer per connection,
                so a connection costs the kernel a few hundred bytes. keepalived writes the rules
                and checks the real servers; <code>ipvsadm</code> shows them.
            </p>

            <p>
                The test cluster writes a working configuration of its own:
                <code>test_cluster.sh lb up</code> builds it from the node list, one real server
                per node, and applies it. This is the shape it writes, with two nodes:
            </p>

            <pre className="userManualCode">{`virtual_server 10.0.0.10 1883 {
    delay_loop 2
    lb_algo lc
    lb_kind NAT
    protocol TCP

    real_server 10.0.0.11 1883 {
        weight 1
        TCP_CHECK { connect_timeout 2 }
    }
    real_server 10.0.0.12 1883 {
        weight 1
        TCP_CHECK { connect_timeout 2 }
    }
}

virtual_server 10.0.0.10 8883 {
    delay_loop 2
    lb_algo lc
    lb_kind NAT
    protocol TCP

    real_server 10.0.0.11 8883 {
        weight 1
        TCP_CHECK { connect_timeout 2 }
    }
    real_server 10.0.0.12 8883 {
        weight 1
        TCP_CHECK { connect_timeout 2 }
    }
}`}</pre>

            <p>
                <code>lb_kind NAT</code> rewrites the destination address, so the nodes need no
                configuration of their own - their replies have to return through the balancer,
                which means the balancer is their gateway for that traffic. <code>DR</code>
                (direct routing) avoids that by returning replies straight from the node; it needs
                the VIP configured on each node's loopback and is the better choice at high
                throughput. In both cases the clients connect to the VIP, not to a node's name.
            </p>

            <pre className="userManualCode">{`# See the balancing and the per-server counters:
sudo ipvsadm -Ln --stats

# The test cluster's own balancer, on the machine running test_cluster.sh:
test_cluster.sh lb up
test_cluster.sh lb status
test_cluster.sh lb down`}</pre>

            <p>
                <code>test_cluster.sh lb up</code> is the checked example: it chooses the VIP as the
                machine's own address and serves <code>1883</code> and <code>8883</code>. A client
                on that machine reaches it by address - the machine's name may resolve to
                <code>127.0.1.1</code>, which is not the balancer.
            </p>

            <h5>HAProxy</h5>

            <p>
                HAProxy is the simpler alternative when the kernel's IPVS is not wanted, or when
                the balancer is a separate host that already runs it. It needs <code>mode tcp</code>:
                terminating MQTT or TLS would put the balancer in the middle of a protocol it has
                no reason to understand, and would hide the nodes' certificates from the clients.
            </p>

            <p>
                Its default buffers are two 16 KB buffers per connection, about 35 KB before any
                MQTT state. Cutting <code>tune.bufsize</code> down is the difference between a
                balancer that costs a few hundred bytes per connection and one that costs
                tens of kilobytes per connection:
            </p>

            <pre className="userManualCode">{`global
    tune.bufsize 4096

defaults
    mode tcp
    timeout connect 5s
    timeout client  1h
    timeout server  1h

frontend mqtt
    bind *:1883
    default_backend xmq

frontend mqtts
    bind *:8883
    default_backend xmq_tls

backend xmq
    balance leastconn
    server node-a node-a:1883 check
    server node-b node-b:1883 check

backend xmq_tls
    balance leastconn
    server node-a node-a:8883 check
    server node-b node-b:8883 check`}</pre>

            <p>
                A 4 KB buffer is enough for the MQTT packets of ordinary clients; raise it if the
                installation carries large payloads. The <code>timeout server</code> and
                <code>timeout client</code> have to outlast the keep-alive interval a client
                negotiated, or the balancer closes connections the broker considers alive.
            </p>

            <h5>Sessions, failover and TLS</h5>

            <p>
                When a node goes away, its clients reconnect through the balancer and land on the
                nodes that remain. Each session is then taken over from the lost node, the same
                way a client's own reconnect moves it. Nothing on the balancer records which node
                served a client, and nothing has to: Redis records the owner, and the next node the
                client reaches takes the session from it.
            </p>

            <p>
                A node that has lost Redis no longer serves clients - it refuses them as "server
                unavailable" - and a port check alone will not see that, because the listener still
                answers. Where that distinction matters, check the node with an MQTT CONNECT rather
                than a TCP connect, or take the failed node out of the balancer by hand while it
                recovers.
            </p>

            <p>
                TLS passes through the balancer to the node. A client then verifies the certificate
                of the node it reached, not of the balancer. If clients connect to the VIP by name
                and verify the host name, the nodes' certificates must carry that name as a
                subject alternative name; a certificate issued for a node's own host will fail that
                check even though the connection is correctly routed. Terminating TLS at the
                balancer avoids this, but then the balancer holds the key and the cluster's
                end-to-end encryption stops at the balancer.
            </p>

            <h5>Tested configuration</h5>

            <p>
                <code>LoadTest/Cluster/clients-through-a-load-balancer.json</code> runs the cluster
                test with every client behind <code>test_cluster.sh lb up</code>. In its timeline
                one node is stopped under load: keepalived removes it from the balancing, its
                clients reconnect through the balancer to the other nodes, and when it serves again
                it returns to the real-server list. The same run checks the second step with
                <code>ipvsadm</code>.
            </p>
        </section>
    </DocumentationPage>;
}
