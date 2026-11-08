import React from "react";
import DocumentationPage from "./DocumentationPage";

export default function PrometheusDocumentation() {
    return <DocumentationPage page="prometheus">
            <section>
            <h4 id="prometheus">Prometheus metrics</h4>

            <p>
                XMQ can expose its counters to Prometheus on a listener of its own. It is off unless
                enabled, because a new listening port is not something a broker should open without
                being asked. The endpoint answers one path, <code>GET /metrics</code>, in the
                Prometheus text exposition format; everything else is a <code>404</code>.
            </p>

            <p>
                It is a listener rather than a path on the configuration interface: a scrape carries
                no administrator session, and the scraper is often a process that has no business
                reaching the interface at all. The figures come from the same counters the Dashboard
                shows, so the exporter adds no work to the delivery path.
            </p>

            <h4 id="prometheus-enable">Enabling it</h4>

            <p>
                Open <b>Configuration → Prometheus</b>, tick <b>Enabled</b>, set the bind address and
                port, and press <b>Save</b>. The listener is replaced at once: no broker restart, and
                no MQTT client is disconnected. The same section can be written in
                <code>xmq_server.conf</code> when the interface is not available:
            </p>

            <pre className="userManualCode">{`"prometheus": {
  "enabled": true,
  "bind_ip": "0.0.0.0",
  "port": 9184,
  "username": "",
  "password": ""
}`}</pre>

            <table className="userManualTable">
                <thead><tr><th>Key</th><th>Meaning</th><th>Default</th></tr></thead>
                <tbody>
                <tr><td><code>enabled</code></td><td>Whether the listener is created.</td><td><code>false</code></td></tr>
                <tr><td><code>bind_ip</code></td><td>Address to accept scrapes on.</td><td><code>0.0.0.0</code></td></tr>
                <tr><td><code>port</code></td><td>Port to accept scrapes on, above 1024.</td><td><code>9184</code></td></tr>
                <tr><td><code>username</code></td><td>HTTP Basic username; empty means no authentication.</td><td>empty</td></tr>
                <tr><td><code>password</code></td><td>HTTP Basic password, stored as a verifier; at least 8 characters.</td><td>empty</td></tr>
                </tbody>
            </table>

            <p>
                The port must not be one of the MQTT, MQTT+SSL or web interface ports: the broker
                refuses it with a sentence naming the other user. If another process already holds
                the address and port, the endpoint does not start and the interface says why; the
                broker itself keeps running.
            </p>

            <h4 id="prometheus-scrape">Scraping it</h4>

            <pre className="userManualCode">{`scrape_configs:
  - job_name: xmq
    metrics_path: /metrics
    static_configs:
      - targets: ["broker-1:9184", "broker-2:9184"]
    # Only when the endpoint has a username and password.
    basic_auth:
      username: metrics
      password: "the same secret entered on the Prometheus page"`}</pre>

            <p>
                The endpoint is per node. A cluster of three nodes answers three series sets, each
                labelled by <code>node</code> in <code>xmq_build_info</code>; aggregate in the query,
                not in the broker. The listener lives for the whole process, not for the MQTT
                server, so a scrape during a stop still answers: <code>xmq_up</code> is <code>0</code>
                and the broker counters are absent rather than stale.
            </p>

            <h4 id="prometheus-auth">Authentication and exposure</h4>

            <p>
                With no username the endpoint is open, which is the default and suits a scraper on a
                private address. With a username and password every scrape must carry HTTP Basic
                credentials; a request without them, or with the wrong ones, is answered with
                <code>401 Unauthorized</code> and a <code>WWW-Authenticate: Basic</code> challenge.
                There is no anonymous fallback once a username is set, and a username without a
                stored password is refused rather than opened.
            </p>

            <p>
                The password is kept as a one-way PBKDF2 verifier, never as text, and is never sent
                back to the page - a password that is set reads as <code>*****</code>, and sending
                that mask back unchanged keeps it. To remove authentication, clear the username.
            </p>

            <p>
                The listener is not encrypted. Basic credentials travel with every scrape, so use a
                private network or put a TLS proxy in front of it. There is one credential pair per
                endpoint, not one per scraper, and no per-metric filtering.
            </p>

            <h4 id="prometheus-metrics">What is exported</h4>

            <p>
                <code>xmq_up</code> is <code>1</code> while MQTT is running and <code>0</code> while
                it is stopped or starting. It is not Prometheus's own <code>up</code>: that one is
                created by Prometheus per target and says whether the scrape arrived, while
                <code>xmq_up</code> says whether the broker behind it is serving.
            </p>

            <table className="userManualTable">
                <thead><tr><th>Metric</th><th>Type</th><th>Meaning</th></tr></thead>
                <tbody>
                <tr><td><code>xmq_build_info</code></td><td>gauge</td><td>Version and node name, as labels.</td></tr>
                <tr><td><code>xmq_up</code></td><td>gauge</td><td>Whether MQTT is being served.</td></tr>
                <tr><td><code>xmq_broker_clients_connected</code></td><td>gauge</td><td>Clients connected now.</td></tr>
                <tr><td><code>xmq_broker_clients_maximum</code></td><td>gauge</td><td>Most clients connected at one time.</td></tr>
                <tr><td><code>xmq_broker_clients_total</code></td><td>counter</td><td>Clients that have connected since the start.</td></tr>
                <tr><td><code>xmq_broker_clients_disconnected_total</code></td><td>counter</td><td>Clients that have disconnected since the start.</td></tr>
                <tr><td><code>xmq_broker_messages_received_total</code></td><td>counter</td><td>MQTT messages received.</td></tr>
                <tr><td><code>xmq_broker_messages_sent_total</code></td><td>counter</td><td>MQTT messages sent.</td></tr>
                <tr><td><code>xmq_broker_publish_received_total</code></td><td>counter</td><td>PUBLISH messages received.</td></tr>
                <tr><td><code>xmq_broker_publish_sent_total</code></td><td>counter</td><td>PUBLISH messages sent.</td></tr>
                <tr><td><code>xmq_broker_publish_dropped_total</code></td><td>counter</td><td>PUBLISH dropped because a session queue was full.</td></tr>
                <tr><td><code>xmq_broker_retained_messages</code></td><td>gauge</td><td>Retained messages held.</td></tr>
                <tr><td><code>xmq_broker_stored_messages</code></td><td>gauge</td><td>Messages held for later delivery.</td></tr>
                <tr><td><code>xmq_broker_bytes_received_total</code></td><td>counter</td><td>Bytes received from clients.</td></tr>
                <tr><td><code>xmq_broker_bytes_sent_total</code></td><td>counter</td><td>Bytes sent to clients.</td></tr>
                <tr><td><code>xmq_broker_subscriptions</code></td><td>gauge</td><td>Subscriptions held.</td></tr>
                <tr><td><code>xmq_broker_cluster_messages_received_total</code></td><td>counter</td><td>Publications received from other cluster nodes.</td></tr>
                <tr><td><code>xmq_broker_queue_receive</code></td><td>gauge</td><td>Messages waiting in the receive queues.</td></tr>
                <tr><td><code>xmq_broker_queue_send</code></td><td>gauge</td><td>Messages waiting in the send queues.</td></tr>
                <tr><td><code>xmq_broker_queue_delivery</code></td><td>gauge</td><td>Messages waiting in the delivery queues.</td></tr>
                <tr><td><code>xmq_broker_uptime_seconds</code></td><td>gauge</td><td>Seconds since the broker started serving.</td></tr>
                <tr><td><code>xmq_host_cpu_percent</code></td><td>gauge</td><td>Whole-machine CPU use, percent of all cores.</td></tr>
                <tr><td><code>xmq_host_cpu_cores</code></td><td>gauge</td><td>Hardware threads on this machine.</td></tr>
                <tr><td><code>xmq_host_memory_used_bytes</code></td><td>gauge</td><td>Memory in use, excluding reclaimable cache.</td></tr>
                <tr><td><code>xmq_host_memory_available_bytes</code></td><td>gauge</td><td>Memory available to start new work.</td></tr>
                <tr><td><code>xmq_host_memory_total_bytes</code></td><td>gauge</td><td>Physical memory installed.</td></tr>
                <tr><td><code>xmq_process_cpu_percent</code></td><td>gauge</td><td>This process's CPU use, percent of one core.</td></tr>
                <tr><td><code>xmq_process_resident_memory_bytes</code></td><td>gauge</td><td>Resident memory of this process.</td></tr>
                <tr><td><code>xmq_process_uptime_seconds</code></td><td>gauge</td><td>Seconds since this process started.</td></tr>
                </tbody>
            </table>

            <h4 id="prometheus-limits">What it does not do</h4>

            <ul>
                <li>No OpenTelemetry export, traces or logs: only the text exposition format over HTTP.</li>
                <li>No TLS and no per-metric filtering. HTTP Basic authentication is available; TLS is expected to be provided by a proxy or a private network.</li>
                <li>No cluster-wide aggregation: each node answers for itself.</li>
                <li>No historical samples. Prometheus is expected to keep them.</li>
            </ul>

            <p>
                Alerts worth having: <code>up == 0</code> means the exporter or the network is gone,
                while <code>up == 1 and xmq_up == 0</code> means the process is alive but MQTT is
                stopped - the dashboard and the log say why.
            </p>
            </section>
    </DocumentationPage>;
}
