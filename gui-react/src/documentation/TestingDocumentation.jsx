import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function TestingDocumentation() {
    return <DocumentationPage page="testing">
            <section>
            <h4 id="testing-suite">MQTT testing suite</h4>

            <p>
                <code>xmq_scn</code> runs load-test scenarios described by JSON files. It is
                broker-neutral: it speaks plain MQTT and can be pointed at Mosquitto, EMQX, NanoMQ
                or anything else, which is what makes its numbers comparable across brokers.
            </p>

            <p>
                Scenarios ship in <code>/usr/local/share/xmq</code>. List them with:
            </p>

            <pre className="userManualCode">{`xmq_scn --list-scenarios`}</pre>

            <p>
                A scenario file names the client groups, their counts, the topics, and the pacing.
                The shipped set covers connection ramps, fan-in, fan-out, and point-to-point, at
                sizes from 10,000 to 1,000,000 connections. Writing a new one is a matter of copying
                the closest and changing the numbers.
            </p>

            <pre className="userManualCode">{`xmq_scn -s Point-To-Point-50K-50K-50K-50K.json --progress
xmq_scn -s 1M-Connections-5K-rate.json -h broker -p 1883`}</pre>

            <table className="userManualTable">
                <thead>
                <tr>
                    <th>Option</th>
                    <th>Meaning</th>
                </tr>
                </thead>
                <tbody>
                <tr>
                    <td><code>-s</code>, <code>--scenario</code></td>
                    <td>Scenario file. A relative path is looked up in the current directory first,
                        then in the installed scenario directory
                    </td>
                </tr>
                <tr>
                    <td><code>-m</code>, <code>--payload-size</code></td>
                    <td>Message size, overriding the scenario</td>
                </tr>
                <tr>
                    <td><code>-r</code>, <code>--publish-rate</code></td>
                    <td>Messages per second per publisher. 0 means unpaced: as fast as possible</td>
                </tr>
                <tr>
                    <td><code>-d</code>, <code>--duration</code></td>
                    <td>How long to run</td>
                </tr>
                <tr>
                    <td><code>-q</code>, <code>--qos</code></td>
                    <td>Quality of service to publish with</td>
                </tr>
                <tr>
                    <td><code>--max-inflight</code></td>
                    <td>Cap on a publisher's un-acknowledged QoS 1 backlog, so it throttles against
                        its own round-trip time rather than publishing blindly at the configured rate
                    </td>
                </tr>
                <tr>
                    <td><code>--id-prefix</code></td>
                    <td>Client id prefix, so several client hosts don't collide</td>
                </tr>
                <tr>
                    <td><code>--progress</code></td>
                    <td>Show a progress bar</td>
                </tr>
                </tbody>
            </table>

            <p>
                Reaching the larger sizes needs the tuning above on both hosts, and enough source
                addresses on the client: a connection is identified by its source address and port,
                so one address runs out of ports long before a million connections. The tested
                1,000,000-connection runs used 30 client addresses, about 33,000 connections each.
                The <code>make_ip_addresses.sh</code> helper in the same directory adds them, and
                <code>remove_ip_addresses.sh</code> takes them away again:
            </p>

            <pre
                className="userManualCode">{`./make_ip_addresses.sh -s <first three octets> -p <prefix> -f <first host octet> -c 29`}</pre>

            <p>
                Scenarios spread their clients across whatever addresses the host has, so the
                <code>--id-prefix</code> option keeps client ids distinct when several client
                machines drive the same broker.
            </p>

            <div className="userManualNote">
                Run the load generator on a different machine from the broker. Sharing a host means
                measuring the two competing for the same cores, which flatters neither.
            </div>
            </section>
            <section>
            <h4 id="testing-setup">Testing your setup</h4>

            <p>
                Three small utilities are enough to prove an installation works. All of them take
                <code>-h</code> host, <code>-p</code> port, <code>-u</code> username,
                <code>--password</code>, and <code>--version</code>.
            </p>

            <h5>Is the server up?</h5>
            <pre className="userManualCode">{`systemctl status xmq_server
tail -f /var/log/xmq/xmq_server.log`}</pre>

            <p>
                The log names each listener it opened at startup, and says which storage it is
                using: a Redis address, or <code>memory</code> when persistence is off or Redis
                could not be reached.
            </p>

            <h5>Subscribe and publish</h5>

            <p>
                In one terminal, subscribe. In another, publish, and watch it arrive:
            </p>

            <pre className="userManualCode">{`xmq_sub -h localhost -p 1883 -u user --password secret -t "test/#" -q 1 -v

xmq_pub -h localhost -p 1883 -u user --password secret -t test/hello -m "it works"`}</pre>

            <p>
                Useful <code>xmq_pub</code> options: <code>-r</code> repeats the message a number of
                times, <code>-g</code> paces it to a rate, <code>-f</code> takes the payload from a
                file, <code>-s</code> reads it from standard input, and <code>-l</code> stamps a
                send timestamp into the payload so latency can be measured at the far end.
            </p>

            <h5>Testing TLS</h5>
            <pre className="userManualCode">{`xmq_sub -h localhost -p 8883 -u user --password secret \\
        -t "test/#" -q 1 -v --cafile /etc/xmq/certs/ca.crt`}</pre>

            <p>
                If this fails while the plain port works, the certificates are the place to look:
                the server log says whether it managed to load them at startup.
            </p>

            <h5>Many connections at once</h5>

            <p>
                <code>xmq_con</code> opens a number of sessions and holds them, which is the quickest
                way to see whether the host limits are in place:
            </p>

            <pre
                className="userManualCode">{`xmq_con -h localhost -p 1883 -u user --password secret -n 10000 --show-counters`}</pre>

            <p>
                If it stops short of the number asked for, the file descriptor limit is the usual
                cause, on whichever host ran out first. Check both, then see the tuning section.
            </p>

            <h5>Checking a bridge</h5>

            <p>
                Subscribe on the remote broker, publish on XMQ, and confirm the message crosses:
            </p>

            <pre className="userManualCode">{`xmq_sub -h remote-broker -p 1883 -t "bridged/#" -q 1 -v

xmq_pub -h localhost -p 1883 -u user --password secret -t bridged/test -m "over the bridge"`}</pre>

            <p>
                Nothing arriving usually means the bridge is disabled, its topic pattern does not
                cover the topic, or its direction excludes it. The server log, with
                <b>Connections</b> at <code>DEBUG</code>, shows the bridge connecting and how many
                topics it subscribed to.
            </p>
            </section>
    </DocumentationPage>;
}
